#include "test.h"
#include "instrument/ui.h"
#include "instrument/settings.h"
#include "midici/midici.h"
#include "midici/responder.h"
#include "midici/initiator.h"
#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "scales/scales.h"
#include "ump/ump.h"
#include "stubs/io_stub.h"
#include "stubs/stubs.h"
#include "app_defs.h"
#include <string.h>

#define T0 1000u

static void setup(void)
{
	io_stub_reset();
	pe_init(io_adc, ui_on_sounding);
	ui_init(T0);
}

static int led_is(uint8_t i, uint8_t r, uint8_t g, uint8_t b)
{
	return io_leds[i][0] == r && io_leds[i][1] == g && io_leds[i][2] == b;
}

static void tap(uint8_t i, uint32_t now)
{
	ui_pad(i, 127, now);
	ui_pad(i, 0, now);
}

static void setup_tap(uint32_t now)
{
	ui_setup(127, now);
	ui_setup(0, now + 50);
}

static void zero_latency(uint32_t now)
{
	struct settings s = *ui_settings();

	s.velocity_mode = VEL_ZERO_LATENCY;
	ui_apply_settings(&s, now);
}

static int count_kind(int from, char k)
{
	int i, n = 0;

	for (i = from; i < io_n; i++)
		if (io_log[i].kind == k)
			n++;
	return n;
}

static void test_play_page_colours_default(void)
{
	setup();
	CHECK(led_is(11, 0, 20, 63));  /* C: root */
	CHECK(led_is(12, 0, 4, 16));   /* D: scale */
	CHECK(led_is(13, 8, 24, 40));  /* E: 3rd */
	CHECK(led_is(15, 8, 24, 40));  /* G: 5th */
	CHECK(led_is(91, 6, 6, 6));
	CHECK(led_is(92, 6, 6, 6));
	CHECK(led_is(93, 6, 6, 6));
	CHECK(led_is(94, 6, 6, 6));
	CHECK(led_is(95, 0, 0, 0));
	CHECK(led_is(98, 0, 0, 0));
	CHECK_EQ(ui_menu_open(), 0);
}

static void test_initial_draw_clears_unused_leds(void)
{
	io_stub_reset();
	memset(io_leds, 33, sizeof io_leds);
	pe_init(io_adc, ui_on_sounding);
	ui_init(T0);
	CHECK(led_is(10, 0, 0, 0));
	CHECK(led_is(80, 0, 0, 0));
	CHECK(led_is(19, 0, 0, 0));
	CHECK(led_is(89, 0, 0, 0));
	CHECK(led_is(1, 0, 0, 0));
	CHECK(led_is(8, 0, 0, 0));
	CHECK(led_is(95, 0, 0, 0));
	CHECK(led_is(96, 0, 0, 0));
	CHECK(led_is(97, 0, 0, 0));
	CHECK(led_is(98, 0, 0, 0));
	CHECK(led_is(11, 0, 20, 63));
}

static void test_tuned_notes_carry_their_pitch(void)
{
	struct settings s;

	setup();
	zero_latency(T0);
	/* 12-EDO at 440: the note number says it all */
	ui_pad(11, 100, T0 + 10);
	ui_pad(11, 0, T0 + 20);
	CHECK_EQ(io_log[0].note, 60);
	CHECK_EQ(io_log[0].has_pitch, 0);

	/* 24-EDO Rast: a quarter-tone a note, the pitch rides on the Note On;
	 * DIN gets the plain note */
	s = *ui_settings();
	s.tuning = 1;
	s.scale = 32;
	s.din_mirror = 1;
	ui_apply_settings(&s, T0 + 30);
	io_n = 0;
	ui_pad(12, 100, T0 + 40);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[0].port, USBSTANDALONE);
	CHECK_EQ(io_log[0].note, 64);
	CHECK_EQ(io_log[0].has_pitch, 1);
	CHECK_EQ(io_log[0].pitch, 62 * 512);
	CHECK_EQ(io_log[1].port, DINMIDI);
	CHECK_EQ(io_log[1].note, 64);
	CHECK_EQ(io_log[1].has_pitch, 0);
	ui_pad(12, 0, T0 + 50);

	/* 12-EDO at 432 Hz: same notes, lower pitch */
	s.tuning = 0;
	s.scale = 0;
	s.din_mirror = 0;
	s.a4_tenths = 4320;
	ui_apply_settings(&s, T0 + 60);
	io_n = 0;
	ui_pad(11, 100, T0 + 70);
	CHECK_EQ(io_log[0].note, 60);
	CHECK_EQ(io_log[0].has_pitch, 1);
	CHECK_EQ(io_log[0].pitch, (60 * 32768 - 10409 + 32) >> 6);
}

static void test_press_plays_mapped_note(void)
{
	setup();
	zero_latency(T0);
	ui_pad(11, 100, T0 + 10);
	CHECK_EQ(io_n, 1);
	CHECK_EQ(io_log[0].kind, 'N');
	CHECK_EQ(io_log[0].port, USBSTANDALONE);
	CHECK_EQ(io_log[0].ch, 0);
	CHECK_EQ(io_log[0].note, 60);
	ui_pad(11, 0, T0 + 20);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[1].kind, 'F');
	CHECK_EQ(io_log[1].note, 60);
}

