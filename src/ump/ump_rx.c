#include "ump/ump.h"
#include "platform/irq.h"
#include "app.h"
#include "midici/midici.h"
#include "midici/responder.h"

static uint32_t ring[UMP_RX_WORDS];
static volatile uint16_t head; /* written by push (interrupt) */
static volatile uint16_t tail; /* written by poll (main loop) */
static volatile uint32_t overflows;
/* Bumped by ump_rx_reset (may run in interrupt context); ump_rx_poll notices a
 * change and discards its own state. Both are only written under irq_save. */
static volatile uint16_t epoch;

typedef char ump_rx_words_pow2[(UMP_RX_WORDS & (UMP_RX_WORDS - 1)) == 0 ? 1 : -1];

/* Owned exclusively by ump_rx_poll; ump_rx_reset must never write these. */
static uint16_t seen_epoch;
static uint32_t msg[4];
static uint8_t have;
static uint8_t need;

/* Interrupt-side framing (touched only under irq_save, by push and reset): MT 0x3
 * packets are diverted to the MIDI-CI queue so the main-loop ring never sees them. */
static uint8_t isr_left;	/* words still to come in the current message */
static uint8_t isr_mt3;		/* the current message is SysEx7 */
static uint8_t isr_have;	/* words of isr_pkt collected */
static uint32_t isr_pkt[2];

void ump_rx_reset(void)
{
	uint32_t s = irq_save();
	head = 0;
	tail = 0;
	overflows = 0;
	epoch++;
	isr_left = 0;
	isr_mt3 = 0;
	isr_have = 0;
	irq_restore(s);
	midici_reset();
	midici_responder_reset();
}

uint16_t ump_rx_free_words(void)
{
	uint32_t s = irq_save();
	uint16_t used = (uint16_t)(head - tail);
	irq_restore(s);
	return (uint16_t)(UMP_RX_WORDS - used);
}

void ump_rx_push_words(const uint32_t *w, uint16_t n)
{
	uint32_t s = irq_save();
	uint16_t h = head;
	uint16_t t = tail;
	for (uint16_t i = 0; i < n; i++) {
		if (isr_left == 0) {
			uint8_t need = ump_word_count(w[i]);
			if (need == 0 || need > 4)
				need = 1;
			isr_left = need;
			isr_mt3 = (uint8_t)((w[i] >> 28) == 3);
			isr_have = 0;
		}
		isr_left--;
		if (isr_mt3) {
			/* SysEx7 is exactly 2 words; the queue copy is all this does */
			isr_pkt[isr_have++] = w[i];
			if (isr_have == 2) {
				isr_have = 0;
				midici_rx_push(isr_pkt);
			}
			continue;
		}
		if ((uint16_t)(h - t) >= UMP_RX_WORDS) {
			overflows++;
			continue;
		}
		ring[h % UMP_RX_WORDS] = w[i];
		h++;
	}
	head = h;
	irq_restore(s);
}

uint32_t ump_rx_overflows(void)
{
	return overflows;
}

static void dispatch(void)
{
	uint8_t mt = (uint8_t)(msg[0] >> 28);
	uint8_t group = (uint8_t)((msg[0] >> 24) & 0xF);

	if (mt == 0xF) {
		ump_stream_handle(msg);
		return;
	}
	if (group > 1)
		return;
	if (mt == 1 || mt == 2 || mt == 4) {
		uint8_t out[8][3];
		uint8_t n = ump_to_midi1(msg, out, 8);
		for (uint8_t i = 0; i < n; i++)
			app_midi_event(group, out[i][0], out[i][1], out[i][2]);
	}
}

void ump_rx_poll(void)
{
	midici_poll();
	midici_responder_poll();
	for (;;) {
		uint32_t s = irq_save();
		uint16_t t = tail;
		uint16_t h = head;
		uint32_t w = 0;
		uint16_t ep = epoch;
		int got = (h != t);
		if (got) {
			w = ring[t % UMP_RX_WORDS];
			tail = (uint16_t)(t + 1);
		}
		irq_restore(s);
		if (ep != seen_epoch) {
			seen_epoch = ep;
			have = 0;
			need = 0;
		}
		if (!got)
			return;

		if (have == 0) {
			need = ump_word_count(w);
			if (need == 0 || need > 4)
				need = 1;
		}
		msg[have++] = w;
		if (have == need) {
			have = 0;
			dispatch(); /* outside the critical section */
		}
	}
}
