#include "test.h"
#include "pads/twostage.h"

#define MAXEV 32
typedef struct {
	int n;
	u8 ch[MAXEV];
	u8 on[MAXEV];
	u16 vel[MAXEV];
	u32 now[MAXEV];
} rec;

static ts_engine g_e;
static rec g_r;
static u32 g_now;	/* absolute ms fed to ts_tick; one new ms per tick0 */

static void note(void *ctx, u8 ch, u8 on, u16 vel)
{
	rec *r = (rec *)ctx;
	if (r->n < MAXEV) {
		r->ch[r->n] = ch;
		r->on[r->n] = on;
		r->vel[r->n] = vel;
		r->now[r->n] = g_now;
	}
	r->n++;
}

static void reset(void)
{
	ts_init(&g_e);
	memset(&g_r, 0, sizeof g_r);
	g_now = 0;
}

/* one frame with every pad at 0 except channel 0 = v0 */
static void tick0(u16 v0)
{
	u16 f[64];
	memset(f, 0, sizeof f);
	f[0] = v0;
	g_now++;
	ts_tick(&g_e, f, g_now, note, &g_r);
}

static int near(u32 a, u32 b, u32 tol)
{
	return a > b ? a - b <= tol : b - a <= tol;
}

static void test_ts_velocity_curve(void)
{
	u32 d;
	u16 last = 0xFFFF;
	CHECK_EQ(ts_velocity(256), 0xFFFF);
	CHECK_EQ(ts_velocity(128), 0xFFFF);
	CHECK_EQ(ts_velocity(512), 0x7FFF);
	CHECK_EQ(ts_velocity(80 * 256), 0x0333);
	CHECK_EQ(ts_velocity(1000 * 256), TS_VEL_MIN);
	CHECK_EQ(ts_velocity(0), 0xFFFF);
	for (d = 1; d <= 30000; d++) {
		u16 v = ts_velocity(d);
		if (v > last) {
			CHECK(v <= last);
			break;
		}
		last = v;
	}
}

static void test_ts_cross_interp(void)
{
	/* (last_ms, span, prev, x, level) */
	CHECK_EQ(ts_cross_q8(9, 1, 0, 240, 120), 9 * 256 + 128);
	CHECK_EQ(ts_cross_q8(9, 1, 100, 100, 120), 2560);
	CHECK_EQ(ts_cross_q8(9, 1, 100, 90, 120), 2560);
	CHECK_EQ(ts_cross_q8(9, 1, 100, 200, 90), 9 * 256);	/* level < prev: fraction 0 */
	CHECK_EQ(ts_cross_q8(9, 1, 0, 100, 4000), 10 * 256);	/* fraction clamped */
	CHECK_EQ(ts_cross_q8(8, 2, 0, 240, 120), 8 * 256 + 256);	/* half of a 2 ms span */
	CHECK_EQ(ts_cross_q8(8, 2, 100, 100, 120), 10 * 256);	/* x <= prev: end of span */
	CHECK_EQ(ts_cross_q8(8, 2, 100, 200, 90), 8 * 256);
}

static void test_ts_primed_first_frame(void)
{
	u16 f[64];
	int i;
	reset();
	for (i = 0; i < 64; i++)
		f[i] = 1000;
	ts_tick(&g_e, f, ++g_now, note, &g_r);
	CHECK_EQ(g_r.n, 0);
	ts_tick(&g_e, f, ++g_now, note, &g_r);
	CHECK_EQ(g_r.n, 0);	/* already above T1 at power-up: ignored */
	for (i = 0; i < 64; i++)
		CHECK_EQ(g_e.pad[i].state, TS_IDLE);
}

static void test_ts_power_up_held_rearms(void)
{
	reset();
	tick0(1000);
	tick0(1000);
	tick0(0);
	tick0(0);
	CHECK_EQ(g_r.n, 0);
	tick0(1000);
	CHECK_EQ(g_r.n, 1);
}

