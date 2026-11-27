#include "instrument/ui.h"
#include "instrument/settings.h"
#include "instrument/io.h"
#include "instrument/palette.h"
#include "pads/pad_engine.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "app_defs.h"

#include <string.h>

#define SETUP_LED_MS 100

/* buttons */
#define BTN_OCT_UP   91
#define BTN_OCT_DOWN 92
#define BTN_ROOT_DN  93
#define BTN_ROOT_UP  94
#define BTN_SCALE    95
#define BTN_OPTIONS  96
#define BTN_TUNING   97
#define BTN_SURFACE  98

enum { PAGE_SCALE, PAGE_OPTIONS, PAGE_TUNING, PAGE_SURFACE };

/* Round buttons on the left, numbered from the bottom of the device. */
#define BTN_SPLIT_X   80
#define BTN_SPLIT_Y   70
#define BTN_SPLIT_OFF 60
#define BTN_PALETTE_FIRST 50
#define BTN_PALETTE_LAST  (BTN_PALETTE_FIRST - (PALETTE_COUNT - 1) * 10)

/* menu item colour families, each dim (index 0) and bright (index 1) */
enum { K_ROOT, K_SCALE, K_OCTAVE, K_LAYOUT, K_OPTION, K_VELOCITY, K_AFTERTOUCH, K_ROUTING,
       K_EQUAL, K_JUST, K_TEMPERAMENT, K_INDIAN, K_XENO, K_A4, K_KINDS,
       /* Surface page palette pads: lit in the palette's own root colour,
        * not from ITEM_RGB */
       K_PALETTE = K_KINDS };

static const uint8_t ITEM_RGB[K_KINDS][2][3] = {
	{{0, 8, 0}, {0, 63, 0}},       /* root keys: green */
	{{0, 0, 8}, {0, 0, 63}},       /* scales: blue */
	{{8, 0, 0}, {63, 0, 0}},       /* octave: red */
	{{8, 3, 0}, {63, 24, 0}},      /* layouts and absolute root: amber */
	{{6, 6, 6}, {63, 63, 63}},     /* page buttons, arrows: white */
	/* Options page sections: mostly white with a light hue */
	{{6, 5, 3}, {63, 48, 36}},     /* velocity: warm */
	{{3, 5, 6}, {36, 52, 63}},     /* aftertouch: cool */
	{{4, 6, 4}, {44, 63, 40}},     /* MIDI routing: soft green */
	/* Tuning page: one hue per tuning family (enum tuning_family order) */
	{{0, 6, 8}, {0, 48, 63}},      /* equal divisions: cyan */
	{{8, 7, 0}, {63, 56, 0}},      /* just: yellow */
	{{8, 3, 1}, {63, 20, 4}},      /* temperaments: orange */
	{{8, 0, 6}, {63, 0, 44}},      /* Indian: magenta */
	{{4, 0, 8}, {32, 0, 63}},      /* xenharmonic: violet */
	{{6, 6, 6}, {63, 63, 63}},     /* A4: white */
};

static const uint8_t RGB_OFF[3] = {0, 0, 0};
#define RGB_DIM_WHITE (ITEM_RGB[K_OPTION][0])
#define RGB_WHITE     (ITEM_RGB[K_OPTION][1])

/* Scale page: root naturals on row 6 (C D E F G A B), sharps on row 7
 * (0xFF: no key), layouts on 83..87. */
static const uint8_t NATURAL[7] = {0, 2, 4, 5, 7, 9, 11};
static const uint8_t SHARP[8] = {0xFF, 1, 3, 0xFF, 6, 8, 10, 0xFF};
static const uint8_t LAYOUT_AT[5] = {
	LAYOUT_CHROM_GUITAR, LAYOUT_INKEY_SEQ, LAYOUT_INKEY_3RDS,
	LAYOUT_INKEY_4THS, LAYOUT_CHROM_4THS,
};

