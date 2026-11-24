#ifndef TWOSTAGE_H
#define TWOSTAGE_H

/* Two-stage velocity engine: velocity from the time between two threshold
 * crossings of the raw 12-bit pad reading, fed one 64-value ADC frame per
 * new millisecond. Time is the absolute ms counter passed to ts_tick, so a
 * caller that skips milliseconds is handled: crossings are interpolated
 * linearly over the real gap since the previous sample (clamped to 1..255 ms),
 * in Q8 milliseconds. Pure (no hal calls): no division by zero, no 64-bit
 * math, bounded work. */

#include "app.h"

#define TS_T1 120
#define TS_T2 480
#define TS_REL 80
#define TS_TIMEOUT_MS 80
#define TS_REL_TICKS 3
#define TS_DT_MIN_Q8 23    /* ~0.09 ms: dt at or below this is full velocity */
#define TS_DT_MAX_Q8 1024  /* 4 ms: dt at or above this is TS_VEL_MIN */
#define TS_VEL_MIN 0x0200

enum { TS_IDLE, TS_ARMED, TS_HELD };

typedef struct {
	u8 state;
	u8 low_count;     /* consecutive samples below TS_REL while held */
	u16 prev;         /* previous raw sample */
	u32 t1_q8;        /* T1 crossing time, Q8 ms */
} ts_pad;

typedef struct {
	ts_pad pad[64];   /* ADC channel order */
	u32 last_ms;      /* ms counter of the previous processed frame */
	u8 primed;        /* 0 until first frame stored */
} ts_engine;

/* on = 1: Note On with velocity; on = 0: Note Off (velocity 0) */
typedef void (*ts_note_fn)(void *ctx, u8 adc_ch, u8 on, u16 velocity);

void ts_init(ts_engine *e);
/* First call primes. A call with now_ms == last_ms is ignored. */
void ts_tick(ts_engine *e, const u16 *raw, u32 now_ms, ts_note_fn fn, void *ctx);
void ts_release_all(ts_engine *e, ts_note_fn fn, void *ctx);   /* Off for every HELD pad, all to IDLE */
/* Velocity from the T1..T2 time dt (Q8 ms), logarithmic between two bounds:
 *   dt <= TS_DT_MIN_Q8 (0 counts as MIN)  -> 0xFFFF
 *   dt >= TS_DT_MAX_Q8                    -> TS_VEL_MIN
 *   between: TS_VEL_MIN + (0xFFFF - TS_VEL_MIN) * ln(MAX/dt) / ln(MAX/MIN)
 * So each doubling of dt costs the same velocity step, spreading medium and
 * hard hits instead of clipping them. Non-increasing in dt, never 0. */
u16 ts_velocity(u32 dt_q8);
/* crossing time inside the span (last_ms, last_ms + span], Q8 ms */
u32 ts_cross_q8(u32 last_ms, u32 span, u16 prev, u16 x, u16 level);

#endif
