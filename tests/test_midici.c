#include "test.h"
#include "midici/midici.h"
#include "midici/ci_queue.h"
#include "ump/ump.h"
#include "stubs/stubs.h"

static uint8_t h_buf[600];
static uint16_t h_len;
static uint8_t h_group;
static int h_calls;

static void handler(uint8_t group, const uint8_t *payload, uint16_t len)
{
	h_calls++;
	h_group = group;
	h_len = len;
	if (len <= sizeof h_buf)
		memcpy(h_buf, payload, len);
}

static void setup(void)
{
	stubs_reset();
	ump_rx_reset();
	ump_tx_reset();
	midici_set_handler(handler);
	h_calls = 0;
	h_len = 0;
}

/* a MIDI-CI style payload: 7E 7F 0D then counting bytes */
static uint16_t make_payload(uint8_t *p, uint16_t len)
{
	p[0] = 0x7E;
	p[1] = 0x7F;
	p[2] = 0x0D;
	for (uint16_t i = 3; i < len; i++)
		p[i] = (uint8_t)(i & 0x7F);
	return len;
}

static void push_sysex(uint8_t group, const uint8_t *payload, uint16_t len)
{
	uint32_t w[2 * 100];
	uint16_t n = ump_sysex7_pack(w, 2 * 100, group, payload, len);
	CHECK(n > 0);
	/* one USB packet (8 SysEx7 packets) at a time, the main loop drains between */
	for (uint16_t i = 0; i < n; i += 16) {
		ump_rx_push_words(w + i, (uint16_t)(n - i < 16 ? n - i : 16));
		ump_rx_poll();
	}
}

static void test_queue_fifo_and_overflow(void)
{
	CI_QUEUE_DEFINE(q, 4);
	uint32_t a[2], b[2] = {0, 0};
	ci_queue_reset(&q);
	for (uint32_t i = 0; i < 4; i++) {
		a[0] = i;
		a[1] = ~i;
		CHECK_EQ(ci_queue_push(&q, a), 0);
	}
	CHECK_EQ(ci_queue_push(&q, a), -1);
	CHECK_EQ(q.overflows, 1);
	CHECK_EQ(ci_queue_free(&q), 0);
	for (uint32_t i = 0; i < 4; i++) {
		CHECK_EQ(ci_queue_pop(&q, b), 0);
		CHECK_EQ(b[0], i);
		CHECK_EQ(b[1], ~i);
	}
	CHECK_EQ(ci_queue_pop(&q, b), -1);
	/* wraps cleanly */
	for (uint32_t i = 0; i < 10; i++) {
		a[0] = 100 + i;
		CHECK_EQ(ci_queue_push(&q, a), 0);
		CHECK_EQ(ci_queue_pop(&q, b), 0);
		CHECK_EQ(b[0], 100 + i);
	}
}

static void test_isr_diverts_mt3_only(void)
{
	/* note on (MT4), then a SysEx7 packet, then another note on */
	uint32_t w[] = {0x40903C00u, 0xFFFF0000u, 0x30037E7Fu, 0x0D000000u, 0x41903E00u, 0x80000000u};
	setup();
	ump_rx_push_words(w, 6);
	/* the two notes took 4 ring words; the SysEx7 packet none */
	CHECK_EQ(ump_rx_free_words(), UMP_RX_WORDS - 4);
	CHECK_EQ(CI_RX_QUEUE_PACKETS - midici_rx_free_packets(), 1);
}

static void test_isr_framing_across_pushes(void)
{
	uint32_t a = 0x30037E7Fu, b = 0x0D000000u;
	setup();
	ump_rx_push_words(&a, 1);
	CHECK_EQ(midici_rx_free_packets(), CI_RX_QUEUE_PACKETS);
	ump_rx_push_words(&b, 1);
	CHECK_EQ(midici_rx_free_packets(), CI_RX_QUEUE_PACKETS - 1);
	ump_rx_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_len, 3);
	CHECK_EQ(h_buf[0], 0x7E);
	CHECK_EQ(h_buf[2], 0x0D);
}