static void test_ts_slow_press(void)
{
	int i;
	reset();
	for (i = 0; i <= 60; i++)
		tick0((u16)(i * 10));
	/* T1 crosses at sample 12, T2 at sample 48: dt = 36 ms exactly */
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.on[0], 1);
	CHECK_EQ(g_r.ch[0], 0);
	CHECK(near(g_r.vel[0], ts_velocity(36 * 256), 1));
	CHECK_EQ(g_r.now[0], 49);	/* sample 48 is the 49th tick */
	CHECK_EQ(g_e.pad[0].state, TS_HELD);
}

static void test_ts_slow_press_across_wrap(void)
{
	int i;
	reset();
	g_now = (1u << 24) - 20;	/* now_ms*256 wraps 2^32 mid-ramp */
	for (i = 0; i <= 60; i++)
		tick0((u16)(i * 10));
	CHECK_EQ(g_r.n, 1);
	CHECK(near(g_r.vel[0], ts_velocity(36 * 256), 1));
	CHECK(g_r.now[0] > (1u << 24));	/* T2 landed after the wrap */
}

static void test_ts_single_tick_hard_hit(void)
{
	reset();
	tick0(0);
	tick0(2000);
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.on[0], 1);
	CHECK_EQ(g_r.vel[0], ts_velocity(((480 - 120) * 256) / 2000));
	CHECK_EQ(g_r.vel[0], 0xFFFF);
	CHECK_EQ(g_e.pad[0].state, TS_HELD);
}

static void test_ts_timeout_gentle(void)
{
	int i;
	u32 t1_tick_now = 2;	/* frame 1 (second tick) crosses T1 */
	reset();
	tick0(0);
	for (i = 0; i < 100; i++)
		tick0(200);
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.on[0], 1);
	CHECK_EQ(g_r.vel[0], ts_velocity(TS_TIMEOUT_MS * 256));
	/* t1 = 1*256 + 153 (Q8); first tick where now*256 - t1 >= 80*256 */
	CHECK_EQ(g_r.now[0], t1_tick_now + TS_TIMEOUT_MS);
	CHECK((g_r.now[0] * 256 - (256 + 153)) >= TS_TIMEOUT_MS * 256);
	CHECK(((g_r.now[0] - 1) * 256 - (256 + 153)) < TS_TIMEOUT_MS * 256);
}

static void test_ts_brush_silent(void)
{
	reset();
	tick0(0);
	tick0(200);
	tick0(200);
	tick0(50);
	CHECK_EQ(g_r.n, 0);
	CHECK_EQ(g_e.pad[0].state, TS_IDLE);
}

static void test_ts_release_debounce(void)
{
	static const u16 s[8] = { 70, 90, 70, 70, 90, 70, 70, 70 };
	int i;
	reset();
	tick0(0);
	tick0(1000);
	CHECK_EQ(g_r.n, 1);
	for (i = 0; i < 8; i++) {
		tick0(s[i]);
		if (i < 7)
			CHECK_EQ(g_r.n, 1);
	}
	CHECK_EQ(g_r.n, 2);
	CHECK_EQ(g_r.on[1], 0);
	CHECK_EQ(g_r.vel[1], 0);
	CHECK_EQ(g_r.now[1], 10);	/* last sample is the 10th tick */
	CHECK_EQ(g_e.pad[0].state, TS_IDLE);
}

static void test_ts_flat_step_no_div0(void)
{
	reset();
	tick0(100);
	tick0(100);
	tick0(100);
	tick0(600);
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.on[0], 1);
}

static void test_ts_retrigger_needs_release(void)
{
	int i;
	reset();
	tick0(0);
	for (i = 0; i < 200; i++)
		tick0(1000);
	CHECK_EQ(g_r.n, 1);
}

static void test_ts_release_all(void)
{
	u16 f[64];
	reset();
	memset(f, 0, sizeof f);
	ts_tick(&g_e, f, ++g_now, note, &g_r);
	f[3] = 1000;
	f[40] = 1000;
	ts_tick(&g_e, f, ++g_now, note, &g_r);
	CHECK_EQ(g_r.n, 2);
	ts_release_all(&g_e, note, &g_r);
	CHECK_EQ(g_r.n, 4);
	CHECK_EQ(g_r.on[2], 0);
	CHECK_EQ(g_r.on[3], 0);
	CHECK_EQ(g_r.ch[2], 3);
	CHECK_EQ(g_r.ch[3], 40);
	CHECK_EQ(g_e.pad[3].state, TS_IDLE);
	CHECK_EQ(g_e.pad[40].state, TS_IDLE);
	ts_release_all(&g_e, note, &g_r);
	CHECK_EQ(g_r.n, 4);
}

