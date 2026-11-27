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

/* Owned exclusively by midici_poll (and midici_init under irq_save). */
static uint16_t seen_epoch;

#define SLOT_GROUP_FREE 0xFFu
struct sysex_rx_slot {
	uint8_t buf[CI_REASSEMBLY_BYTES + 2]; /* [0]=0xF0 reserve, [1..len]=payload, [len+1]=0xF7 reserve */
	uint16_t len;                         /* 0..CI_REASSEMBLY_BYTES */
	uint8_t group;                        /* 0..15, or SLOT_GROUP_FREE */
	uint8_t lru;                          /* 0 = LRU .. (CI_REASSEMBLY_SLOTS - 1) = MRU */
};
static struct sysex_rx_slot rx_slots[CI_REASSEMBLY_SLOTS];
static uint8_t rx_slots_ready;

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

static void rx_reset_slots(void)
{
	uint8_t i;

	for (i = 0; i < CI_REASSEMBLY_SLOTS; i++) {
		rx_slots[i].group = SLOT_GROUP_FREE;
		rx_slots[i].len = 0;
		rx_slots[i].lru = i;
	}
	rx_slots_ready = 1;
}

static struct sysex_rx_slot *slot_find(uint8_t group)
{
	uint8_t i;

	for (i = 0; i < CI_REASSEMBLY_SLOTS; i++) {
		if (rx_slots[i].group == group)
			return &rx_slots[i];
	}
	return 0;
}

static void slot_touch(struct sysex_rx_slot *s)
{
	uint8_t old = s->lru;
	uint8_t i;

	for (i = 0; i < CI_REASSEMBLY_SLOTS; i++) {
		if (rx_slots[i].lru > old)
			rx_slots[i].lru--;
	}
	s->lru = CI_REASSEMBLY_SLOTS - 1u;
}

static void slot_free(struct sysex_rx_slot *s)
{
	uint8_t old = s->lru;
	uint8_t i;

	s->group = SLOT_GROUP_FREE;
	s->len = 0;
	for (i = 0; i < CI_REASSEMBLY_SLOTS; i++) {
		if (rx_slots[i].lru < old)
			rx_slots[i].lru++;
	}
	s->lru = 0;
}

static struct sysex_rx_slot *slot_alloc(uint8_t group, uint8_t *evicted)
{
	struct sysex_rx_slot *victim = &rx_slots[0];
	uint8_t i;

	for (i = 0; i < CI_REASSEMBLY_SLOTS; i++) {
		if (rx_slots[i].group == SLOT_GROUP_FREE) {
			*evicted = 0;
			rx_slots[i].group = group;
			rx_slots[i].len = 0;
			slot_touch(&rx_slots[i]);
			return &rx_slots[i];
		}
		if (rx_slots[i].lru < victim->lru)
			victim = &rx_slots[i];
	}
	*evicted = 1;
	victim->group = group;
	victim->len = 0;
	slot_touch(victim);
	return victim;
}

static void rx_dispatch(uint8_t group, uint8_t *buf, uint16_t len)
{
	if (len >= 3u && buf[1] == 0x7Eu && buf[3] == 0x0Du) {
		received++;
		if (handler)
			handler(group, &buf[1], len);
	} else if (group <= 1u) {
		buf[0] = 0xF0u;
		buf[1u + len] = 0xF7u;
		app_sysex_event(group, buf, (uint16_t)(len + 2u));
	}
}