static void test_same_pitch_pads_light_together(void)
{
	setup();
	zero_latency(T0);
	ui_pad(25, 100, T0 + 10);
	CHECK_EQ(io_log[0].note, 72);
	CHECK(led_is(18, 63, 63, 63));
	CHECK(led_is(25, 63, 63, 63));
	CHECK(led_is(32, 63, 63, 63));
	CHECK(led_is(11, 0, 20, 63));  /* 60 is not sounding */
	ui_pad(25, 0, T0 + 20);
	CHECK(led_is(18, 0, 20, 63));
	CHECK(led_is(25, 0, 20, 63));
	CHECK(led_is(32, 0, 20, 63));
}

static void test_peak_window_lights_when_sounding(void)
{
	setup();
	ui_pad(11, 100, T0);
	CHECK_EQ(io_n, 0);
	CHECK(led_is(11, 0, 20, 63));  /* capturing: not sounding yet */
	ui_poll(T0 + PEAK_WINDOW_MS);
	CHECK_EQ(count_kind(0, 'N'), 1);
	CHECK(led_is(11, 63, 63, 63));
}

static void test_octave_arrows_clamp(void)
{
	int i;

	setup();
	for (i = 0; i < 5; i++)
		tap(91, T0 + i);
	CHECK_EQ(ui_settings()->octave, 7);
	for (i = 0; i < 9; i++)
		tap(92, T0 + 10 + i);
	CHECK_EQ(ui_settings()->octave, 0);
	CHECK(led_is(11, 0, 20, 63));
	CHECK_EQ(io_n, 0);
}

static void test_root_arrows_wrap(void)
{
	setup();
	tap(93, T0);
	CHECK_EQ(ui_settings()->root, 11);
	tap(94, T0 + 1);
	CHECK_EQ(ui_settings()->root, 0);
	tap(94, T0 + 2);
	CHECK_EQ(ui_settings()->root, 1);
	CHECK(led_is(11, 0, 20, 63));  /* the bottom-left pad is still the root */
}

static void test_arrows_keep_held_note(void)
{
	setup();
	zero_latency(T0);
	ui_pad(11, 100, T0 + 1);
	tap(91, T0 + 2);
	tap(94, T0 + 3);
	CHECK_EQ(io_n, 1);           /* no note off from the change */
	ui_pad(11, 0, T0 + 4);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[1].kind, 'F');
	CHECK_EQ(io_log[1].note, 60); /* the latched pitch */
}

static void test_setup_tap_opens_and_closes(void)
{
	setup();
	ui_setup(127, T0);
	CHECK_EQ(ui_menu_open(), 1);
	ui_setup(0, T0 + 50);
	CHECK_EQ(ui_menu_open(), 1);
	ui_setup(127, T0 + 500);
	CHECK_EQ(ui_menu_open(), 0);
	ui_setup(0, T0 + 1500);       /* a long release of a closing press */
	CHECK_EQ(ui_menu_open(), 0);
	CHECK(led_is(11, 0, 20, 63));
	CHECK(led_is(91, 6, 6, 6));
}

static void test_setup_peek(void)
{
	setup();
	ui_setup(127, T0);
	CHECK_EQ(ui_menu_open(), 1);
	ui_setup(0, T0 + 400);
	CHECK_EQ(ui_menu_open(), 0);
	CHECK(led_is(11, 0, 20, 63));
}

static void test_menu_entry_releases_notes(void)
{
	setup();
	ui_pad(11, 100, T0);
	ui_poll(T0 + PEAK_WINDOW_MS);
	CHECK_EQ(io_n, 1);
	CHECK_EQ(io_log[0].kind, 'N');
	ui_setup(127, T0 + 10);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[1].kind, 'F');
	CHECK_EQ(io_log[1].note, 60);
}

static void test_menu_drawing(void)
{
	setup();
	setup_tap(T0);
	CHECK(led_is(95, 63, 63, 63));
	CHECK(led_is(96, 6, 6, 6));
	CHECK(led_is(97, 6, 6, 6));
	CHECK(led_is(91, 0, 0, 0));
	CHECK(led_is(94, 0, 0, 0));
	CHECK(led_is(61, 0, 63, 0));   /* root C selected */
	CHECK(led_is(62, 0, 8, 0));
	CHECK(led_is(72, 0, 8, 0));
	CHECK(led_is(71, 0, 0, 0));
	CHECK(led_is(55, 63, 0, 0));   /* octave 4 */
	CHECK(led_is(51, 8, 0, 0));
	CHECK(led_is(41, 0, 0, 63));   /* Major */
	CHECK(led_is(11, 0, 0, 8));    /* scale 24 */
	CHECK(led_is(17, 0, 0, 8));    /* scale 30, Chromatic */
	CHECK(led_is(18, 0, 0, 0));
	CHECK(led_is(86, 63, 24, 0));  /* In-key 4ths */
	CHECK(led_is(83, 8, 3, 0));
	CHECK(led_is(81, 8, 3, 0));    /* absolute root off */
	CHECK(led_is(82, 0, 0, 0));
}

