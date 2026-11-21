#include "test.h"
#include "instrument/ui.h"
#include "instrument/settings.h"
#include "pads/pad_engine.h"
#include "scales/scales.h"
#include "stubs/io_stub.h"
#include "app_defs.h"

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
	CHECK(led_is(12, 0, 0, 0));
	CHECK(led_is(86, 63, 24, 0));  /* In-key 4ths */
	CHECK(led_is(83, 8, 3, 0));
	CHECK(led_is(81, 8, 3, 0));    /* absolute root off */
	CHECK(led_is(82, 0, 0, 0));
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
	tap(12, T0 + 180);             /* unused pad */
	CHECK_EQ(ui_settings()->scale, 24);
	CHECK_EQ(io_n, 0);             /* pads don't reach the engine */
	CHECK_EQ(ui_menu_open(), 1);
}

static void test_options_page_select(void)
{
	setup();
	setup_tap(T0);
	tap(96, T0 + 100);
	CHECK(led_is(96, 63, 63, 63));
	CHECK(led_is(95, 6, 6, 6));
	CHECK(led_is(81, 63, 63, 63));  /* Peak-window */
	CHECK(led_is(21, 63, 63, 63));  /* channel 1 */
	CHECK(led_is(31, 0, 0, 0));
	tap(82, T0 + 110);
	CHECK_EQ(ui_settings()->velocity_mode, VEL_ZERO_LATENCY);
	CHECK(led_is(82, 63, 63, 63));
	CHECK(led_is(81, 6, 6, 6));
	/* 11..18 = channels 9..16: pad 12 is channel 10 (stored 9) */
	tap(12, T0 + 120);
	CHECK_EQ(ui_settings()->channel, 9);
	CHECK(led_is(12, 63, 63, 63));
	CHECK(led_is(21, 6, 6, 6));
	tap(13, T0 + 125);
	CHECK_EQ(ui_settings()->channel, 10);
	tap(18, T0 + 126);
	CHECK_EQ(ui_settings()->channel, 15);
	tap(73, T0 + 130);
	CHECK_EQ(ui_settings()->velocity_curve, CURVE_HARD);
	tap(62, T0 + 140);
	CHECK_EQ(ui_settings()->pressure_mode, PRESS_CHANNEL);
	tap(51, T0 + 150);
	CHECK_EQ(ui_settings()->pressure_threshold, THRESH_LOW);
	tap(42, T0 + 160);
	CHECK_EQ(ui_settings()->din_mirror, 1);
	CHECK(led_is(42, 63, 63, 63));
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
	s.scale = 30;
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
	RUN(test_scale_page_select);
	RUN(test_options_page_select);
	RUN(test_menu_reopens_on_last_page);
	RUN(test_pad_held_across_menu_is_silent);
	RUN(test_settings_saved_after_quiet);
	RUN(test_settings_save_waits_for_playing_to_stop);
	RUN(test_settings_save_in_menu_ignores_earlier_playing);
	RUN(test_apply_settings_rejects_invalid);
	RUN(test_apply_settings_change);
	RUN(test_setup_led_reflects_protocol);
}
