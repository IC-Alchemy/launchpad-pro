#ifndef TWOSTAGE_H
#define TWOSTAGE_H

/* Two-stage velocity engine: velocity from the time between two threshold
 * crossings of the raw 12-bit pad reading, fed one 64-value ADC frame per
 * 1 kHz timer tick. Pure (no hal calls), safe to call from interrupt context:
 * no division by zero, no 64-bit math, bounded work. Crossing times are
 * interpolated linearly between samples, in Q8 milliseconds. */

#include "app.h"

#define TS_T1 120
#define TS_T2 480
#define TS_REL 80
#define TS_TIMEOUT_MS 80
#define TS_REL_TICKS 3
#define TS_DT_FAST_Q8 256
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
	u32 now_ms;
	u8 primed;        /* 0 until first frame stored */
} ts_engine;

/* on = 1: Note On with velocity; on = 0: Note Off (velocity 0) */
typedef void (*ts_note_fn)(void *ctx, u8 adc_ch, u8 on, u16 velocity);

void ts_init(ts_engine *e);
void ts_tick(ts_engine *e, const u16 *raw, ts_note_fn fn, void *ctx);
void ts_release_all(ts_engine *e, ts_note_fn fn, void *ctx);   /* Off for every HELD pad, all to IDLE */
u16 ts_velocity(u32 dt_q8);
u32 ts_cross_q8(u32 now_ms, u16 prev, u16 x, u16 level);       /* interpolated crossing time */

#endif
