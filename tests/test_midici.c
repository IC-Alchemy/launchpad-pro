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

static void make_nonci_payload(uint8_t *p, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++)
		p[i] = (uint8_t)((i + 1u) & 0x7Fu);
}

static void push_raw_packets(uint32_t (*pkts)[2], uint16_t count)
{
	uint16_t i = 0;
	while (i < count) {
		uint16_t batch = (uint16_t)(count - i < 16u ? count - i : 16u);
		for (uint16_t j = 0; j < batch; j++)
			CHECK_EQ(midici_rx_push(pkts[i + j]), 0);
		midici_poll();
		i = (uint16_t)(i + batch);
	}
}

static void test_packetize_forms_and_sizes(void)
{
	uint8_t p[512];
	uint32_t pkts[86][2];
	uint16_t n;

	make_nonci_payload(p, 512);

	/* Invalid argument & capacity guards */
	CHECK_EQ(midici_packetize_sysex7(0, p, 6, 0, 1), 0);
	CHECK_EQ(midici_packetize_sysex7(0, 0, 1, pkts, 1), 0);
	CHECK_EQ(midici_packetize_sysex7(0, p, 7, pkts, 1), 0);
	CHECK_EQ(midici_packetize_sysex7(0, 0, 0, pkts, 0), 0);

	/* 0 bytes with NULL payload -> 1 kComplete packet, cnt=0, zero-padded */
	pkts[0][0] = 0xFFFFFFFFu;
	pkts[0][1] = 0xFFFFFFFFu;
	CHECK_EQ(midici_packetize_sysex7(5, 0, 0, pkts, 1), 1);
	CHECK_EQ(pkts[0][0], 0x35000000u);
	CHECK_EQ(pkts[0][1], 0u);

	/* 1 byte -> 1 kComplete packet, zero-padded */
	{
		uint8_t b1[1] = {0x41};
		CHECK_EQ(midici_packetize_sysex7(2, b1, 1, pkts, 1), 1);
		CHECK_EQ(pkts[0][0], 0x32014100u);
		CHECK_EQ(pkts[0][1], 0u);
	}

	/* 5 bytes -> 1 kComplete packet, trailing byte zero-padded */
	{
		uint8_t b5[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
		CHECK_EQ(midici_packetize_sysex7(15, b5, 5, pkts, 1), 1);
		CHECK_EQ(pkts[0][0], 0x3F051122u);
		CHECK_EQ(pkts[0][1], 0x33445500u);
	}

	/* 6 bytes -> 1 kComplete packet */
	CHECK_EQ(midici_packetize_sysex7(0, p, 6, pkts, 1), 1);
	CHECK_EQ(pkts[0][0], 0x30060102u);
	CHECK_EQ(pkts[0][1], 0x03040506u);

	/* 7 bytes -> kStart(6) + kEnd(1) */
	CHECK_EQ(midici_packetize_sysex7(3, p, 7, pkts, 2), 2);
	CHECK_EQ(pkts[0][0] >> 16, 0x3316u);
	CHECK_EQ(pkts[1][0], 0x33310700u);
	CHECK_EQ(pkts[1][1], 0u);

	/* 12 bytes -> kStart(6) + kEnd(6) */
	CHECK_EQ(midici_packetize_sysex7(1, p, 12, pkts, 2), 2);
	CHECK_EQ(pkts[0][0] >> 16, 0x3116u);
	CHECK_EQ(pkts[1][0] >> 16, 0x3136u);

	/* 13 bytes -> kStart(6) + kContinue(6) + kEnd(1) */
	CHECK_EQ(midici_packetize_sysex7(4, p, 13, pkts, 3), 3);
	CHECK_EQ(pkts[0][0] >> 16, 0x3416u);
	CHECK_EQ(pkts[1][0] >> 16, 0x3426u);
	CHECK_EQ(pkts[2][0], 0x34310D00u);
	CHECK_EQ(pkts[2][1], 0u);

	/* 512 bytes -> 86 packets: kStart(6) + 84*kContinue(6) + kEnd(2) */
	n = midici_packetize_sysex7(7, p, 512, pkts, 86);
	CHECK_EQ(n, 86);
	CHECK_EQ(pkts[0][0] >> 16, 0x3716u);
	for (uint16_t i = 1; i < 85; i++)
		CHECK_EQ(pkts[i][0] >> 16, 0x3726u);
	CHECK_EQ(pkts[85][0] >> 16, 0x3732u);
	CHECK_EQ(pkts[85][1], 0u);

	/* Raw payload: does NOT strip 0xF0/0xF7 */
	{
		uint8_t raw[3] = {0xF0, 0x11, 0xF7};
		CHECK_EQ(midici_packetize_sysex7(0, raw, 3, pkts, 1), 1);
		CHECK_EQ(pkts[0][0], 0x30037011u);
		CHECK_EQ(pkts[0][1], 0x77000000u);
	}
}

static void test_roundtrip_lengths_midici_and_non_ci(void)
{
	static const uint16_t nonci_lens[] = {0, 1, 5, 6, 7, 13, 512};
	static const uint16_t ci_lens[] = {5, 6, 7, 13, 512};
	uint8_t p[512];
	uint32_t pkts[86][2];

	for (uint16_t idx = 0; idx < sizeof(nonci_lens) / sizeof(nonci_lens[0]); idx++) {
		uint16_t len = nonci_lens[idx];
		uint8_t g = (uint8_t)(idx & 1u);
		uint16_t n;

		setup();
		make_nonci_payload(p, len);
		n = midici_packetize_sysex7(g, p, len, pkts, 86);
		CHECK(n > 0);
		push_raw_packets(pkts, n);
		CHECK_EQ(h_calls, 0);
		CHECK_EQ(stub_sysex_calls, 1);
		CHECK_EQ(stub_sysex_port, g);
		CHECK_EQ(stub_sysex_len, len + 2u);
		CHECK_EQ(stub_sysex_buf[0], 0xF0);
		if (len + 2u <= sizeof(stub_sysex_buf)) {
			CHECK_EQ(stub_sysex_buf[1u + len], 0xF7);
			if (len > 0)
				CHECK_MEM(stub_sysex_buf + 1, p, len);
		} else {
			/* stub_sysex_buf clamps memcpy at 320 bytes while recording full stub_sysex_len */
			CHECK_MEM(stub_sysex_buf + 1, p, sizeof(stub_sysex_buf) - 1u);
		}
	}

	/* Non-CI 512 B on group 2 completes cleanly without dispatch or drop */
	{
		uint32_t drops0;
		uint16_t n;
		setup();
		drops0 = midici_reassembly_drops();
		make_nonci_payload(p, 512);
		n = midici_packetize_sysex7(2, p, 512, pkts, 86);
		push_raw_packets(pkts, n);
		CHECK_EQ(stub_sysex_calls, 0);
		CHECK_EQ(h_calls, 0);
		CHECK_EQ(midici_reassembly_drops(), drops0);
	}

	/* MIDI-CI round-trips via midici_packetize_sysex7 -> midici_rx_push */
	for (uint16_t idx = 0; idx < sizeof(ci_lens) / sizeof(ci_lens[0]); idx++) {
		uint16_t len = ci_lens[idx];
		uint32_t before;
		uint16_t n;

		setup();
		before = midici_messages_received();
		make_payload(p, len);
		n = midici_packetize_sysex7(2, p, len, pkts, 86);
		push_raw_packets(pkts, n);
		CHECK_EQ(h_calls, 1);
		CHECK_EQ(h_group, 2);
		CHECK_EQ(h_len, len);
		CHECK_MEM(h_buf, p, len);
		CHECK_EQ(stub_sysex_calls, 0);
		CHECK_EQ(midici_messages_received(), before + 1u);
	}

	/* MIDI-CI round-trip through TX -> ump_tx_fill_packet -> ump_rx_push_words -> RX */
	{
		uint8_t usb[64];
		uint32_t words[16];
		uint8_t bytes;

		setup();
		make_payload(p, 13);
		CHECK_EQ(midici_send_sysex(1, p, 13), 0);
		CHECK_EQ(midici_tx_drain(CI_TX_DRAIN_MAX), 3);
		bytes = ump_tx_fill_packet(usb);
		CHECK_EQ(bytes, 24);
		for (uint8_t w = 0; w < bytes / 4u; w++) {
			words[w] = (uint32_t)usb[4u * w] |
			           ((uint32_t)usb[4u * w + 1u] << 8) |
			           ((uint32_t)usb[4u * w + 2u] << 16) |
			           ((uint32_t)usb[4u * w + 3u] << 24);
		}
		ump_rx_push_words(words, (uint16_t)(bytes / 4u));
		ump_rx_poll();
		CHECK_EQ(h_calls, 1);
		CHECK_EQ(h_group, 1);
		CHECK_EQ(h_len, 13);
		CHECK_MEM(h_buf, p, 13);
	}

	/* Split MIDI-CI header across kStart (2B) + kEnd (2B) */
	{
		uint32_t split[2][2] = {
			{0x30127E7Fu, 0x00000000u},
			{0x30320D70u, 0x00000000u}
		};
		static const uint8_t expect[4] = {0x7E, 0x7F, 0x0D, 0x70};
		setup();
		push_raw_packets(split, 2);
		CHECK_EQ(h_calls, 1);
		CHECK_EQ(h_len, 4);
		CHECK_MEM(h_buf, expect, 4);
	}
}

static void test_high_bit_masking_tx_and_rx(void)
{
	uint8_t raw[7] = {0xF0, 0x81, 0xFE, 0xFF, 0x80, 0xAB, 0xF7};
	uint32_t pkts[2][2];
	uint32_t rxpkt[2] = {0x3204FEAAu, 0x8DC10000u};
	static const uint8_t expect_rx[4] = {0x7E, 0x2A, 0x0D, 0x41};

	CHECK_EQ(midici_packetize_sysex7(1, raw, 7, pkts, 2), 2);
	CHECK_EQ(pkts[0][0], 0x31167001u);
	CHECK_EQ(pkts[0][1], 0x7E7F002Bu);
	CHECK_EQ(pkts[1][0], 0x31317700u);
	CHECK_EQ(pkts[1][1], 0u);

	setup();
	CHECK_EQ(midici_rx_push(rxpkt), 0);
	midici_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_group, 2);
	CHECK_EQ(h_len, 4);
	CHECK_MEM(h_buf, expect_rx, 4);
}

