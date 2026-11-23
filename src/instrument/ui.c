#include "instrument/ui.h"
#include "instrument/settings.h"
#include "instrument/io.h"
#include "pads/pad_engine.h"
#include "scales/scales.h"
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

enum { PAGE_SCALE, PAGE_OPTIONS };

/* menu item colour families, each dim (index 0) and bright (index 1) */
enum { K_ROOT, K_SCALE, K_OCTAVE, K_LAYOUT, K_OPTION };

static const uint8_t ITEM_RGB[5][2][3] = {
	{{0, 8, 0}, {0, 63, 0}},       /* root keys: green */
	{{0, 0, 8}, {0, 0, 63}},       /* scales: blue */
	{{8, 0, 0}, {63, 0, 0}},       /* octave: red */
	{{8, 3, 0}, {63, 24, 0}},      /* layouts and absolute root: amber */
	{{6, 6, 6}, {63, 63, 63}},     /* option rows, page buttons, arrows: white */
};

static const uint8_t ROLE_RGB[5][3] = {
	{0, 0, 0},     /* ROLE_OFF */
	{4, 4, 4},     /* ROLE_OUT */
	{0, 4, 16},    /* ROLE_SCALE */
	{8, 24, 40},   /* ROLE_HIGHLIGHT */
	{0, 20, 63},   /* ROLE_ROOT */
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

static struct settings g_s;
static uint8_t g_sounding[16];  /* bit per pitch: sounding on the engine */
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
}

static uint8_t is_sounding(int16_t pitch)
{
	return pitch >= 0 && pitch < 128 && (g_sounding[pitch >> 3] & (1u << (pitch & 7)));
}

static void configure(void)
{
	pe_configure(g_s.velocity_mode, g_s.velocity_curve, g_s.pressure_mode,
	             g_s.pressure_threshold);
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
		} else if (r >= 2 && r <= 4) {
			it->field = &g_s.scale;
			it->value = (uint8_t)((4 - r) * 8 + c);
			it->kind = K_SCALE;
		} else if (r == 1 && c == 0) {
			it->field = &g_s.scale;
			it->value = 24;
			it->kind = K_SCALE;
		} else {
			return 0;
		}
	} else {
		it->kind = K_OPTION;
		if (r == 8 && c < 4) {
			/* menu order: Peak-window, Zero-latency, Two-stage, Fixed */
			static const uint8_t VEL_AT[4] = {VEL_PEAK_WINDOW, VEL_ZERO_LATENCY,
			                                  VEL_TWO_STAGE, VEL_FIXED};

			it->field = &g_s.velocity_mode;
			it->value = VEL_AT[c];
		} else if (r >= 5 && r < 8 && c < 3) {
			uint8_t *rows[3] = {&g_s.pressure_threshold, &g_s.pressure_mode,
			                    &g_s.velocity_curve};
			it->field = rows[r - 5];
			it->value = c;
		} else if (r == 4 && c < 2) {
			it->field = &g_s.din_mirror;
			it->value = c;
		} else if (r == 2 || r == 1) {
			it->field = &g_s.channel;
			it->value = (uint8_t)((r == 1 ? 8 : 0) + c);
		} else {
			return 0;
		}
	}
	return 1;
}

static const uint8_t *colour(uint8_t i)
{
	if (g_menu) {
		struct item it;

		if (is_grid(i)) {
			if (!menu_item(i, &it))
				return RGB_OFF;
			return ITEM_RGB[it.kind][it.toggle ? *it.field != 0 : *it.field == it.value];
		}
		if (i == BTN_SCALE || i == BTN_OPTIONS)
			return (i == BTN_OPTIONS) == (g_page == PAGE_OPTIONS) ? RGB_WHITE : RGB_DIM_WHITE;
		return RGB_OFF;
	}
	if (is_grid(i)) {
		struct scale_ctx c;
		int16_t pitch;

		ctx(&c);
		pitch = scale_pad_pitch(&c, i);
		return is_sounding(pitch) ? RGB_WHITE : ROLE_RGB[scale_pitch_role(&c, pitch)];
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

void ui_on_sounding(uint8_t pitch, uint8_t on)
{
	struct scale_ctx c;
	uint8_t r, col;

	if (pitch > 127)
		return;
	if (on)
		g_sounding[pitch >> 3] |= (uint8_t)(1u << (pitch & 7));
	else
		g_sounding[pitch >> 3] &= (uint8_t)~(1u << (pitch & 7));
	if (g_menu)
		return;
	ctx(&c);
	for (r = 10; r <= 80; r += 10)
		for (col = 1; col <= 8; col++)
			if (scale_pad_pitch(&c, (uint8_t)(r + col)) == pitch)
				draw((uint8_t)(r + col));
}

/* the pad engine's Two-stage target: what the pad plays when it starts */
static int16_t pad_target(uint8_t pad, uint8_t *ch, uint8_t *din)
{
	struct scale_ctx c;

	if (g_menu)
		return -1;
	ctx(&c);
	*ch = g_s.channel;
	*din = g_s.din_mirror;
	return scale_pad_pitch(&c, pad);
}

void ui_init(uint32_t now)
{
	pe_set_target(pad_target);
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

	if (i == BTN_SCALE || i == BTN_OPTIONS) {
		g_page = i == BTN_OPTIONS ? PAGE_OPTIONS : PAGE_SCALE;
		draw_all();
		return;
	}
	if (!is_grid(i) || !menu_item(i, &it))
		return;
	v = it.toggle ? !*it.field : it.value;
	if (v == *it.field)
		return;
	*it.field = v;
	if (g_page == PAGE_OPTIONS)
		configure();
	draw_all();
	settings_touch(now);
}

void ui_pad(uint8_t index, uint8_t value, uint32_t now)
{
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
			pe_press(index, scale_pad_pitch(&c, index), g_s.channel, g_s.din_mirror, now);
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
