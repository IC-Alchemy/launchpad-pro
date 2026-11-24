#include "pads/twostage.h"

/* Q8 times are modulo 2^32; the mask keeps that true where u32 is wider (host tests) */
#define M32(v) ((u32)(v) & 0xFFFFFFFFu)

/* thresholds must be ordered; a negative array size fails the build otherwise */
typedef char ts_order_check[(TS_REL < TS_T1 && TS_T1 < TS_T2) ? 1 : -1];

void ts_init(ts_engine *e)
{
	u8 i;
	e->last_ms = 0;
	e->primed = 0;
	for (i = 0; i < 64; i++) {
		e->pad[i].state = TS_IDLE;
		e->pad[i].low_count = 0;
		e->pad[i].prev = 0;
		e->pad[i].t1_q8 = 0;
	}
}

/* log2(1 + i/16) in Q12, i = 0..16 */
static const u16 lg_tab[17] = {
	0, 358, 696, 1016, 1319, 1607, 1882, 2145, 2396,
	2637, 2869, 3092, 3307, 3514, 3715, 3908, 4096
};

/* log2(x) in Q12 for 1 <= x < 2^20: highest set bit + table interpolation */
static u32 log2_q12(u32 x)
{
	u32 msb = 0, f, i, r;
	while ((x >> (msb + 1)) != 0)
		msb++;
	f = (((x - (1u << msb)) << 12) >> msb);	/* fraction, Q12, < 4096 */
	i = f >> 8;
	r = f & 255;
	return (msb << 12) + lg_tab[i] + (((u32)(lg_tab[i + 1] - lg_tab[i]) * r) >> 8);
}

u16 ts_velocity(u32 dt_q8)
{
	u32 num, den;
	if (dt_q8 <= TS_DT_MIN_Q8)
		return 0xFFFFu;
	if (dt_q8 >= TS_DT_MAX_Q8)
		return TS_VEL_MIN;
	num = log2_q12(TS_DT_MAX_Q8) - log2_q12(dt_q8);	/* 0..den */
	den = log2_q12(TS_DT_MAX_Q8) - log2_q12(TS_DT_MIN_Q8);
	/* (0xFFFF - MIN) * num < 65023 * 22700 < 2^32 */
	return (u16)(TS_VEL_MIN + ((u32)(0xFFFFu - TS_VEL_MIN) * num) / den);
}

u32 ts_cross_q8(u32 last_ms, u32 span, u16 prev, u16 x, u16 level)
{
	u32 base = M32(last_ms * 256);
	u32 frac;
	if (x <= prev)
		return M32((last_ms + span) * 256);
	if (level <= prev) {
		frac = 0;
	} else {
		frac = ((u32)(level - prev) * 256) / (u32)(x - prev);
		if (frac > 256)
			frac = 256;
	}
	return M32(base + frac * span);	/* span <= 255: frac * span < 2^17 */
}

static u16 vel_between(u32 t1_q8, u32 t2_q8)
{
	u32 d = M32(t2_q8 - t1_q8);	/* modular: survives the now_ms*256 wrap */
	return ts_velocity(d > 0x80000000u ? 0 : d);
}

static void step(ts_engine *e, u8 ch, u16 x, u32 now_ms, u32 span, ts_note_fn fn, void *ctx)
{
	ts_pad *p = &e->pad[ch];
	u16 prev = p->prev;
	u32 last = e->last_ms;
	u32 now_q8 = M32(now_ms * 256);

	switch (p->state) {
	case TS_IDLE:
		/* upward crossing only: a pad already above T1 is ignored until it
		 * drops below and rises again */
		if (prev < TS_T1 && x >= TS_T1) {
			u32 t1 = ts_cross_q8(last, span, prev, x, TS_T1);
			if (x >= TS_T2) {
				u32 t2 = ts_cross_q8(last, span, prev, x, TS_T2);
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
		/* x in [TS_REL, TS_T1): keep waiting, accepted behaviour. A pad
		 * parked there and then rising through T1 again starts a fresh
		 * ramp, so t1 is re-stamped on every upward T1 crossing. */
		if (x < TS_REL) {
			p->state = TS_IDLE;
			break;
		}
		if (prev < TS_T1 && x >= TS_T1)
			p->t1_q8 = ts_cross_q8(last, span, prev, x, TS_T1);
		if (x >= TS_T2) {
			u32 t2 = ts_cross_q8(last, span, prev, x, TS_T2);
			p->state = TS_HELD;
			p->low_count = 0;
			fn(ctx, ch, 1, vel_between(p->t1_q8, t2));
		} else if (x >= TS_T1 && M32(now_q8 - p->t1_q8) >= (u32)TS_TIMEOUT_MS * 256) {
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

void ts_tick(ts_engine *e, const u16 *raw, u32 now_ms, ts_note_fn fn, void *ctx)
{
	u8 i;
	u32 span;
	if (!e->primed) {
		for (i = 0; i < 64; i++)
			e->pad[i].prev = raw[i];
		e->last_ms = now_ms;
		e->primed = 1;
		return;
	}
	span = M32(now_ms - e->last_ms);
	if (span == 0)
		return;
	if (span > 255)
		span = 255;
	for (i = 0; i < 64; i++)
		step(e, i, raw[i], now_ms, span, fn, ctx);
	e->last_ms = now_ms;
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
