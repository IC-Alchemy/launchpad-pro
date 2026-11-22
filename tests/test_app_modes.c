/* Checkpoint app velocity modes (src/app.c linked with recording stubs). */
#include "test.h"
#include "stubs/app_stubs.h"
#include "app_defs.h"
#include "pads/twostage.h"
#include "platform/pads.h"

int g_test_failures = 0;

int test_failures(void)
{
	return g_test_failures;
}

#define PAD 11			/* grid pad 11 = ADC channel 0 */
#define PAD_B 21		/* grid pad 21 = ADC channel 16 */
#define SETUP_ON() app_surface_event(TYPESETUP, 0, 127)
#define SETUP_OFF() app_surface_event(TYPESETUP, 0, 0)

static u16 g_frame[64];

static void reset(void)
{
	app_stubs_reset();
	memset(g_frame, 0, sizeof g_frame);
	app_init(g_frame);			/* back to the default mode, fresh state */
	app_ev_n = 0;				/* drop anything app_init sent */
}

/* run one timer tick with `ch` at `v` (everything else 0) */
static void tick(u8 ch, u16 v)
{
	g_frame[ch] = v;
	app_timer_event();
}

/* drive ADC channel `ch` through prime, a fast rise past TS_T2 and a hold */
static void press_ts(u8 ch)
{
	tick(ch, 0);		/* primes the engine */
	tick(ch, 0);
	tick(ch, TS_T1 + 10);
	tick(ch, TS_T2 + 200);
	tick(ch, TS_T2 + 200);
}

static void release_ts(u8 ch)
{
	int i;
	for (i = 0; i < TS_REL_TICKS; i++)
		tick(ch, 0);
}

/* the "library" press of grid pad `index` at ADC_Max level `peak` */
static void lib_press(u8 index, u16 peak)
{
	ADC_Max[pad_slot(index)] = peak;
	app_surface_event(TYPEPAD, index, 100);
}

static void select_zero(void)
{
	SETUP_ON();
	lib_press(11, 300);
	SETUP_OFF();
	app_ev_n = 0;
}

static void test_app_twostage_note_on_off(void)
{
	reset();
	press_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD), 1);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);
	CHECK(app_pad_led[PAD] != 0);
	release_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 1);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);
	CHECK_EQ(app_pad_led[PAD], 0);
}

static void test_app_mode_switch_releases(void)
{
	int i, offs;
	reset();
	press_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD), 1);
	SETUP_ON();
	lib_press(11, 300);			/* pad 11: Zero-latency */
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);
	offs = app_ev_count(EV_NOTE_OFF, -1);
	CHECK_EQ(offs, 1);
	SETUP_OFF();
	/* the physical release afterwards, in either path, adds nothing */
	for (i = 0; i < 5; i++)
		tick(0, 0);
	app_surface_event(TYPEPAD, 11, 0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 1);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 1);
}

static void test_app_twostage_ignores_lib_press(void)
{
	reset();
	app_surface_event(TYPEPAD, 11, 100);
	CHECK_EQ(app_ev_n, 0);
	/* also when the engine holds the pad: no duplicate Note On */
	press_ts(0);
	app_surface_event(TYPEPAD, 11, 100);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 1);
	app_surface_event(TYPEPAD, 11, 0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);	/* the library release is ignored too */
}

static void test_app_zero_latency_unchanged(void)
{
	reset();
	select_zero();
	ADC_Max[pad_slot(PAD_B)] = 512;
	app_surface_event(TYPEPAD, PAD_B, 100);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD_B), 1);
	CHECK_EQ(app_ev_log[app_ev_n - 1].value, 0x8000);
	CHECK_EQ(app_pad_led[PAD_B], 0x8000 >> 10);
	app_surface_event(TYPEPAD, PAD_B, 0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD_B), 1);
	CHECK_EQ(app_pad_led[PAD_B], 0);
	/* the engine is not running in this mode */
	app_ev_n = 0;
	press_ts(32);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 0);
}

static void test_app_setup_pad_select_silent(void)
{
	reset();
	/* Two-stage -> Zero-latency: library press of pad 11 under Setup */
	SETUP_ON();
	lib_press(11, 400);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 0);
	app_surface_event(TYPEPAD, 11, 0);
	SETUP_OFF();
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);
	/* now Zero-latency: grid press while Setup is held is only a selection */
	SETUP_ON();
	lib_press(PAD_B, 400);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 0);
	app_surface_event(TYPEPAD, PAD_B, 0);
	/* pad 12 back to Two-stage, silent too */
	lib_press(12, 400);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 0);
	SETUP_OFF();
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);
	/* and Two-stage is live again */
	press_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD), 1);
}

static void test_app_setup_suppresses_engine(void)
{
	reset();
	SETUP_ON();
	press_ts(16);				/* pad 21 hit while Setup is held */
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 0);
	release_ts(16);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);	/* no orphan Off */
	SETUP_OFF();
	/* a note sounding before Setup still gets its Off */
	press_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD), 1);
	SETUP_ON();
	release_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);
	SETUP_OFF();
}