static void test_interleaved_ci_groups(void)
{
	uint8_t p0[20], p2[15], p1[10];
	uint32_t pk0[4][2], pk2[3][2], pk1[2][2];
	uint32_t drops0;

	setup();
	drops0 = midici_reassembly_drops();
	make_payload(p0, 20);
	make_payload(p2, 15);
	make_nonci_payload(p1, 10);

	CHECK_EQ(midici_packetize_sysex7(0, p0, 20, pk0, 4), 4);
	CHECK_EQ(midici_packetize_sysex7(2, p2, 15, pk2, 3), 3);
	CHECK_EQ(midici_packetize_sysex7(1, p1, 10, pk1, 2), 2);

	CHECK_EQ(midici_rx_push(pk0[0]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk2[0]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk1[0]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk0[1]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk2[1]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk0[2]), 0); midici_poll();
	CHECK_EQ(midici_rx_push(pk1[1]), 0); midici_poll();
	CHECK_EQ(stub_sysex_calls, 1);
	CHECK_EQ(stub_sysex_port, 1);
	CHECK_EQ(stub_sysex_len, 12);
	CHECK_MEM(stub_sysex_buf + 1, p1, 10);

	CHECK_EQ(midici_rx_push(pk2[2]), 0); midici_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_group, 2);
	CHECK_EQ(h_len, 15);
	CHECK_MEM(h_buf, p2, 15);

	CHECK_EQ(midici_rx_push(pk0[3]), 0); midici_poll();
	CHECK_EQ(h_calls, 2);
	CHECK_EQ(h_group, 0);
	CHECK_EQ(h_len, 20);
	CHECK_MEM(h_buf, p0, 20);
	CHECK_EQ(midici_reassembly_drops(), drops0);
}

