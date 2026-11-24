#ifndef CI_QUEUE_H
#define CI_QUEUE_H

#include <stdint.h>

/*
 * Fixed-capacity single-producer / single-consumer queue of 2-word UMP
 * packets (SysEx7, MT 0x3). Storage is supplied by the owner (static only).
 * Every operation runs under irq_save, so a producer in the USB interrupt and
 * a consumer in the main loop (or a reset from SET_INTERFACE) never race.
 * Push never blocks: when the queue is full the packet is dropped and counted.
 */
struct ci_queue {
	uint32_t *words;	/* 2 * packets words */
	uint16_t packets;	/* capacity, power of two */
	uint16_t head;		/* next pop */
	uint16_t tail;		/* next push */
	uint32_t overflows;
};

#define CI_QUEUE_DEFINE(name, npackets)                                                    \
	static uint32_t name##_store[2 * (npackets)];                                      \
	static struct ci_queue name = {name##_store, (npackets), 0, 0, 0};                 \
	typedef char name##_pow2[(((npackets) & ((npackets)-1)) == 0) ? 1 : -1] __attribute__((unused))

void ci_queue_reset(struct ci_queue *q);
/* 0 queued, -1 full (overflow counted). */
int ci_queue_push(struct ci_queue *q, const uint32_t pkt[2]);
/* 0 and *pkt filled, -1 empty. */
int ci_queue_pop(struct ci_queue *q, uint32_t pkt[2]);
int ci_queue_peek(struct ci_queue *q, uint32_t pkt[2]);
void ci_queue_drop(struct ci_queue *q);
uint16_t ci_queue_free(struct ci_queue *q);
uint16_t ci_queue_count(struct ci_queue *q);

#endif