static void test_app_aftertouch_only_held(void)
{
	reset();
	app_aftertouch_event(PAD, 64);
	CHECK_EQ(app_ev_count(EV_POLY, -1), 0);
	press_ts(0);
	app_aftertouch_event(PAD, 64);
	CHECK_EQ(app_ev_count(EV_POLY, PAD), 1);
	app_aftertouch_event(PAD_B, 64);
	app_aftertouch_event(200, 64);			/* out of range */
	CHECK_EQ(app_ev_count(EV_POLY, -1), 1);
	release_ts(0);
	app_aftertouch_event(PAD, 64);
	CHECK_EQ(app_ev_count(EV_POLY, -1), 1);
}

static void test_app_zero_latency_switch_releases(void)
{
	reset();
	select_zero();
	lib_press(PAD, 600);
	lib_press(PAD_B, 600);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, -1), 2);
	SETUP_ON();
	lib_press(12, 300);			/* back to Two-stage */
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD_B), 1);
	SETUP_OFF();
	app_surface_event(TYPEPAD, PAD, 0);		/* late library releases */
	app_surface_event(TYPEPAD, PAD_B, 0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 2);
	app_aftertouch_event(PAD, 10);
	CHECK_EQ(app_ev_count(EV_POLY, -1), 0);
}

static void test_app_zero_latency_release_under_setup(void)
{
	reset();
	select_zero();
	lib_press(PAD_B, 600);
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD_B), 1);
	SETUP_ON();
	app_surface_event(TYPEPAD, PAD_B, 0);	/* released while Setup is held */
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD_B), 1);
	CHECK_EQ(app_pad_led[PAD_B], 0);
	SETUP_OFF();
	lib_press(PAD_B, 600);			/* no stuck state: a clean new press */
	CHECK_EQ(app_ev_count(EV_NOTE_ON, PAD_B), 2);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 1);
}

static void test_app_mode_led_kept_on_release(void)
{
	/* Two-stage: pad 11 sounding, Setup held, engine releases it */
	reset();
	press_ts(0);
	SETUP_ON();
	release_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);
	CHECK(app_pad_led[11] != 0);
	CHECK(app_pad_led[12] > app_pad_led[11]);
	SETUP_OFF();
	CHECK_EQ(app_pad_led[11], 0);
	/* Zero-latency: pad 12 sounding, released under Setup */
	select_zero();
	lib_press(12, 600);
	SETUP_ON();
	app_surface_event(TYPEPAD, 12, 0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, 12), 1);
	CHECK(app_pad_led[12] != 0);
	CHECK(app_pad_led[11] > app_pad_led[12]);
	SETUP_OFF();
	CHECK_EQ(app_pad_led[12], 0);
}

static void test_app_same_mode_noop(void)
{
	reset();
	press_ts(0);
	SETUP_ON();
	lib_press(12, 300);			/* already Two-stage */
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, -1), 0);
	SETUP_OFF();
	release_ts(0);
	CHECK_EQ(app_ev_count(EV_NOTE_OFF, PAD), 1);	/* still held by the engine */
}

static void test_app_setup_leds_and_critical_section(void)
{
	reset();
	SETUP_ON();
	CHECK(app_pad_led[12] > app_pad_led[11]);	/* Two-stage selected: bright */
	CHECK(app_pad_led[11] != 0);
	lib_press(11, 300);
	CHECK(app_pad_led[11] > app_pad_led[12]);
	CHECK(app_pad_led[12] != 0);
	CHECK_EQ(stub_irq_depth, 0);
	SETUP_OFF();
	CHECK_EQ(app_pad_led[11], 0);
	CHECK_EQ(app_pad_led[12], 0);
	CHECK_EQ(stub_irq_depth, 0);
	CHECK_EQ(app_pause_rx_last, 0);
	CHECK(app_pause_rx_calls >= 2);
}

static void test_app_timer_before_init(void)
{
	/* g_ADC is null before app_init: must not crash (fresh process state is
	 * not available here, so only check the guard path via a null init) */
	app_init(0);
	app_timer_event();
	CHECK(1);
}

int main(void)
{
	RUN(test_app_twostage_note_on_off);
	RUN(test_app_mode_switch_releases);
	RUN(test_app_twostage_ignores_lib_press);
	RUN(test_app_zero_latency_unchanged);
	RUN(test_app_setup_pad_select_silent);
	RUN(test_app_setup_suppresses_engine);
	RUN(test_app_aftertouch_only_held);
	RUN(test_app_zero_latency_switch_releases);
	RUN(test_app_zero_latency_release_under_setup);
	RUN(test_app_mode_led_kept_on_release);
	RUN(test_app_same_mode_noop);
	RUN(test_app_setup_leds_and_critical_section);
	RUN(test_app_timer_before_init);
	printf("%d failures\n", test_failures());
	return test_failures() ? 1 : 0;
}