/* Tuning page: the library on rows 8..5 (library order, row 8 first); A4
 * steps on row 2 (-1, -0.1, +0.1, +1 Hz on columns 1, 2, 7, 8) and the
 * common reference pitches on row 1. */
static const int8_t A4_STEP[8] = {-10, -1, 0, 0, 0, 0, 1, 10};
static const uint16_t A4_PRESET[4] = {4150, 4320, 4400, 4420};

/* Surface page (rows mirror the play surface: the upper half's settings on
 * top, the lower half's below):
 *   row 8     upper palette (one pad per palette)
 *   rows 7, 6 upper channel 1..8, 9..16
 *   rows 5, 4 previews of the upper/right and lower/left palettes on
 *             columns 1..7 (root, 2, 3rd, 4, 5th, 6, 7)
 *   rows 3, 2 lower channel 1..8, 9..16 (the Options page's channel)
 *   row 1     lower palette (the whole grid when split is off) */
static const uint8_t PREVIEW_ROLE[7] = {
	ROLE_ROOT, ROLE_SCALE, ROLE_THIRD, ROLE_SCALE, ROLE_FIFTH, ROLE_SCALE, ROLE_SCALE,
};

static struct settings g_s;
/* bit per pitch: sounding on the engine, per lane (1: the upper half of a
 * split surface on its own channel, 0: everything else) */
static uint8_t g_sounding[2][16];
static uint32_t g_setup_t;      /* ms counter at the Setup press that opened the menu */
static uint32_t g_led_t;        /* ms counter at the last Setup LED update */
static uint8_t g_menu;          /* menu open */
static uint8_t g_page;          /* PAGE_*: shown in the menu, kept while closed */
static uint8_t g_peek;          /* the Setup press held now opened the menu */
static uint32_t g_play_t;       /* ms counter at the last grid press or release
                                 * on the play page */

/* A menu pad: the settings field it sets, the value, its colour family and
 * whether a tap toggles the field instead. */
struct item {
	uint8_t *field;
	uint8_t value;
	uint8_t kind;
	uint8_t toggle;
};

static uint8_t is_grid(uint8_t i)
{
	uint8_t r = i / 10, c = i % 10;

	return r >= 1 && r <= 8 && c >= 1 && c <= 8;
}

static void ctx(struct scale_ctx *c)
{
	c->root = g_s.root;
	c->scale = g_s.scale;
	c->octave = g_s.octave;
	c->layout = g_s.layout;
	c->absolute_root = g_s.absolute_root;
	c->tuning = g_s.tuning;
}

/* The second zone is above the X seam or right of the Y seam. */
static uint8_t in_second_zone(uint8_t i)
{
	if (g_s.split_mode == SPLIT_X)
		return i / 10 >= 5;
	return g_s.split_mode == SPLIT_Y && i % 10 >= 5;
}

static uint8_t pad_channel(uint8_t i)
{
	return in_second_zone(i) ? g_s.upper_channel : g_s.channel;
}

static uint8_t pad_palette(uint8_t i)
{
	return in_second_zone(i) ? g_s.upper_palette : g_s.palette;
}

/* Lanes keep the halves' sounding pads apart when they play on different
 * channels; on one channel the halves share notes, so they share a lane. */
static uint8_t lane_of_channel(uint8_t ch)
{
	return g_s.split_mode != SPLIT_OFF && ch == g_s.upper_channel && ch != g_s.channel;
}

static uint8_t lane_of_pad(uint8_t i)
{
	return lane_of_channel(pad_channel(i));
}

static uint8_t is_sounding(uint8_t lane, int16_t pitch)
{
	return pitch >= 0 && pitch < 128 && (g_sounding[lane][pitch >> 3] & (1u << (pitch & 7)));
}

static void configure(void)
{
	pe_configure(g_s.velocity_mode, g_s.velocity_curve, g_s.pressure_mode,
	             g_s.pressure_threshold);
	pe_set_smoothing(g_s.pressure_smoothing);
}