/* a pad parked ARMED in [TS_REL, TS_T1) must not keep its stale t1 */
static void park_armed(void)
{
	int i;
	reset();
	tick0(0);
	tick0(150);			/* crosses T1, ARMED */
	CHECK_EQ(g_e.pad[0].state, TS_ARMED);
	for (i = 0; i < 200; i++)
		tick0(100);		/* settles in [REL, T1) for 200 ms */
	CHECK_EQ(g_e.pad[0].state, TS_ARMED);
	CHECK_EQ(g_r.n, 0);
}

static void test_ts_parked_then_hard_hit(void)
{
	park_armed();
	tick0(2000);
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.vel[0], 0xFFFF);
}

static void test_ts_parked_then_medium_ramp(void)
{
	u32 t1, t2;
	park_armed();
	tick0(300);			/* fresh T1 crossing, no note yet */
	CHECK_EQ(g_r.n, 0);
	t1 = ts_cross_q8(g_now - 1, 1, 100, 300, TS_T1);
	tick0(900);
	t2 = ts_cross_q8(g_now - 1, 1, 300, 900, TS_T2);
	CHECK_EQ(g_r.n, 1);
	CHECK(g_r.vel[0] != 0x0333);
	CHECK_EQ(g_r.vel[0], ts_velocity(t2 - t1));
}

/* samples 2 ms apart: the crossings land inside the real gap */
static void test_ts_gap_interpolates(void)
{
	u16 f[64];
	memset(f, 0, sizeof f);
	reset();
	ts_tick(&g_e, f, 10, note, &g_r);	/* prime */
	f[0] = 240;
	ts_tick(&g_e, f, 12, note, &g_r);	/* T1 (120) at half of 10..12 = 11.0 ms */
	CHECK_EQ(g_r.n, 0);
	CHECK_EQ(g_e.pad[0].state, TS_ARMED);
	CHECK_EQ(g_e.pad[0].t1_q8, 11 * 256);
	f[0] = 720;
	g_now = 14;
	ts_tick(&g_e, f, 14, note, &g_r);	/* T2 (480) at half of 12..14 = 13.0 ms */
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.on[0], 1);
	CHECK_EQ(g_r.vel[0], ts_velocity(2 * 256));	/* dt = 2 ms */
	CHECK_EQ(g_e.pad[0].state, TS_HELD);
}

static void test_ts_same_ms_ignored(void)
{
	u16 f[64];
	memset(f, 0, sizeof f);
	reset();
	ts_tick(&g_e, f, 5, note, &g_r);	/* prime */
	f[0] = 2000;
	ts_tick(&g_e, f, 5, note, &g_r);	/* same ms: not processed */
	CHECK_EQ(g_r.n, 0);
	CHECK_EQ(g_e.pad[0].state, TS_IDLE);
	CHECK_EQ(g_e.pad[0].prev, 0);		/* prev not updated */
	ts_tick(&g_e, f, 6, note, &g_r);	/* next ms sees the crossing from prev 0 */
	CHECK_EQ(g_r.n, 1);
	CHECK_EQ(g_r.vel[0], 0xFFFF);
}

void test_twostage_all(void)
{
	RUN(test_ts_velocity_curve);
	RUN(test_ts_cross_interp);
	RUN(test_ts_primed_first_frame);
	RUN(test_ts_power_up_held_rearms);
	RUN(test_ts_slow_press);
	RUN(test_ts_slow_press_across_wrap);
	RUN(test_ts_single_tick_hard_hit);
	RUN(test_ts_timeout_gentle);
	RUN(test_ts_brush_silent);
	RUN(test_ts_release_debounce);
	RUN(test_ts_flat_step_no_div0);
	RUN(test_ts_retrigger_needs_release);
	RUN(test_ts_release_all);
	RUN(test_ts_parked_then_hard_hit);
	RUN(test_ts_parked_then_medium_ramp);
	RUN(test_ts_gap_interpolates);
	RUN(test_ts_same_ms_ignored);
}