static void test_ci_multi_packet_message(void)
{
	uint8_t p[40];
	uint32_t before;
	setup();
	before = midici_messages_received();
	make_payload(p, 40);
	push_sysex(3, p, 40); /* group 3: not a standalone group, still accepted */
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_group, 3);
	CHECK_EQ(h_len, 40);
	CHECK_MEM(h_buf, p, 40);
	CHECK_EQ(stub_sysex_calls, 0); /* not forwarded to the app */
	CHECK_EQ(midici_messages_received(), before + 1);
}

static void test_ci_512_byte_limit(void)
{
	uint8_t p[600];
	setup();
	make_payload(p, 512);
	push_sysex(0, p, 512);
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_len, 512);
	CHECK_MEM(h_buf, p, 512);

	setup();
	make_payload(p, 513);
	push_sysex(0, p, 513);
	CHECK_EQ(h_calls, 0);
	CHECK(midici_reassembly_drops() >= 1);
}

static void test_non_ci_sysex_still_reaches_app(void)
{
	uint8_t p[] = {0x01, 0x02, 0x03};
	setup();
	push_sysex(1, p, 3);
	ump_rx_poll();
	CHECK_EQ(h_calls, 0);
	CHECK_EQ(stub_sysex_calls, 1);
	CHECK_EQ(stub_sysex_port, 1);
	CHECK_EQ(stub_sysex_len, 5);
}

static void test_ci_and_other_interleave_on_different_groups(void)
{
	uint8_t ci[20], other[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	uint32_t wc[2 * 4], wo[2 * 2];
	uint16_t nc, no;
	setup();
	make_payload(ci, 20);
	nc = ump_sysex7_pack(wc, 8, 0, ci, 20);	/* 4 packets */
	no = ump_sysex7_pack(wo, 4, 1, other, 10);	/* 2 packets */
	CHECK_EQ(nc, 8);
	CHECK_EQ(no, 4);
	ump_rx_push_words(wc, 4);	/* ci start + continue */
	ump_rx_push_words(wo, 2);	/* other start */
	ump_rx_push_words(wc + 4, 4);	/* ci end */
	ump_rx_push_words(wo + 2, 2);	/* other end */
	ump_rx_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_len, 20);
	CHECK_EQ(stub_sysex_calls, 1);
	CHECK_EQ(stub_sysex_port, 1);
	CHECK_EQ(stub_sysex_len, 12);
}

static void test_reset_drops_partial_ci(void)
{
	uint8_t p[20];
	uint32_t w[8];
	setup();
	make_payload(p, 20);
	ump_sysex7_pack(w, 8, 0, p, 20);
	ump_rx_push_words(w, 4);	/* start + continue */
	ump_rx_poll();
	ump_rx_reset();			/* SET_INTERFACE */
	ump_rx_push_words(w + 4, 4);	/* orphan continue + end */
	ump_rx_poll();
	CHECK_EQ(h_calls, 0);
}

static void test_muid_range_and_stability(void)
{
	uint32_t m;
	CHECK(!midici_muid_valid(0));
	CHECK(!midici_muid_valid(0x0FFFFF00u));
	CHECK(!midici_muid_valid(0x0FFFFFFFu));
	CHECK(!midici_muid_valid(0x10000000u));
	CHECK(midici_muid_valid(1));
	CHECK(midici_muid_valid(0x0FFFFEFFu));

	stubs_reset();
	/* 0, reserved, broadcast and 28-bit-masked 0 are rejected; 0xF0000005 -> 5 */
	stub_entropy_seq[0] = 0;
	stub_entropy_seq[1] = 0x0FFFFF00u;
	stub_entropy_seq[2] = 0x0FFFFFFFu;
	stub_entropy_seq[3] = 0xF0000000u;
	stub_entropy_seq[4] = 0xF0000005u;
	stub_entropy_n = 5;
	m = midici_muid_regenerate();
	CHECK_EQ(m, 5);
	CHECK_EQ(midici_muid(), 5);	/* stable until regenerated */
	CHECK_EQ(midici_muid(), 5);
	stub_entropy_n = 0;
	m = midici_muid_regenerate();
	CHECK(midici_muid_valid(m));
	CHECK(m != 5);
}

