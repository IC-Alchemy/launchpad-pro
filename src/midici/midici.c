#include "midici/midici.h"
#include "midici/ci_queue.h"
#include "platform/irq.h"
#include "platform/platform.h"
#include "ump/ump.h"
#include "app.h"

CI_QUEUE_DEFINE(ci_rx, CI_RX_QUEUE_PACKETS);	/* CiRxQueue: USB RX ISR -> main loop */
CI_QUEUE_DEFINE(ci_tx, CI_TX_QUEUE_PACKETS);	/* CiTxQueue: main loop -> USB TX path */

/* Bumped by midici_reset (may run in interrupt context); midici_poll notices a
 * change and discards its own state. Only written under irq_save. */
static volatile uint16_t epoch;

/* Owned exclusively by midici_poll. */
static uint16_t seen_epoch;
static uint8_t ci_buf[CI_REASSEMBLY_BYTES];	/* payload from the 0x7E, no F0/F7 */
static uint16_t ci_len;
static uint8_t ci_active;
static uint8_t ci_group;
/* Non-MIDI-CI SysEx (the app's own): one reassembler per standalone/MIDI group. */
static struct ump_sysex7_rx other[2];

static uint8_t reply_buf[CI_REPLY_BYTES];

static midici_handler_fn handler;
static uint32_t received;
static uint32_t drops;

static uint32_t muid;
static uint8_t muid_ready;

void midici_set_handler(midici_handler_fn fn)
{
	handler = fn;
}

void midici_reset(void)
{
	uint32_t s = irq_save();
	ci_queue_reset(&ci_rx);
	ci_queue_reset(&ci_tx);
	epoch++;
	irq_restore(s);
}

int midici_rx_push(const uint32_t pkt[2])
{
	return ci_queue_push(&ci_rx, pkt);
}

uint16_t midici_rx_free_packets(void)
{
	return ci_queue_free(&ci_rx);
}

uint32_t midici_rx_overflows(void)
{
	return ci_rx.overflows;
}

uint32_t midici_tx_overflows(void)
{
	return ci_tx.overflows;
}

uint32_t midici_messages_received(void)
{
	return received;
}

uint32_t midici_reassembly_drops(void)
{
	return drops;
}

uint8_t *midici_reply_buffer(uint16_t *capacity)
{
	if (capacity)
		*capacity = CI_REPLY_BYTES;
	return reply_buf;
}

/* ---- MUID ---- */

int midici_muid_valid(uint32_t m)
{
	return m != 0 && m < 0x0FFFFF00u;
}

uint32_t midici_muid_regenerate(void)
{
	uint32_t m = 0;
	uint8_t i;

	for (i = 0; i < 16; i++) {
		m = platform_entropy32() & 0x0FFFFFFFu;
		if (midici_muid_valid(m))
			break;
	}
	if (!midici_muid_valid(m))
		m = (m % 0x0FFFFEFFu) + 1u;	/* 1 .. 0x0FFFFEFF, always valid */
	muid = m;
	muid_ready = 1;
	return m;
}

uint32_t midici_muid(void)
{
	return muid_ready ? muid : midici_muid_regenerate();
}

/* ---- RX reassembly ---- */

static void ci_abort(void)
{
	if (ci_active)
		drops++;
	ci_active = 0;
	ci_len = 0;
}

static void other_complete(uint8_t group, struct ump_sysex7_rx *rx)
{
	app_sysex_event(group, rx->buf, rx->len);
	ump_sysex7_rx_reset(rx);
}

static void feed_other(uint8_t group, const uint32_t pkt[2])
{
	if (group > 1)
		return;
	if (ump_sysex7_rx_feed(&other[group], pkt) == 1)
		other_complete(group, &other[group]);
}

