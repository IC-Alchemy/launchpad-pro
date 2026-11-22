#include "pads/twostage.h"

/* thresholds must be ordered; a negative array size fails the build otherwise */
typedef char ts_order_check[(TS_REL < TS_T1 && TS_T1 < TS_T2) ? 1 : -1];

void ts_init(ts_engine *e)
{
	u8 i;
	e->now_ms = 0;
	e->primed = 0;
	for (i = 0; i < 64; i++) {
		e->pad[i].state = TS_IDLE;
		e->pad[i].low_count = 0;
		e->pad[i].prev = 0;
		e->pad[i].t1_q8 = 0;
	}
}

u16 ts_velocity(u32 dt_q8)
{
	u32 dt = dt_q8 < 1 ? 1 : dt_q8;
	u32 v = (0xFFFFu * TS_DT_FAST_Q8) / dt;	/* 0xFFFF * 256 < 2^32 */
	if (v > 0xFFFFu)
		v = 0xFFFFu;
	if (v < TS_VEL_MIN)
		v = TS_VEL_MIN;
	return (u16)v;
}

u32 ts_cross_q8(u32 now_ms, u16 prev, u16 x, u16 level)
{
	u32 base = now_ms ? (now_ms - 1) * 256 : 0;
	u32 frac;
	if (x <= prev)
		return now_ms * 256;
	if (level <= prev) {
		frac = 0;
	} else {
		frac = ((u32)(level - prev) * 256) / (u32)(x - prev);
		if (frac > 256)
			frac = 256;
	}
	return base + frac;
}

static u16 vel_between(u32 t1_q8, u32 t2_q8)
{
	return ts_velocity(t2_q8 > t1_q8 ? t2_q8 - t1_q8 : 0);
}

static void step(ts_engine *e, u8 ch, u16 x, ts_note_fn fn, void *ctx)
{
	ts_pad *p = &e->pad[ch];
	u16 prev = p->prev;
	u32 now_q8 = e->now_ms * 256;

	switch (p->state) {
	case TS_IDLE:
		/* upward crossing only: a pad already above T1 is ignored until it
		 * drops below and rises again */
		if (prev < TS_T1 && x >= TS_T1) {
			u32 t1 = ts_cross_q8(e->now_ms, prev, x, TS_T1);
			if (x >= TS_T2) {
				u32 t2 = ts_cross_q8(e->now_ms, prev, x, TS_T2);
				p->state = TS_HELD;
				p->low_count = 0;
				fn(ctx, ch, 1, vel_between(t1, t2));
			} else {
				p->state = TS_ARMED;
				p->t1_q8 = t1;
			}
		}
		break;
	case TS_ARMED:
		if (x < TS_REL) {
			p->state = TS_IDLE;
		} else if (x >= TS_T2) {
			u32 t2 = ts_cross_q8(e->now_ms, prev, x, TS_T2);
			p->state = TS_HELD;
			p->low_count = 0;
			fn(ctx, ch, 1, vel_between(p->t1_q8, t2));
		} else if (x >= TS_T1 && now_q8 - p->t1_q8 >= (u32)TS_TIMEOUT_MS * 256) {
			p->state = TS_HELD;
			p->low_count = 0;
			fn(ctx, ch, 1, ts_velocity((u32)TS_TIMEOUT_MS * 256));
		}
		break;
	default:	/* TS_HELD */
		if (x < TS_REL) {
			if (++p->low_count >= TS_REL_TICKS) {
				p->state = TS_IDLE;
				p->low_count = 0;
				fn(ctx, ch, 0, 0);
			}
		} else {
			p->low_count = 0;
		}
		break;
	}
	p->prev = x;
}

void ts_tick(ts_engine *e, const u16 *raw, ts_note_fn fn, void *ctx)
{
	u8 i;
	e->now_ms++;
	if (!e->primed) {
		for (i = 0; i < 64; i++)
			e->pad[i].prev = raw[i];
		e->primed = 1;
		return;
	}
	for (i = 0; i < 64; i++)
		step(e, i, raw[i], fn, ctx);
}

void ts_release_all(ts_engine *e, ts_note_fn fn, void *ctx)
{
	u8 i;
	for (i = 0; i < 64; i++) {
		ts_pad *p = &e->pad[i];
		if (p->state == TS_HELD)
			fn(ctx, i, 0, 0);
		p->state = TS_IDLE;
		p->low_count = 0;
	}
}