static void test_tuning_page(void)
{
	setup();
	setup_tap(T0);
	tap(97, T0 + 10);
	CHECK(led_is(97, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
	CHECK(led_is(81, 0, 48, 63));  /* 12-EDO selected */
	CHECK(led_is(82, 0, 6, 8));    /* 24-EDO */
	CHECK(led_is(75, 8, 7, 0));    /* Just Intonation */
	CHECK(led_is(67, 8, 0, 6));    /* 22 Shruti */
	CHECK(led_is(55, 4, 0, 8));    /* Carlos Gamma, the 29th */
	CHECK(led_is(56, 0, 0, 0));
	CHECK(led_is(15, 63, 63, 63)); /* 440 */
	CHECK(led_is(13, 6, 6, 6));
	CHECK(led_is(21, 6, 6, 6));
	CHECK(led_is(23, 0, 0, 0));
	tap(33, T0 + 20);              /* nothing on this page */
	CHECK_EQ(ui_settings()->scale, 0);

	/* a tuning that doesn't offer the scale takes its default */
	tap(82, T0 + 30);
	CHECK_EQ(ui_settings()->tuning, 1);
	CHECK_EQ(ui_settings()->scale, 32);
	CHECK(led_is(82, 0, 48, 63));
	CHECK(led_is(81, 0, 6, 8));

	/* A4: steps clamp at the range, presets jump */
	tap(28, T0 + 40);
	CHECK_EQ(ui_settings()->a4_tenths, 4410);
	CHECK(led_is(15, 6, 6, 6));
	tap(21, T0 + 50);
	tap(22, T0 + 60);
	CHECK_EQ(ui_settings()->a4_tenths, 4399);
	tap(27, T0 + 70);
	CHECK_EQ(ui_settings()->a4_tenths, 4400);
	CHECK(led_is(15, 63, 63, 63));
	tap(13, T0 + 80);
	CHECK_EQ(ui_settings()->a4_tenths, 4150);
	tap(22, T0 + 90);
	CHECK_EQ(ui_settings()->a4_tenths, 4150);
	tap(16, T0 + 100);
	CHECK_EQ(ui_settings()->a4_tenths, 4420);
	tap(14, T0 + 110);
	CHECK_EQ(ui_settings()->a4_tenths, 4320);

	/* the Scale page offers the tuning's scales */
	tap(95, T0 + 120);
	CHECK(led_is(41, 0, 0, 63));   /* Maqam Rast */
	CHECK(led_is(45, 0, 0, 8));    /* All Degrees */
	CHECK(led_is(46, 0, 0, 0));
	tap(42, T0 + 130);
	CHECK_EQ(ui_settings()->scale, 33);
	tap(46, T0 + 140);
	CHECK_EQ(ui_settings()->scale, 33);

	/* back to 12-EDO keeps a scale it offers, else Major */
	tap(97, T0 + 150);
	tap(81, T0 + 160);
	CHECK_EQ(ui_settings()->tuning, 0);
	CHECK_EQ(ui_settings()->scale, 0);
	CHECK_EQ(io_n, 0);
	ui_poll(T0 + 160 + SETTINGS_SAVE_QUIET_MS);
	CHECK_EQ(io_flash_writes, 1);
}

static void test_scale_page_select(void)
{
	setup();
	setup_tap(T0);
	tap(33, T0 + 100);
	CHECK_EQ(ui_settings()->scale, 10);
	CHECK(led_is(33, 0, 0, 63));
	CHECK(led_is(41, 0, 0, 8));
	tap(75, T0 + 110);
	CHECK_EQ(ui_settings()->root, 6);
	CHECK(led_is(75, 0, 63, 0));
	CHECK(led_is(61, 0, 8, 0));
	tap(84, T0 + 120);
	CHECK_EQ(ui_settings()->layout, LAYOUT_INKEY_SEQ);
	CHECK(led_is(84, 63, 24, 0));
	CHECK(led_is(86, 8, 3, 0));
	tap(81, T0 + 130);
	CHECK_EQ(ui_settings()->absolute_root, 1);
	CHECK(led_is(81, 63, 24, 0));
	tap(81, T0 + 140);
	CHECK_EQ(ui_settings()->absolute_root, 0);
	tap(52, T0 + 150);
	CHECK_EQ(ui_settings()->octave, 1);
	tap(11, T0 + 160);
	CHECK_EQ(ui_settings()->scale, 24);
	tap(67, T0 + 170);
	CHECK_EQ(ui_settings()->root, 11);
	tap(18, T0 + 180);             /* unused pad */
	CHECK_EQ(ui_settings()->scale, 24);
	CHECK_EQ(io_n, 0);             /* pads don't reach the engine */
	CHECK_EQ(ui_menu_open(), 1);
}

static void test_velocity_row_columns(void)
{
	static const uint8_t col_val[4] = {VEL_PEAK_WINDOW, VEL_ZERO_LATENCY,
	                                   VEL_TWO_STAGE, VEL_FIXED};
	int c, k;

	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	for (c = 0; c < 4; c++) {
		tap((uint8_t)(81 + c), T0 + 110 + c);
		CHECK_EQ(ui_settings()->velocity_mode, col_val[c]);
		for (k = 0; k < 4; k++) {
			if (k == c)
				CHECK(led_is((uint8_t)(81 + k), 63, 48, 36));
			else
				CHECK(led_is((uint8_t)(81 + k), 6, 5, 3));
		}
	}
	/* unused columns do nothing */
	tap(85, T0 + 200);
	CHECK_EQ(ui_settings()->velocity_mode, VEL_FIXED);
	CHECK(led_is(85, 0, 0, 0));
	/* curve and pressure mode stay 3 columns */
	tap(74, T0 + 210);
	CHECK_EQ(ui_settings()->velocity_curve, CURVE_LINEAR);
	CHECK(led_is(74, 0, 0, 0));
	tap(64, T0 + 220);
	CHECK_EQ(ui_settings()->pressure_mode, PRESS_POLY);
	/* the threshold row has a fourth column: instant aftertouch */
	tap(55, T0 + 225);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_MED);
	CHECK(led_is(55, 0, 0, 0));
	tap(54, T0 + 230);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_INSTANT);
	CHECK(!led_is(54, 0, 0, 0));
}