/* 1 if grid pad i is an item on the current menu page */
static uint8_t menu_item(uint8_t i, struct item *it)
{
	uint8_t r = i / 10, c = i % 10 - 1; /* c: column 0..7 */

	it->toggle = 0;
	if (g_page == PAGE_SCALE) {
		if (r == 8 && c == 0) {
			it->field = &g_s.absolute_root;
			it->value = 1;
			it->kind = K_LAYOUT;
			it->toggle = 1;
		} else if (r == 8 && c >= 2 && c <= 6) {
			it->field = &g_s.layout;
			it->value = LAYOUT_AT[c - 2];
			it->kind = K_LAYOUT;
		} else if (r == 7 && SHARP[c] != 0xFF) {
			it->field = &g_s.root;
			it->value = SHARP[c];
			it->kind = K_ROOT;
		} else if (r == 6 && c < 7) {
			it->field = &g_s.root;
			it->value = NATURAL[c];
			it->kind = K_ROOT;
		} else if (r == 5) {
			it->field = &g_s.octave;
			it->value = c;
			it->kind = K_OCTAVE;
		} else if (r >= 1 && r <= 4) {
			/* the scales the tuning offers, in its order: rows 4..2,
			 * then row 1 */
			uint8_t sc = tuning_scale_at(tuning_get(g_s.tuning), (uint8_t)((4 - r) * 8 + c));

			if (sc == 0xFF)
				return 0;
			it->field = &g_s.scale;
			it->value = sc;
			it->kind = K_SCALE;
		} else {
			return 0;
		}
	} else if (g_page == PAGE_SURFACE) {
		if ((r == 8 || r == 1) && c < PALETTE_COUNT) {
			it->field = r == 8 ? &g_s.upper_palette : &g_s.palette;
			it->value = c;
			it->kind = K_PALETTE;
		} else if (r == 7 || r == 6) {
			it->field = &g_s.upper_channel;
			it->value = (uint8_t)((r == 6 ? 8 : 0) + c);
			it->kind = K_ROUTING;
		} else if (r == 3 || r == 2) {
			it->field = &g_s.channel;
			it->value = (uint8_t)((r == 2 ? 8 : 0) + c);
			it->kind = K_ROUTING;
		} else {
			return 0;
		}
	} else if (g_page == PAGE_TUNING) {
		uint8_t at = (uint8_t)((8 - r) * 8 + c);

		if (r < 5 || at >= TUNING_COUNT)
			return 0;
		it->field = &g_s.tuning;
		it->value = TUNINGS[at].id;
		it->kind = (uint8_t)(K_EQUAL + TUNINGS[at].family);
	} else {
		if (r == 8 && c < 4) {
			/* menu order: Peak-window, Zero-latency, Two-stage, Fixed */
			static const uint8_t VEL_AT[4] = {VEL_PEAK_WINDOW, VEL_ZERO_LATENCY,
			                                  VEL_TWO_STAGE, VEL_FIXED};

			it->field = &g_s.velocity_mode;
			it->value = VEL_AT[c];
			it->kind = K_VELOCITY;
		} else if (r >= 5 && r < 8 && c < (r == 5 ? THRESH_COUNT : 3)) {
			uint8_t *rows[3] = {&g_s.pressure_threshold, &g_s.pressure_mode,
			                    &g_s.velocity_curve};
			it->field = rows[r - 5];
			it->value = c;
			it->kind = r == 7 ? K_VELOCITY : K_AFTERTOUCH;
		} else if (r == 3 && c < 4) {
			it->field = &g_s.pressure_smoothing;
			it->value = c;
			it->kind = K_AFTERTOUCH;
		} else if (r == 4 && c < 2) {
			it->field = &g_s.din_mirror;
			it->value = c;
			it->kind = K_ROUTING;
		} else if (r == 2 || r == 1) {
			it->field = &g_s.channel;
			it->value = (uint8_t)((r == 1 ? 8 : 0) + c);
			it->kind = K_ROUTING;
		} else {
			return 0;
		}
	}
	return 1;
}