static void feed_ci(uint8_t group, uint8_t st, uint8_t cnt, const uint8_t b[6])
{
	uint8_t i;

	if (st == 0 || st == 1) {
		ci_active = 1;
		ci_group = group;
		ci_len = 0;
	}
	if ((uint16_t)(ci_len + cnt) > CI_REASSEMBLY_BYTES) {
		ci_abort();
		return;
	}
	for (i = 0; i < cnt; i++)
		ci_buf[ci_len++] = b[i] & 0x7F;
	if (st == 0 || st == 3) {
		uint16_t n = ci_len;
		ci_active = 0;
		ci_len = 0;
		received++;
		if (handler)
			handler(group, ci_buf, n);
	}
}

static void feed(const uint32_t pkt[2])
{
	uint32_t w0 = pkt[0], w1 = pkt[1];
	uint8_t group = (uint8_t)((w0 >> 24) & 0xF);
	uint8_t st = (uint8_t)((w0 >> 20) & 0xF);
	uint8_t cnt = (uint8_t)((w0 >> 16) & 0xF);
	uint8_t b[6];

	if ((w0 >> 28) != 3 || cnt > 6 || st > 3) {
		if (ci_active && ci_group == group)
			ci_abort();
		if (group < 2)
			ump_sysex7_rx_reset(&other[group]);
		return;
	}
	b[0] = (uint8_t)(w0 >> 8);
	b[1] = (uint8_t)w0;
	b[2] = (uint8_t)(w1 >> 24);
	b[3] = (uint8_t)(w1 >> 16);
	b[4] = (uint8_t)(w1 >> 8);
	b[5] = (uint8_t)w1;

	if (st == 0 || st == 1) {
		/* the 0x7E 0x?? 0x0D universal-SysEx header tells MIDI-CI apart */
		if (cnt >= 3 && (b[0] & 0x7F) == 0x7E && (b[2] & 0x7F) == 0x0D) {
			if (ci_active)
				ci_abort();	/* one reassembly buffer: older stream lost */
			if (group < 2)
				ump_sysex7_rx_reset(&other[group]);
			feed_ci(group, st, cnt, b);
		} else {
			if (ci_active && ci_group == group)
				ci_abort();
			feed_other(group, pkt);
		}
		return;
	}
	if (ci_active && ci_group == group)
		feed_ci(group, st, cnt, b);
	else
		feed_other(group, pkt);
}

void midici_poll(void)
{
	uint32_t pkt[2];
	uint32_t s;
	uint16_t ep;

	for (;;) {
		s = irq_save();
		ep = epoch;
		irq_restore(s);
		if (ep != seen_epoch) {
			seen_epoch = ep;
			ci_active = 0;
			ci_len = 0;
			ump_sysex7_rx_reset(&other[0]);
			ump_sysex7_rx_reset(&other[1]);
		}
		if (ci_queue_pop(&ci_rx, pkt) != 0)
			return;
		feed(pkt);
	}
}

/* ---- TX ---- */

int midici_send_sysex(uint8_t group, const uint8_t *data, uint16_t len)
{
	uint16_t packets, p;
	uint32_t pkt[2];

	if (len && data[0] == 0xF0) {
		data++;
		len--;
	}
	if (len && data[len - 1] == 0xF7)
		len--;
	packets = len ? (uint16_t)((len + 5) / 6) : 1;
	if (ci_queue_free(&ci_tx) < packets) {
		uint32_t s = irq_save();
		ci_tx.overflows++;
		irq_restore(s);
		return -1;
	}
	for (p = 0; p < packets; p++) {
		ump_sysex7_packet(pkt, group, data, len, p, packets);
		ci_queue_push(&ci_tx, pkt);	/* room checked above; only this context pushes */
	}
	return 0;
}

uint16_t midici_tx_pending(void)
{
	return ci_queue_count(&ci_tx);
}

uint8_t midici_tx_drain(uint8_t max_packets)
{
	uint32_t pkt[2];
	uint8_t n = 0;

	while (n < max_packets && ci_queue_peek(&ci_tx, pkt) == 0) {
		/* app traffic: leaves the UMP Stream reply headroom free */
		if (ump_tx_push_control_app(pkt, 2) != 0)
			break;
		ci_queue_drop(&ci_tx);
		n++;
	}
	return n;
}