static void test_options_page_select(void)
{
	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	CHECK(led_is(96, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
	CHECK(led_is(81, 63, 48, 36));  /* Peak-window */
	CHECK(led_is(21, 44, 63, 40));  /* channel 1 */
	CHECK(led_is(31, 3, 5, 6));     /* None, unselected */
	tap(82, T0 + 110);
	CHECK_EQ(ui_settings()->velocity_mode, VEL_ZERO_LATENCY);
	CHECK(led_is(82, 63, 48, 36));
	CHECK(led_is(81, 6, 5, 3));
	/* 11..18 = channels 9..16: pad 12 is channel 10 (stored 9) */
	tap(12, T0 + 120);
	CHECK_EQ(ui_settings()->channel, 9);
	CHECK(led_is(12, 44, 63, 40));
	CHECK(led_is(21, 4, 6, 4));
	tap(13, T0 + 125);
	CHECK_EQ(ui_settings()->channel, 10);
	tap(18, T0 + 126);
	CHECK_EQ(ui_settings()->channel, 15);
	tap(73, T0 + 130);
	CHECK_EQ(ui_settings()->velocity_curve, CURVE_HARD);
	tap(62, T0 + 140);
	CHECK_EQ(ui_settings()->pressure_mode, PRESS_CHANNEL);
	tap(54, T0 + 145);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_INSTANT);
	tap(51, T0 + 150);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_LOW);
	tap(42, T0 + 160);
	CHECK_EQ(ui_settings()->din_mirror, 1);
	CHECK(led_is(42, 44, 63, 40));
	tap(28, T0 + 170);
	CHECK_EQ(ui_settings()->channel, 7);
	CHECK_EQ(io_n, 0);

	/* the new options apply on the play page: zero latency, channel 8 */
	setup_tap(T0 + 200);
	CHECK_EQ(ui_menu_open(), 0);
	ui_pad(11, 100, T0 + 300);
	CHECK_EQ(count_kind(0, 'N'), 2);  /* Standalone and DIN */
	CHECK_EQ(io_log[0].ch, 7);
	CHECK_EQ(io_log[0].note, 60);
}

static void test_smoothing_row_columns(void)
{
	static const uint8_t col_val[4] = {SMOOTH_NONE, SMOOTH_LIGHT, SMOOTH_MEDIUM,
	                                   SMOOTH_HEAVY};
	int c, k;

	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	CHECK_EQ(ui_settings()->pressure_smoothing, SMOOTH_LIGHT);
	for (c = 0; c < 4; c++) {
		tap((uint8_t)(31 + c), T0 + 110 + c);
		CHECK_EQ(ui_settings()->pressure_smoothing, col_val[c]);
		for (k = 0; k < 4; k++) {
			if (k == c)
				CHECK(led_is((uint8_t)(31 + k), 36, 52, 63));
			else
				CHECK(led_is((uint8_t)(31 + k), 3, 5, 6));
		}
	}
	/* pads 35..38 do nothing */
	for (c = 5; c <= 8; c++) {
		tap((uint8_t)(30 + c), T0 + 200 + c);
		CHECK_EQ(ui_settings()->pressure_smoothing, SMOOTH_HEAVY);
		CHECK(led_is((uint8_t)(30 + c), 0, 0, 0));
	}
	CHECK_EQ(io_n, 0);
}