static uint8_t page_of(uint8_t button)
{
	return button == BTN_OPTIONS ? PAGE_OPTIONS
	     : button == BTN_TUNING  ? PAGE_TUNING
	     : button == BTN_SURFACE ? PAGE_SURFACE
	     : PAGE_SCALE;
}

static uint8_t is_page_button(uint8_t i)
{
	return i == BTN_SCALE || i == BTN_OPTIONS || i == BTN_TUNING || i == BTN_SURFACE;
}

/* A palette pad: its root colour, at an eighth of the brightness when it is
 * not the one chosen (the menu's dim/bright convention). */
static const uint8_t *palette_item(uint8_t palette, uint8_t chosen)
{
	static uint8_t dim[3];
	const uint8_t *root = palette_rgb(palette, ROLE_ROOT);
	uint8_t k;

	if (chosen)
		return root;
	for (k = 0; k < 3; k++)
		dim[k] = (uint8_t)((root[k] + 7) >> 3);
	return dim;
}

/* Shared by the play and Surface pages. Palette buttons edit the primary zone. */
static uint8_t surface_button(uint8_t i, struct item *it)
{
	if (g_menu && g_page != PAGE_SURFACE)
		return 0;
	it->toggle = 0;
	if (i == BTN_SPLIT_X || i == BTN_SPLIT_Y || i == BTN_SPLIT_OFF) {
		it->field = &g_s.split_mode;
		it->value = i == BTN_SPLIT_X ? SPLIT_X : i == BTN_SPLIT_Y ? SPLIT_Y : SPLIT_OFF;
		it->kind = K_LAYOUT;
	} else if (i >= BTN_PALETTE_LAST && i <= BTN_PALETTE_FIRST && i % 10 == 0) {
		it->field = &g_s.palette;
		it->value = (uint8_t)((BTN_PALETTE_FIRST - i) / 10);
		it->kind = K_PALETTE;
	} else {
		return 0;
	}
	return 1;
}

#define A4_NONE (-1)
#define A4_STEP_PAD (-2)

/* On the Tuning page: a reference-pitch preset (0..3), A4_STEP_PAD or
 * A4_NONE. */
static int8_t a4_pad(uint8_t i)
{
	uint8_t r = i / 10, c = i % 10 - 1;

	if (g_page != PAGE_TUNING)
		return A4_NONE;
	if (r == 2 && A4_STEP[c])
		return A4_STEP_PAD;
	if (r == 1 && c >= 2 && c < 6)
		return (int8_t)(c - 2);
	return A4_NONE;
}

static const uint8_t *colour(uint8_t i)
{
	struct item it;

	if (surface_button(i, &it)) {
		if (it.kind == K_PALETTE)
			return palette_item(it.value, *it.field == it.value);
		return ITEM_RGB[it.kind][*it.field == it.value];
	}
	if (g_menu) {
		if (is_grid(i)) {
			int8_t a4 = a4_pad(i);

			if (a4 >= 0)
				return ITEM_RGB[K_A4][A4_PRESET[a4] == g_s.a4_tenths];
			if (a4 == A4_STEP_PAD)
				return ITEM_RGB[K_A4][0];
			if (!menu_item(i, &it)) {
				if (g_page == PAGE_SURFACE && (i / 10 == 4 || i / 10 == 5) && i % 10 <= 7)
					return palette_rgb(i / 10 == 5 ? g_s.upper_palette : g_s.palette,
					                   (enum role)PREVIEW_ROLE[i % 10 - 1]);
				return RGB_OFF;
			}
			if (it.kind == K_PALETTE)
				return palette_item(it.value, *it.field == it.value);
			return ITEM_RGB[it.kind][it.toggle ? *it.field != 0 : *it.field == it.value];
		}
		if (is_page_button(i))
			return page_of(i) == g_page ? RGB_WHITE : RGB_DIM_WHITE;
		return RGB_OFF;
	}
	if (is_grid(i)) {
		struct scale_ctx c;
		int16_t pitch;

		ctx(&c);
		pitch = scale_pad_pitch(&c, i);
		return is_sounding(lane_of_pad(i), pitch) ? RGB_WHITE
		                                          : palette_rgb(pad_palette(i), scale_pitch_role(&c, pitch));
	}
	if (i >= BTN_OCT_UP && i <= BTN_ROOT_UP)
		return RGB_DIM_WHITE;
	return RGB_OFF;
}