static void test_interleaved_pool_boundary(void)
{
	uint8_t msg0[18], msg1[12], msg2[12], msg3[12], msg4[12], msg5[4];
	uint32_t pk0[3][2], pk1[2][2], pk2[2][2], pk3[2][2], pk4[2][2], pk5[1][2];
	uint32_t drops0;

	setup();
	drops0 = midici_reassembly_drops();
	make_payload(msg0, 18);
	make_payload(msg1, 12);
	make_payload(msg2, 12);
	make_payload(msg3, 12);
	make_payload(msg4, 12);
	make_nonci_payload(msg5, 4);

	CHECK_EQ(midici_packetize_sysex7(0, msg0, 18, pk0, 3), 3);
	CHECK_EQ(midici_packetize_sysex7(1, msg1, 12, pk1, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(2, msg2, 12, pk2, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(3, msg3, 12, pk3, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(4, msg4, 12, pk4, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(5, msg5, 4, pk5, 1), 1);

	/* Start 4 concurrent streams on groups 0..3 */
	CHECK_EQ(midici_rx_push(pk0[0]), 0);
	CHECK_EQ(midici_rx_push(pk1[0]), 0);
	CHECK_EQ(midici_rx_push(pk2[0]), 0);
	CHECK_EQ(midici_rx_push(pk3[0]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0);

	/* Continue on group 0 promotes group 0 to MRU -> group 1 becomes LRU */
	CHECK_EQ(midici_rx_push(pk0[1]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0);

	/* kComplete on idle group 5 uses stack buffer and does not evict any slot */
	CHECK_EQ(midici_rx_push(pk5[0]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0);

	/* 5th multi-packet stream on group 4 evicts LRU group 1 */
	CHECK_EQ(midici_rx_push(pk4[0]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 1u);

	/* Complete groups 0, 2, 3, 4 */
	CHECK_EQ(midici_rx_push(pk0[2]), 0);
	CHECK_EQ(midici_rx_push(pk2[1]), 0);
	CHECK_EQ(midici_rx_push(pk3[1]), 0);
	CHECK_EQ(midici_rx_push(pk4[1]), 0);
	midici_poll();
	CHECK_EQ(h_calls, 4);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 1u);

	/* Group 1's kEnd is now orphaned and dropped */
	CHECK_EQ(midici_rx_push(pk1[1]), 0);
	midici_poll();
	CHECK_EQ(h_calls, 4);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 2u);
}

static void test_malformed_orphaned_continue_and_end(void)
{
	static const uint32_t orphans[8][2] = {
		{0x30260102u, 0x03040506u}, /* group 0 continue */
		{0x31260102u, 0x03040506u}, /* group 1 continue */
		{0x32260102u, 0x03040506u}, /* group 2 continue */
		{0x3F260102u, 0x03040506u}, /* group 15 continue */
		{0x30320102u, 0x00000000u}, /* group 0 end */
		{0x31320102u, 0x00000000u}, /* group 1 end */
		{0x33320102u, 0x00000000u}, /* group 3 end */
		{0x3E320102u, 0x00000000u}  /* group 14 end */
	};
	uint8_t ci[5], nonci[4];
	uint32_t pk_ci[1][2], pk_nonci[1][2];
	uint32_t drops0;

	setup();
	drops0 = midici_reassembly_drops();
	for (uint16_t i = 0; i < 8; i++)
		CHECK_EQ(midici_rx_push(orphans[i]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 8u);
	CHECK_EQ(h_calls, 0);
	CHECK_EQ(stub_sysex_calls, 0);

	/* Subsequent valid messages succeed cleanly */
	make_payload(ci, 5);
	make_nonci_payload(nonci, 4);
	CHECK_EQ(midici_packetize_sysex7(0, ci, 5, pk_ci, 1), 1);
	CHECK_EQ(midici_packetize_sysex7(1, nonci, 4, pk_nonci, 1), 1);
	CHECK_EQ(midici_rx_push(pk_ci[0]), 0);
	CHECK_EQ(midici_rx_push(pk_nonci[0]), 0);
	midici_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(stub_sysex_calls, 1);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 8u);
}

static void test_malformed_start_and_complete_while_active(void)
{
	uint8_t msgA[12], msgB[10], msgC[12], msgD[5];
	uint32_t pkA[2][2], pkB[2][2], pkC[2][2], pkD[1][2];
	uint32_t drops0;

	setup();
	drops0 = midici_reassembly_drops();
	make_payload(msgA, 12);
	make_payload(msgB, 10);
	make_payload(msgC, 12);
	make_payload(msgD, 5);

	CHECK_EQ(midici_packetize_sysex7(0, msgA, 12, pkA, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(0, msgB, 10, pkB, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(0, msgC, 12, pkC, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(0, msgD, 5, pkD, 1), 1);

	/* Start msgA, interrupt with msgB kStart -> drop +1, then complete msgB */
	CHECK_EQ(midici_rx_push(pkA[0]), 0);
	CHECK_EQ(midici_rx_push(pkB[0]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 1u);
	CHECK_EQ(midici_rx_push(pkB[1]), 0);
	midici_poll();
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(h_len, 10);
	CHECK_MEM(h_buf, msgB, 10);

	/* Start msgC, interrupt with msgD kComplete -> drop +2, msgD delivered */
	CHECK_EQ(midici_rx_push(pkC[0]), 0);
	CHECK_EQ(midici_rx_push(pkD[0]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 2u);
	CHECK_EQ(h_calls, 2);
	CHECK_EQ(h_len, 5);
	CHECK_MEM(h_buf, msgD, 5);

	/* Trailing kEnd of msgC is now orphaned -> drop +3 */
	CHECK_EQ(midici_rx_push(pkC[1]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 3u);
	CHECK_EQ(h_calls, 2);
}

static void test_malformed_overlong_resets_partial(void)
{
	uint8_t big[518], ok_ci[12], ok_nonci[6];
	uint32_t pkBig[87][2], pk_ci[2][2], pk_nonci[1][2];
	uint32_t drops0;
	uint16_t n;

	setup();
	drops0 = midici_reassembly_drops();

	/* 516 B MIDI-CI on group 0 (86 pkts): 85 pkts accepted, 86th kEnd drops */
	make_payload(big, 516);
	n = midici_packetize_sysex7(0, big, 516, pkBig, 87);
	CHECK_EQ(n, 86);
	push_raw_packets(pkBig, 85);
	CHECK_EQ(midici_reassembly_drops(), drops0);
	CHECK_EQ(h_calls, 0);
	CHECK_EQ(midici_rx_push(pkBig[85]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 1u);
	CHECK_EQ(h_calls, 0);

	/* 514 B non-CI on group 1 (86 pkts): 86th kEnd (510+4=514 > 512) drops */
	make_nonci_payload(big, 514);
	n = midici_packetize_sysex7(1, big, 514, pkBig, 87);
	CHECK_EQ(n, 86);
	push_raw_packets(pkBig, 86);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 2u);
	CHECK_EQ(stub_sysex_calls, 0);

	/* 518 B MIDI-CI on group 0 (87 pkts): 86th kContinue drops & frees slot, 87th kEnd orphaned */
	make_payload(big, 518);
	n = midici_packetize_sysex7(0, big, 518, pkBig, 87);
	CHECK_EQ(n, 87);
	push_raw_packets(pkBig, 86);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 3u);
	CHECK_EQ(h_calls, 0);
	CHECK_EQ(midici_rx_push(pkBig[86]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 4u);
	CHECK_EQ(h_calls, 0);

	/* Subsequent valid messages on groups 0 and 1 succeed cleanly */
	make_payload(ok_ci, 12);
	make_nonci_payload(ok_nonci, 6);
	CHECK_EQ(midici_packetize_sysex7(0, ok_ci, 12, pk_ci, 2), 2);
	CHECK_EQ(midici_packetize_sysex7(1, ok_nonci, 6, pk_nonci, 1), 1);
	push_raw_packets(pk_ci, 2);
	push_raw_packets(pk_nonci, 1);
	CHECK_EQ(h_calls, 1);
	CHECK_EQ(stub_sysex_calls, 1);
	CHECK_EQ(midici_reassembly_drops(), drops0 + 4u);
}

static void test_malformed_invalid_count_and_form(void)
{
	static const uint32_t bad[5][2] = {
		{0x30070000u, 0u}, /* cnt = 7 */
		{0x300F0000u, 0u}, /* cnt = 15 */
		{0x30410000u, 0u}, /* form = 4 */
		{0x30F10000u, 0u}, /* form = 15 */
		{0x40120102u, 0u}  /* non-MT3 */
	};
	uint8_t msg[12];
	uint32_t pk[2][2];
	uint32_t bad_g2[2] = {0x32070000u, 0u};
	uint32_t drops0;

	setup();
	drops0 = midici_reassembly_drops();
	for (uint16_t i = 0; i < 5; i++)
		CHECK_EQ(midici_rx_push(bad[i]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 5u);
	CHECK_EQ(h_calls, 0);
	CHECK_EQ(stub_sysex_calls, 0);

	/* Active partial on group 2 freed by invalid cnt = 7 on group 2 */
	make_payload(msg, 12);
	CHECK_EQ(midici_packetize_sysex7(2, msg, 12, pk, 2), 2);
	CHECK_EQ(midici_rx_push(pk[0]), 0);
	CHECK_EQ(midici_rx_push(bad_g2), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 6u);

	/* Subsequent kEnd is orphaned -> drop +7 */
	CHECK_EQ(midici_rx_push(pk[1]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), drops0 + 7u);
	CHECK_EQ(h_calls, 0);
}

static void test_midici_init_and_reset_contract(void)
{
	uint8_t p[12];
	uint32_t pk[2][2];
	uint32_t orphan_g1[2] = {0x31260102u, 0x03040506u};

	stubs_reset();
	ump_rx_reset();
	ump_tx_reset();
	midici_init();
	CHECK_EQ(midici_reassembly_drops(), 0);
	CHECK_EQ(midici_messages_received(), 0);
	CHECK_EQ(midici_tx_pending(), 0);
	CHECK_EQ(midici_tx_free_packets(), CI_TX_QUEUE_PACKETS);

	midici_set_handler(handler);
	h_calls = 0;

	make_payload(p, 12);
	CHECK_EQ(midici_send_sysex(0, p, 12), 0);
	CHECK_EQ(midici_tx_pending(), 2);
	CHECK_EQ(midici_tx_free_packets(), CI_TX_QUEUE_PACKETS - 2u);

	CHECK_EQ(midici_packetize_sysex7(0, p, 12, pk, 2), 2);
	CHECK_EQ(midici_rx_push(pk[0]), 0);
	CHECK_EQ(midici_rx_push(orphan_g1), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), 1);

	/* midici_reset clears queues & partial slots while preserving monotonic drops */
	midici_reset();
	CHECK_EQ(midici_tx_pending(), 0);
	CHECK_EQ(midici_tx_free_packets(), CI_TX_QUEUE_PACKETS);
	CHECK_EQ(midici_reassembly_drops(), 1);

	CHECK_EQ(midici_rx_push(pk[1]), 0);
	midici_poll();
	CHECK_EQ(midici_reassembly_drops(), 2);
	CHECK_EQ(h_calls, 0);

	/* Re-init restores zeroed counters for subsequent tests */
	midici_init();
	CHECK_EQ(midici_reassembly_drops(), 0);
	CHECK_EQ(midici_messages_received(), 0);
	midici_set_handler(handler);
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
	RUN(test_packetize_forms_and_sizes);
	RUN(test_roundtrip_lengths_midici_and_non_ci);
	RUN(test_high_bit_masking_tx_and_rx);
	RUN(test_interleaved_ci_groups);
	RUN(test_interleaved_pool_boundary);
	RUN(test_malformed_orphaned_continue_and_end);
	RUN(test_malformed_start_and_complete_while_active);
	RUN(test_malformed_overlong_resets_partial);
	RUN(test_malformed_invalid_count_and_form);
	RUN(test_midici_init_and_reset_contract);
}