static void test_options_section_hues(void)
{
	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	/* velocity: warm (mode row 8, curve row 7) */
	CHECK(led_is(81, 63, 48, 36));
	CHECK(led_is(82, 6, 5, 3));
	CHECK(led_is(72, 63, 48, 36));   /* Linear selected */
	CHECK(led_is(71, 6, 5, 3));
	/* aftertouch: cool (mode row 6, threshold row 5, smoothing row 3) */
	CHECK(led_is(61, 36, 52, 63));
	CHECK(led_is(62, 3, 5, 6));
	CHECK(led_is(52, 36, 52, 63));   /* Med selected */
	CHECK(led_is(51, 3, 5, 6));
	CHECK(led_is(32, 36, 52, 63));   /* Light selected */
	CHECK(led_is(31, 3, 5, 6));
	/* MIDI routing: soft green (DIN row 4, channel rows 2 and 1) */
	CHECK(led_is(41, 44, 63, 40));
	CHECK(led_is(42, 4, 6, 4));
	CHECK(led_is(21, 44, 63, 40));
	CHECK(led_is(22, 4, 6, 4));
	CHECK(led_is(11, 4, 6, 4));
	/* page buttons stay pure white */
	CHECK(led_is(96, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
}

static void test_menu_reopens_on_last_page(void)
{
	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	setup_tap(T0 + 200);
	CHECK_EQ(ui_menu_open(), 0);
	setup_tap(T0 + 300);
	CHECK_EQ(ui_menu_open(), 1);
	CHECK(led_is(96, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
}

static void test_pad_held_across_menu_is_silent(void)
{
	int mark;

	setup();
	ui_pad(45, 100, T0);
	ui_poll(T0 + PEAK_WINDOW_MS);
	CHECK_EQ(count_kind(0, 'N'), 1);
	ui_setup(127, T0 + 10);
	ui_setup(0, T0 + 60);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[1].kind, 'F');
	mark = io_n;
	ui_poll(T0 + 70);
	ui_setup(127, T0 + 100);
	ui_setup(0, T0 + 150);
	CHECK_EQ(ui_menu_open(), 0);
	ui_poll(T0 + 160);
	ui_pad(45, 0, T0 + 200);
	ui_poll(T0 + 210);
	ui_poll(T0 + 220);
	CHECK_EQ(io_n, mark);
}

static void test_settings_saved_after_quiet(void)
{
	setup();
	tap(91, T0 + 100);
	ui_poll(T0 + 100 + SETTINGS_SAVE_QUIET_MS - 1);
	CHECK_EQ(io_flash_writes, 0);
	ui_poll(T0 + 100 + 2000);
	CHECK_EQ(io_flash_writes, 1);
	ui_poll(T0 + 100 + 5000);
	CHECK_EQ(io_flash_writes, 1);
}

/* poll every ms over [from, to] */
static void poll_span(uint32_t from, uint32_t to)
{
	uint32_t t;

	for (t = from; t <= to; t++)
		ui_poll(t);
}

static void test_settings_save_waits_for_playing_to_stop(void)
{
	uint32_t t0 = T0 + 100, t, last = 0;

	/* an octave change, then a short tap every 500 ms for 5 s: the save
	 * (a flash page erase that stalls the device) never lands mid-phrase */
	setup();
	tap(91, t0);
	for (t = t0 + 1; t <= t0 + 5000; t++) {
		if ((t - t0) % 500 == 0) {
			tap(11, t);
			last = t;
		}
		ui_poll(t);
	}
	CHECK_EQ(io_flash_writes, 0);
	/* then 2 s of silence: exactly one write, at the end of it */
	poll_span(t0 + 5001, last + SETTINGS_SAVE_QUIET_MS - 1);
	CHECK_EQ(io_flash_writes, 0);
	poll_span(last + SETTINGS_SAVE_QUIET_MS, last + SETTINGS_SAVE_QUIET_MS + 3000);
	CHECK_EQ(io_flash_writes, 1);
	CHECK_EQ(io_flash[0 + 5 + 2], 5);  /* the octave byte: 4 + 1 */
}

static void test_settings_save_in_menu_ignores_earlier_playing(void)
{
	uint32_t t0 = T0 + 100;

	/* an octave change, a tap on the play page, then the menu opens (no
	 * note can sound there): the save follows the change, not the tap */
	setup();
	tap(91, t0);
	tap(11, t0 + 1500);
	ui_poll(t0 + 1500);
	setup_tap(t0 + 1600);
	CHECK_EQ(ui_menu_open(), 1);
	poll_span(t0 + 1600, t0 + SETTINGS_SAVE_QUIET_MS - 1);
	CHECK_EQ(io_flash_writes, 0);
	ui_poll(t0 + SETTINGS_SAVE_QUIET_MS);
	CHECK_EQ(io_flash_writes, 1);
}

static void test_apply_settings_rejects_invalid(void)
{
	struct settings s;
	uint8_t leds[100][3];

	setup();
	s = *ui_settings();
	memcpy(leds, io_leds, sizeof leds);
	s.scale = SCALE_ALL_DEGREES; /* 12-EDO doesn't offer it */
	ui_apply_settings(&s, T0 + 10);
	CHECK_EQ(ui_settings()->scale, 0);
	CHECK_MEM(leds, io_leds, sizeof leds);
	ui_poll(T0 + 10 + 3000);
	CHECK_EQ(io_flash_writes, 0);
}

static void test_apply_settings_change(void)
{
	struct settings s;

	setup();
	zero_latency(T0);
	ui_pad(11, 100, T0 + 10);
	CHECK_EQ(io_n, 1);
	s = *ui_settings();
	ui_apply_settings(&s, T0 + 20);   /* identical: no-op */
	CHECK_EQ(io_n, 1);
	s.root = 2;
	ui_apply_settings(&s, T0 + 30);
	CHECK_EQ(io_n, 2);
	CHECK_EQ(io_log[1].kind, 'F');
	CHECK_EQ(ui_settings()->root, 2);
	CHECK(led_is(11, 0, 20, 63));     /* redrawn, no longer white */
	ui_pad(11, 0, T0 + 40);           /* released by the apply: silent */
	CHECK_EQ(io_n, 2);
	/* the save waits for quiet after the last pad event (the release at
	 * T0 + 40), which comes after the change */
	ui_poll(T0 + 40 + SETTINGS_SAVE_QUIET_MS - 1);
	CHECK_EQ(io_flash_writes, 0);
	ui_poll(T0 + 40 + SETTINGS_SAVE_QUIET_MS);
	CHECK_EQ(io_flash_writes, 1);
}

static void test_setup_led_reflects_protocol(void)
{
	setup();
	io_midi2[0] = 1;
	ui_poll(T0 + 100);
	CHECK_EQ(io_setup_led[0], 0);
	CHECK_EQ(io_setup_led[1], 63);
	CHECK_EQ(io_setup_led[2], 0);
	io_midi2[0] = 0;
	ui_poll(T0 + 200);
	CHECK_EQ(io_setup_led[0], 63);
	CHECK_EQ(io_setup_led[1], 20);
	CHECK_EQ(io_setup_led[2], 0);
}

static void test_transport_change_forgets_held_notes(void)
{
	int mark;

	setup();
	zero_latency(T0);
	ui_pad(25, 100, T0 + 10);
	CHECK(led_is(18, 63, 63, 63));
	CHECK(led_is(25, 63, 63, 63));
	mark = io_n;
	ui_transport_changed(T0 + 20);
	CHECK_EQ(io_n, mark);              /* the transport ended the note */
	CHECK(led_is(18, 0, 20, 63));
	CHECK(led_is(25, 0, 20, 63));
	CHECK(led_is(32, 0, 20, 63));
	ui_poll(T0 + 30);
	ui_pad(25, 0, T0 + 40);            /* the held pad's release: silent */
	CHECK_EQ(io_n, mark);
	/* the same pitch on another pad plays at once */
	ui_pad(18, 100, T0 + 50);
	CHECK_EQ(io_n, mark + 1);
	CHECK_EQ(io_log[mark].kind, 'N');
	CHECK_EQ(io_log[mark].note, 72);
	CHECK(led_is(25, 63, 63, 63));
	ui_pad(18, 0, T0 + 60);
	CHECK_EQ(io_n, mark + 2);
	CHECK_EQ(io_log[mark + 1].kind, 'F');

	/* in the menu: the menu stays drawn */
	setup();
	setup_tap(T0);
	ui_transport_changed(T0 + 100);
	CHECK_EQ(ui_menu_open(), 1);
	CHECK(led_is(95, 63, 63, 63));
	CHECK(led_is(61, 0, 63, 0));
	CHECK_EQ(io_n, 0);
}

static void ts_two_stage(uint32_t now)
{
	struct settings s = *ui_settings();

	s.velocity_mode = VEL_TWO_STAGE;
	ui_apply_settings(&s, now);
}

/* raw ramp on pad 45 at 10 per ms from `from`, one ui_poll per ms */
static uint32_t ts_ramp(uint32_t t, uint16_t from, uint16_t to)
{
	uint16_t v;

	for (v = from; v <= to; v = (uint16_t)(v + 10)) {
		io_adc[pad_raw_index(45)] = v;
		ui_poll(++t);
	}
	return t;
}

static void test_two_stage_plays_scale_pitch(void)
{
	struct scale_ctx c;
	const struct settings *st;
	uint32_t t = T0 + 10;

	setup();
	ts_two_stage(T0);
	memset(io_adc, 0, sizeof io_adc);
	ui_poll(t);
	st = ui_settings();
	c.root = st->root;
	c.scale = st->scale;
	c.octave = st->octave;
	c.layout = st->layout;
	c.absolute_root = st->absolute_root;
	t = ts_ramp(t, 0, 700);
	CHECK_EQ(count_kind(0, 'N'), 1);
	CHECK_EQ(io_log[0].note, scale_pad_pitch(&c, 45));
	CHECK_EQ(io_log[0].ch, st->channel);
	/* library press/release from ui_pad do nothing in this mode */
	ui_pad(46, 100, t);
	ui_pad(46, 0, t + 1);
	CHECK_EQ(count_kind(0, 'N'), 1);
}

static void test_two_stage_no_note_while_menu_open(void)
{
	uint32_t t = T0 + 10;

	setup();
	ts_two_stage(T0);
	memset(io_adc, 0, sizeof io_adc);
	ui_poll(t);
	setup_tap(t + 1);
	CHECK_EQ(ui_menu_open(), 1);
	t = ts_ramp(t + 100, 0, 700);
	CHECK_EQ(count_kind(0, 'N'), 0);
}

static void setup_ci(void)
{
	stubs_reset();
	ump_rx_reset();
	ump_tx_reset();
	midici_init();
	midici_responder_init();
	midici_initiator_init();
	setup();
}

static void drain_ci_tx(void)
{
	uint8_t pkt[64];

	while (midici_tx_pending()) {
		midici_tx_drain(100);
		while (ump_tx_fill_packet(pkt) > 0) {}
	}
}

static void ui_write_muid(uint8_t *p, uint32_t m)
{
	p[0] = (uint8_t)(m & 0x7Fu);
	p[1] = (uint8_t)((m >> 7) & 0x7Fu);
	p[2] = (uint8_t)((m >> 14) & 0x7Fu);
	p[3] = (uint8_t)((m >> 21) & 0x7Fu);
}

static void feed_peer_discovery(uint32_t peer_muid, uint32_t my_muid, uint8_t cat, uint32_t now)
{
	uint8_t msg[31];
	uint16_t n = 0;

	msg[n++] = 0x7E; msg[n++] = 0x7F; msg[n++] = 0x0D; msg[n++] = 0x71; msg[n++] = 0x02;
	ui_write_muid(msg + n, peer_muid); n += 4;
	ui_write_muid(msg + n, my_muid); n += 4;
	msg[n++] = 0x00; msg[n++] = 0x20; msg[n++] = 0x33;
	msg[n++] = 0x10; msg[n++] = 0x00;
	msg[n++] = 0x20; msg[n++] = 0x00;
	msg[n++] = 1; msg[n++] = 0; msg[n++] = 0; msg[n++] = 0;
	msg[n++] = cat;
	ui_write_muid(msg + n, 512u); n += 4;
	msg[n++] = 0;
	msg[n++] = 0;
	CHECK(midici_initiator_on_message(0, msg, n, now));
}

static void feed_peer_endpoint(uint32_t peer_muid, uint32_t my_muid, uint32_t now)
{
	uint8_t msg[24];
	uint16_t n = 0;

	msg[n++] = 0x7E; msg[n++] = 0x7F; msg[n++] = 0x0D; msg[n++] = 0x73; msg[n++] = 0x02;
	ui_write_muid(msg + n, peer_muid); n += 4;
	ui_write_muid(msg + n, my_muid); n += 4;
	msg[n++] = 0x00;
	msg[n++] = 4; msg[n++] = 0;
	msg[n++] = 'P'; msg[n++] = 'E'; msg[n++] = 'E'; msg[n++] = 'R';
	CHECK(midici_initiator_on_message(0, msg, n, now));
}

static void feed_peer_pe_caps_and_device_info(uint32_t peer_muid, uint32_t my_muid, uint32_t now)
{
	uint8_t msg[96];
	uint16_t n = 0;
	const char *hdr = "{\"status\":200}";
	const char *body = "{\"manufacturerId\":[0,32,41]}";
	uint16_t hl = (uint16_t)strlen(hdr);
	uint16_t dl = (uint16_t)strlen(body);

	midici_initiator_tick(now + 1u);
	midici_initiator_tick(now + 2u);
	drain_ci_tx();

	msg[n++] = 0x7E; msg[n++] = 0x7F; msg[n++] = 0x0D; msg[n++] = 0x31; msg[n++] = 0x02;
	ui_write_muid(msg + n, peer_muid); n += 4;
	ui_write_muid(msg + n, my_muid); n += 4;
	msg[n++] = 1; msg[n++] = 0; msg[n++] = 0;
	CHECK(midici_initiator_on_message(0, msg, n, now + 3u));

	midici_initiator_tick(now + 4u);
	drain_ci_tx();

	n = 0;
	msg[n++] = 0x7E; msg[n++] = 0x7F; msg[n++] = 0x0D; msg[n++] = 0x35; msg[n++] = 0x02;
	ui_write_muid(msg + n, peer_muid); n += 4;
	ui_write_muid(msg + n, my_muid); n += 4;
	msg[n++] = 1;
	msg[n++] = (uint8_t)hl; msg[n++] = 0;
	memcpy(msg + n, hdr, hl); n += hl;
	msg[n++] = 1; msg[n++] = 0;
	msg[n++] = 1; msg[n++] = 0;
	msg[n++] = (uint8_t)dl; msg[n++] = 0;
	memcpy(msg + n, body, dl); n += dl;
	CHECK(midici_initiator_on_message(0, msg, n, now + 5u));
}

static void test_midici_button_play_and_menu_entry(void)
{
	struct settings before;

	setup_ci();
	before = *ui_settings();
	tap(98, T0 + 10);
	CHECK_EQ(ui_menu_open(), 0);
	CHECK_EQ(midici_tx_pending(), 0);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK_EQ(io_n, 0);
	CHECK(led_is(98, 0, 0, 0));
	CHECK_MEM(&before, ui_settings(), sizeof before);

	setup_tap(T0 + 50);
	CHECK_EQ(ui_menu_open(), 1);
	/* Release-only on 98 does not switch page or trigger Discovery */
	ui_pad(98, 0, T0 + 80);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK_EQ(midici_tx_pending(), 0);

	/* Pressing 98 enters PAGE_MIDICI and starts Discovery */
	tap(98, T0 + 100);
	CHECK(led_is(98, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
	CHECK(led_is(96, 6, 6, 6));
	CHECK(led_is(97, 6, 6, 6));
	CHECK(led_is(91, 0, 0, 0));
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	CHECK(midici_tx_pending() > 0);
	ui_poll(T0 + 100 + SETTINGS_SAVE_QUIET_MS);
	CHECK_EQ(io_flash_writes, 0);
	CHECK_MEM(&before, ui_settings(), sizeof before);
}

static void test_midici_pad11_trigger_and_window_expiry(void)
{
	struct settings before;

	setup_ci();
	before = *ui_settings();
	setup_tap(T0);
	tap(98, T0 + 10);
	drain_ci_tx();
	CHECK(led_is(11, 0, 48, 63));
	CHECK(led_is(21, 0, 0, 0));
	CHECK(led_is(28, 0, 0, 0));

	/* 3000 ms window expires -> pad 11 dims to idle cyan */
	midici_initiator_tick(T0 + 10 + 3000);
	ui_poll(T0 + 10 + 3000);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK(led_is(11, 0, 6, 8));

	/* Other grid pads do nothing and never touch settings */
	tap(12, T0 + 3020);
	tap(54, T0 + 3021);
	tap(31, T0 + 3022);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK_EQ(midici_tx_pending(), 0);
	CHECK_MEM(&before, ui_settings(), sizeof before);

	/* Pad 11 re-triggers Discovery */
	tap(11, T0 + 3030);
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	CHECK(midici_tx_pending() > 0);
	CHECK(led_is(11, 0, 48, 63));
	ui_poll(T0 + 3030 + SETTINGS_SAVE_QUIET_MS);
	CHECK_EQ(io_flash_writes, 0);
}

static void test_midici_peer_leds_and_poll_refresh(void)
{
	uint32_t my_muid;
	uint8_t i;

	setup_ci();
	setup_tap(T0);
	tap(98, T0 + 10);
	my_muid = midici_muid();
	drain_ci_tx();

	/* Peer 0 (PE-capable) discovered -> pending amber on pad 21 */
	feed_peer_discovery(0x00110001u, my_muid, 0x08u, T0 + 20);
	ui_poll(T0 + 21);
	CHECK(led_is(21, 63, 24, 0));
	CHECK(led_is(22, 0, 0, 0));

	/* Peer 0 completes Endpoint + PE Caps + DeviceInfo -> green on pad 21 */
	feed_peer_endpoint(0x00110001u, my_muid, T0 + 22);
	feed_peer_pe_caps_and_device_info(0x00110001u, my_muid, T0 + 23);
	ui_poll(T0 + 30);
	CHECK(led_is(21, 0, 63, 0));

	/* Peer 1 (non-PE) completes Endpoint -> cyan on pad 22 */
	feed_peer_discovery(0x00110002u, my_muid, 0x00u, T0 + 31);
	feed_peer_endpoint(0x00110002u, my_muid, T0 + 32);
	ui_poll(T0 + 35);
	CHECK(led_is(22, 0, 48, 63));
	CHECK(led_is(23, 0, 0, 0));

	/* Fill peers 2..8 (9 total -> capped at 8 on pads 21..28, pad 99 untouched) */
	for (i = 2; i < 9; i++)
		feed_peer_discovery(0x00110001u + i, my_muid, 0x00u, (uint32_t)(T0 + 40 + i));
	ui_poll(T0 + 60);
	for (i = 0; i < 8; i++)
		CHECK(!led_is((uint8_t)(21 + i), 0, 0, 0));
	CHECK(led_is(99, 0, 0, 0));

	/* Reset clears peers -> pads 21..28 revert to off on ui_poll */
	midici_initiator_reset();
	ui_poll(T0 + 70);
	for (i = 0; i < 8; i++)
		CHECK(led_is((uint8_t)(21 + i), 0, 0, 0));

	/* Closing Setup restores play-page LEDs; background peer does not overwrite them */
	setup_tap(T0 + 100);
	CHECK_EQ(ui_menu_open(), 0);
	CHECK(led_is(11, 0, 20, 63));
	CHECK(led_is(98, 0, 0, 0));

	midici_initiator_start_discovery(T0 + 110);
	feed_peer_discovery(0x00110010u, my_muid, 0x00u, T0 + 111);
	ui_poll(T0 + 112);
	CHECK(led_is(21, 0, 4, 16)); /* still Play-page scale colour */

	/* Reopening Setup restores PAGE_MIDICI with the peer LED */
	setup_tap(T0 + 200);
	CHECK(led_is(98, 63, 63, 63));
	CHECK(led_is(21, 63, 24, 0));
}

static void test_midici_preserves_tuning_and_options_controls(void)
{
	setup_ci();
	setup_tap(T0);
	tap(98, T0 + 10);
	CHECK(led_is(98, 63, 63, 63));

	/* BTN_TUNING (97) still selects Tuning page and changes tuning */
	tap(97, T0 + 20);
	CHECK(led_is(97, 63, 63, 63));
	CHECK(led_is(98, 6, 6, 6));
	tap(82, T0 + 30);
	CHECK_EQ(ui_settings()->tuning, 1);

	/* BTN_OPTIONS (96) still keeps row 3 smoothing (31..34) and pad 54 (instant) */
	tap(96, T0 + 40);
	CHECK(led_is(96, 63, 63, 63));
	tap(34, T0 + 50);
	CHECK_EQ(ui_settings()->pressure_smoothing, SMOOTH_HEAVY);
	tap(54, T0 + 60);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_INSTANT);
}

void test_ui_all(void)
{
	RUN(test_play_page_colours_default);
	RUN(test_initial_draw_clears_unused_leds);
	RUN(test_press_plays_mapped_note);
	RUN(test_same_pitch_pads_light_together);
	RUN(test_peak_window_lights_when_sounding);
	RUN(test_octave_arrows_clamp);
	RUN(test_root_arrows_wrap);
	RUN(test_arrows_keep_held_note);
	RUN(test_setup_tap_opens_and_closes);
	RUN(test_setup_peek);
	RUN(test_menu_entry_releases_notes);
	RUN(test_menu_drawing);
	RUN(test_velocity_row_columns);
	RUN(test_scale_page_select);
	RUN(test_tuning_page);
	RUN(test_tuned_notes_carry_their_pitch);
	RUN(test_options_page_select);
	RUN(test_smoothing_row_columns);
	RUN(test_options_section_hues);
	RUN(test_menu_reopens_on_last_page);
	RUN(test_pad_held_across_menu_is_silent);
	RUN(test_settings_saved_after_quiet);
	RUN(test_settings_save_waits_for_playing_to_stop);
	RUN(test_settings_save_in_menu_ignores_earlier_playing);
	RUN(test_apply_settings_rejects_invalid);
	RUN(test_apply_settings_change);
	RUN(test_setup_led_reflects_protocol);
	RUN(test_transport_change_forgets_held_notes);
	RUN(test_two_stage_plays_scale_pitch);
	RUN(test_two_stage_no_note_while_menu_open);
	RUN(test_midici_button_play_and_menu_entry);
	RUN(test_midici_pad11_trigger_and_window_expiry);
	RUN(test_midici_peer_leds_and_poll_refresh);
	RUN(test_midici_preserves_tuning_and_options_controls);
}