static void draw(uint8_t i)
{
	const uint8_t *c = colour(i);

	io_led(TYPEPAD, i, c[0], c[1], c[2]);
}

/* every pad and button LED, clearing those the page doesn't use */
static void draw_all(void)
{
	uint8_t i;

	for (i = 1; i <= 98; i++)
		if (i != 9 && i != 90)
			draw(i);
}

static void draw_setup_led(void)
{
	if (io_port_is_midi2(USBSTANDALONE))
		io_led(TYPESETUP, 0, 0, 63, 0);
	else
		io_led(TYPESETUP, 0, 63, 20, 0);
}

void ui_on_sounding(uint8_t pitch, uint8_t ch, uint8_t on)
{
	struct scale_ctx c;
	uint8_t r, col, lane = lane_of_channel(ch);

	if (pitch > 127)
		return;
	if (on)
		g_sounding[lane][pitch >> 3] |= (uint8_t)(1u << (pitch & 7));
	else
		g_sounding[lane][pitch >> 3] &= (uint8_t)~(1u << (pitch & 7));
	if (g_menu)
		return;
	ctx(&c);
	for (r = 10; r <= 80; r += 10)
		for (col = 1; col <= 8; col++)
			if (lane_of_pad((uint8_t)(r + col)) == lane &&
			    scale_pad_pitch(&c, (uint8_t)(r + col)) == pitch)
				draw((uint8_t)(r + col));
}

/* the pad engine's Two-stage target: what the pad plays when it starts */
static int16_t pad_target(uint8_t pad, uint8_t *ch, uint8_t *din)
{
	struct scale_ctx c;

	if (g_menu)
		return -1;
	ctx(&c);
	*ch = pad_channel(pad);
	*din = g_s.din_mirror;
	return scale_pad_pitch(&c, pad);
}

/* the pad engine's intonation hook: a pitch attribute unless the world is
 * 12-EDO at A4 = 440, where the note number says it all */
static uint8_t note_pitch(uint8_t note, uint16_t *pitch79)
{
	if (tuning_is_standard(g_s.tuning, g_s.a4_tenths))
		return 0;
	*pitch79 = tuning_pitch79(g_s.tuning, g_s.root, g_s.a4_tenths, note);
	return 1;
}

void ui_init(uint32_t now)
{
	pe_set_target(pad_target);
	pe_set_pitch(note_pitch);
	settings_load(&g_s);
	configure();
	memset(g_sounding, 0, sizeof g_sounding);
	g_menu = 0;
	g_page = PAGE_SCALE;
	g_peek = 0;
	g_play_t = now - SETTINGS_SAVE_QUIET_MS;
	draw_all();
	draw_setup_led();
	g_led_t = now;
}

static void play_button(uint8_t i, uint32_t now)
{
	struct settings old = g_s;

	if (i == BTN_OCT_UP && g_s.octave < 7)
		g_s.octave++;
	else if (i == BTN_OCT_DOWN && g_s.octave > 0)
		g_s.octave--;
	else if (i == BTN_ROOT_DN)
		g_s.root = g_s.root ? (uint8_t)(g_s.root - 1) : 11;
	else if (i == BTN_ROOT_UP)
		g_s.root = g_s.root < 11 ? (uint8_t)(g_s.root + 1) : 0;
	if (memcmp(&old, &g_s, sizeof g_s) == 0)
		return;
	/* held notes keep their latched pitch: no release */
	draw_all();
	settings_touch(now);
}

