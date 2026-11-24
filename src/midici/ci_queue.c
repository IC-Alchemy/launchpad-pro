#include "midici/ci_queue.h"
#include "platform/irq.h"

void ci_queue_reset(struct ci_queue *q)
{
	uint32_t s = irq_save();
	q->head = 0;
	q->tail = 0;
	q->overflows = 0;
	irq_restore(s);
}

int ci_queue_push(struct ci_queue *q, const uint32_t pkt[2])
{
	uint32_t s = irq_save();
	int rc = -1;

	if ((uint16_t)(q->tail - q->head) < q->packets) {
		uint16_t i = (uint16_t)(q->tail & (q->packets - 1));
		q->words[2 * i] = pkt[0];
		q->words[2 * i + 1] = pkt[1];
		q->tail++;
		rc = 0;
	} else {
		q->overflows++;
	}
	irq_restore(s);
	return rc;
}

int ci_queue_peek(struct ci_queue *q, uint32_t pkt[2])
{
	uint32_t s = irq_save();
	int rc = -1;

	if (q->head != q->tail) {
		uint16_t i = (uint16_t)(q->head & (q->packets - 1));
		pkt[0] = q->words[2 * i];
		pkt[1] = q->words[2 * i + 1];
		rc = 0;
	}
	irq_restore(s);
	return rc;
}

void ci_queue_drop(struct ci_queue *q)
{
	uint32_t s = irq_save();
	if (q->head != q->tail)
		q->head++;
	irq_restore(s);
}

int ci_queue_pop(struct ci_queue *q, uint32_t pkt[2])
{
	uint32_t s = irq_save();
	int rc = -1;

	if (q->head != q->tail) {
		uint16_t i = (uint16_t)(q->head & (q->packets - 1));
		pkt[0] = q->words[2 * i];
		pkt[1] = q->words[2 * i + 1];
		q->head++;
		rc = 0;
	}
	irq_restore(s);
	return rc;
}

uint16_t ci_queue_count(struct ci_queue *q)
{
	uint32_t s = irq_save();
	uint16_t n = (uint16_t)(q->tail - q->head);
	irq_restore(s);
	return n;
}

uint16_t ci_queue_free(struct ci_queue *q)
{
	return (uint16_t)(q->packets - ci_queue_count(q));
}