static void feed(const uint32_t pkt[2])
{
	uint32_t w0 = pkt[0], w1 = pkt[1];
	uint8_t mt = (uint8_t)(w0 >> 28);
	uint8_t group = (uint8_t)((w0 >> 24) & 0xFu);
	uint8_t st = (uint8_t)((w0 >> 20) & 0xFu);
	uint8_t cnt = (uint8_t)((w0 >> 16) & 0xFu);
	struct sysex_rx_slot *s;
	uint8_t b[6];
	uint8_t i;

	if (mt != 3u || st > 3u || cnt > 6u) {
		drops++;
		s = slot_find(group);
		if (s)
			slot_free(s);
		return;
	}

	b[0] = (uint8_t)((w0 >> 8) & 0x7Fu);
	b[1] = (uint8_t)(w0 & 0x7Fu);
	b[2] = (uint8_t)((w1 >> 24) & 0x7Fu);
	b[3] = (uint8_t)((w1 >> 16) & 0x7Fu);
	b[4] = (uint8_t)((w1 >> 8) & 0x7Fu);
	b[5] = (uint8_t)(w1 & 0x7Fu);

	if (st == 0u) {
		uint8_t tmp[8];

		s = slot_find(group);
		if (s) {
			drops++;
			slot_free(s);
		}
		for (i = 0; i < cnt; i++)
			tmp[1u + i] = b[i];
		rx_dispatch(group, tmp, cnt);
		return;
	}

	if (st == 1u) {
		s = slot_find(group);
		if (s) {
			drops++;
			s->len = 0;
			slot_touch(s);
		} else {
			uint8_t evicted = 0;
			s = slot_alloc(group, &evicted);
			if (evicted)
				drops++;
		}
		for (i = 0; i < cnt; i++)
			s->buf[1u + i] = b[i];
		s->len = cnt;
		return;
	}

	/* st == 2 (kContinue) or st == 3 (kEnd) */
	s = slot_find(group);
	if (!s) {
		drops++;
		return;
	}
	if ((uint32_t)s->len + cnt > CI_REASSEMBLY_BYTES) {
		drops++;
		slot_free(s);
		return;
	}
	for (i = 0; i < cnt; i++)
		s->buf[1u + s->len++] = b[i];
	if (st == 2u) {
		slot_touch(s);
		return;
	}
	rx_dispatch(group, s->buf, s->len);
	slot_free(s);
}

void midici_init(void)
{
	uint32_t s = irq_save();
	ci_rx.head = 0;
	ci_rx.tail = 0;
	ci_rx.overflows = 0;
	ci_tx.head = 0;
	ci_tx.tail = 0;
	ci_tx.overflows = 0;
	epoch = 0;
	seen_epoch = 0;
	received = 0;
	drops = 0;
	muid = 0;
	muid_ready = 0;
	rx_reset_slots();
	irq_restore(s);
}

void midici_poll(void)
{
	for (;;) {
		uint32_t pkt[2];
		uint32_t s;
		uint16_t ep;
		int got = 0;

		s = irq_save();
		ep = epoch;
		if (ci_rx.head != ci_rx.tail) {
			uint16_t idx = (uint16_t)(ci_rx.head & (ci_rx.packets - 1u));
			pkt[0] = ci_rx.words[2u * idx];
			pkt[1] = ci_rx.words[2u * idx + 1u];
			ci_rx.head++;
			got = 1;
		}
		irq_restore(s);
		if (!rx_slots_ready || ep != seen_epoch) {
			seen_epoch = ep;
			rx_reset_slots();
		}
		if (!got)
			return;
		feed(pkt);
	}
}

/* ---- TX ---- */

uint16_t midici_packetize_sysex7(uint8_t group, const uint8_t *payload, uint16_t len,
                                 uint32_t (*out)[2], uint16_t max_packets)
{
	uint32_t packets32 = len ? (((uint32_t)len + 5u) / 6u) : 1u;
	uint16_t packets, p;

	if (!out || (len > 0 && !payload) || packets32 > max_packets)
		return 0;
	packets = (uint16_t)packets32;
	for (p = 0; p < packets; p++)
		ump_sysex7_packet(out[p], group, payload, len, p, packets);
	return packets;
}

int midici_send_sysex(uint8_t group, const uint8_t *data, uint16_t len)
{
	uint16_t packets, p;
	uint32_t pkt[2];

	if (len > 0 && !data)
		return -1;
	if (len && data[0] == 0xF0) {
		data++;
		len--;
	}
	if (len && data[len - 1] == 0xF7)
		len--;
	packets = len ? (uint16_t)(((uint32_t)len + 5u) / 6u) : 1u;
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

uint16_t midici_tx_free_packets(void)
{
	return ci_queue_free(&ci_tx);
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