static void test_muid_fallback_when_entropy_stuck(void)
{
	uint32_t m;
	stubs_reset();
	for (int i = 0; i < 16; i++)
		stub_entropy_seq[i] = 0;
	stub_entropy_n = 16;
	m = midici_muid_regenerate();	/* all 16 draws invalid: deterministic fallback */
	CHECK_EQ(m, 1);
	CHECK(midici_muid_valid(m));
}

static void test_tx_drain_bounded_and_ordered(void)
{
	uint8_t p[60];
	uint8_t pkt[64];
	uint8_t n;
	setup();
	make_payload(p, 60);	/* 10 packets */
	CHECK_EQ(midici_send_sysex(2, p, 60), 0);
	CHECK_EQ(midici_tx_drain(CI_TX_DRAIN_MAX), CI_TX_DRAIN_MAX);
	n = ump_tx_fill_packet(pkt);
	CHECK_EQ(n, 8 * CI_TX_DRAIN_MAX);
	/* first packet: MT 3, group 2, start, 6 bytes; little-endian words */
	CHECK_EQ(pkt[3], 0x32);
	CHECK_EQ(pkt[2], 0x16);
	CHECK_EQ(pkt[1], 0x7E);
	CHECK_EQ(pkt[0], 0x7F);
	CHECK_EQ(midici_tx_drain(100), 6);	/* the other six, no more */
	CHECK_EQ(midici_tx_drain(100), 0);
}

static void test_tx_send_all_or_nothing(void)
{
	uint8_t p[120];
	int sent = 0;
	setup();
	make_payload(p, 120);	/* 20 packets */
	while (midici_send_sysex(0, p, 120) == 0)
		sent++;
	CHECK_EQ(sent, CI_TX_QUEUE_PACKETS / 20);	/* 6 whole messages, never a partial one */
	CHECK_EQ(midici_tx_overflows(), 1);
	/* F0..F7 framing is accepted too */
	{
		uint8_t f[5] = {0xF0, 0x7E, 0x7F, 0x0D, 0xF7};
		CHECK_EQ(midici_send_sysex(0, f, 5), 0);	/* 8 packets free: 128 - 120 */
	}
}

static void test_tx_drain_stops_when_ring_full(void)
{
	uint8_t p[6] = {1, 2, 3, 4, 5, 6};
	uint32_t m[1] = {0x10F80000u};	/* MT 1 system real time, 1 word */
	setup();
	/* fill the control ring so only the stream headroom is left */
	while (ump_tx_push_control_app(m, 1) == 0)
		;
	CHECK_EQ(midici_send_sysex(0, p, 6), 0);
	CHECK_EQ(midici_tx_drain(CI_TX_DRAIN_MAX), 0);	/* nothing lost, stays queued */
	{
		uint8_t pkt[64];
		ump_tx_fill_packet(pkt);	/* frees room */
		ump_tx_fill_packet(pkt);
		ump_tx_fill_packet(pkt);
	}
	CHECK_EQ(midici_tx_drain(CI_TX_DRAIN_MAX), 1);
}

static void test_reply_buffer_is_512(void)
{
	uint16_t cap = 0;
	uint8_t *b = midici_reply_buffer(&cap);
	CHECK(b != 0);
	CHECK_EQ(cap, 512);
	b[0] = 1;
	b[511] = 2;
}

void test_midici_all(void)
{
	RUN(test_queue_fifo_and_overflow);
	RUN(test_isr_diverts_mt3_only);
	RUN(test_isr_framing_across_pushes);
	RUN(test_ci_multi_packet_message);
	RUN(test_ci_512_byte_limit);
	RUN(test_non_ci_sysex_still_reaches_app);
	RUN(test_ci_and_other_interleave_on_different_groups);
	RUN(test_reset_drops_partial_ci);
	RUN(test_muid_range_and_stability);
	RUN(test_muid_fallback_when_entropy_stuck);
	RUN(test_tx_drain_bounded_and_ordered);
	RUN(test_tx_send_all_or_nothing);
	RUN(test_tx_drain_stops_when_ring_full);
	RUN(test_reply_buffer_is_512);
}