static void menu_pad(uint8_t i, uint32_t now)
{
	struct item it;
	uint8_t v;

	if (is_page_button(i)) {
		g_page = page_of(i);
		draw_all();
		return;
	}
	if (!is_grid(i))
		return;
	if (a4_pad(i) != A4_NONE) {
		int8_t p = a4_pad(i);
		int32_t a4 = p >= 0 ? A4_PRESET[p] : g_s.a4_tenths + A4_STEP[i % 10 - 1];

		a4 = a4 < A4_TENTHS_MIN ? A4_TENTHS_MIN : a4 > A4_TENTHS_MAX ? A4_TENTHS_MAX : a4;
		if (a4 == g_s.a4_tenths)
			return;
		g_s.a4_tenths = (uint16_t)a4;
		draw_all();
		settings_touch(now);
		return;
	}
	if (!menu_item(i, &it))
		return;
	v = it.toggle ? !*it.field : it.value;
	if (v == *it.field)
		return;
	*it.field = v;
	if (g_page == PAGE_OPTIONS)
		configure();
	if (g_page == PAGE_TUNING)
		g_s.scale = settings_scale_for(g_s.tuning, g_s.scale);
	draw_all();
	settings_touch(now);
}

void ui_pad(uint8_t index, uint8_t value, uint32_t now)
{
	struct item it;

	if (value && surface_button(index, &it)) {
		if (*it.field != it.value) {
			if (it.field == &g_s.split_mode)
				pe_release_all();
			*it.field = it.value;
			draw_all();
			settings_touch(now);
		}
		return;
	}
	if (g_menu) {
		if (value)
			menu_pad(index, now);
		return;
	}
	if (is_grid(index)) {
		g_play_t = now;
		if (value) {
			struct scale_ctx c;

			ctx(&c);
			pe_press(index, scale_pad_pitch(&c, index), pad_channel(index), g_s.din_mirror, now);
		} else {
			pe_release(index, now);
		}
	} else if (value) {
		play_button(index, now);
	}
}

void ui_setup(uint8_t value, uint32_t now)
{
	if (value) {
		if (!g_menu) {
			g_menu = 1; /* first, so the release's sounding edges don't draw */
			pe_release_all();
			g_peek = 1;
			g_setup_t = now;
		} else {
			g_menu = 0;
			g_peek = 0;
		}
		draw_all();
	} else {
		if (g_peek && (uint32_t)(now - g_setup_t) >= SETUP_PEEK_MS) {
			g_menu = 0;
			draw_all();
		}
		g_peek = 0;
	}
}

void ui_poll(uint32_t now)
{
	pe_poll(now);
	if ((uint32_t)(now - g_led_t) >= SETUP_LED_MS) {
		g_led_t = now;
		draw_setup_led();
	}
	/* A save erases a flash page and stalls the device for tens of ms, so
	 * it waits until the player has been still for the quiet time too, not
	 * just for a moment between notes. No note sounds in the menu. */
	settings_poll(&g_s, now,
	              pe_notes_active() ||
	              (!g_menu && (uint32_t)(now - g_play_t) < SETTINGS_SAVE_QUIET_MS));
}

const struct settings *ui_settings(void)
{
	return &g_s;
}

void ui_apply_settings(const struct settings *s, uint32_t now)
{
	if (!settings_valid(s) || memcmp(s, &g_s, sizeof g_s) == 0)
		return;
	pe_release_all();
	g_s = *s;
	configure();
	draw_all();
	settings_touch(now);
}

void ui_transport_changed(uint32_t now)
{
	pe_forget_all();
	memset(g_sounding, 0, sizeof g_sounding);
	if (!g_menu)
		draw_all();
	draw_setup_led();
	g_led_t = now;
}

uint8_t ui_menu_open(void)
{
	return g_menu;
}
