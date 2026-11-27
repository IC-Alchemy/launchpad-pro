#include "instrument/settings.h"
#include "instrument/io.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "instrument/palette.h"
#include <string.h>

#define FIELDS 19

static struct settings g_saved;
static uint32_t g_touch;

void settings_defaults(struct settings *s)
{
	memset(s, 0, sizeof *s); /* padding too: settings are compared with memcmp */
	s->root = 0;
	s->scale = 0;
	s->octave = 4;
	s->layout = LAYOUT_INKEY_4THS;
	s->absolute_root = 0;
	s->velocity_mode = VEL_PEAK_WINDOW;
	s->velocity_curve = CURVE_LINEAR;
	s->pressure_mode = PRESS_POLY;
	s->pressure_threshold = THRESH_MED;
	s->din_mirror = 0;
	s->channel = 0;
	s->pressure_smoothing = SMOOTH_LIGHT;
	s->tuning = TUNING_DEFAULT_ID;
	s->a4_tenths = A4_TENTHS_DEFAULT;
	s->palette = PALETTE_OCEAN;
	s->split_mode = SPLIT_OFF;
	s->upper_palette = PALETTE_CONTRAST;
	s->upper_channel = 1;
}

uint8_t settings_scale_for(uint8_t tuning, uint8_t scale)
{
	const struct tuning_def *t = tuning_get(tuning);

	return tuning_offers(t, scale) ? scale : t->default_scale;
}

uint8_t settings_valid(const struct settings *s)
{
	const struct tuning_def *t = tuning_find(s->tuning);

	return t && tuning_offers(t, s->scale) &&
	       s->a4_tenths >= A4_TENTHS_MIN && s->a4_tenths <= A4_TENTHS_MAX &&
	       s->root < 12 && s->scale < SCALE_COUNT && s->octave < 8 &&
	       s->layout < LAYOUT_COUNT && s->absolute_root <= 1 &&
	       s->din_mirror <= 1 && s->velocity_mode < 4 &&
	       s->velocity_curve < 3 && s->pressure_mode < 3 &&
	       s->pressure_threshold < THRESH_COUNT && s->channel < 16 &&
	       s->pressure_smoothing < 4 && s->palette < PALETTE_COUNT &&
	       s->split_mode < SPLIT_COUNT && s->upper_palette < PALETTE_COUNT && s->upper_channel < 16;
}

static void to_bytes(const struct settings *s, uint8_t *b)
{
	b[0] = s->root;
	b[1] = s->scale;
	b[2] = s->octave;
	b[3] = s->layout;
	b[4] = s->absolute_root;
	b[5] = s->velocity_mode;
	b[6] = s->velocity_curve;
	b[7] = s->pressure_mode;
	b[8] = s->pressure_threshold;
	b[9] = s->din_mirror;
	b[10] = s->channel;
	b[11] = s->pressure_smoothing;
	b[12] = s->tuning;
	b[13] = (uint8_t)(s->a4_tenths & 0xFF);
	b[14] = (uint8_t)(s->a4_tenths >> 8);
	b[15] = s->palette;
	b[16] = s->split_mode;
	b[17] = s->upper_palette;
	b[18] = s->upper_channel;
}

void settings_encode(const struct settings *s, uint8_t out[SETTINGS_RECORD_LEN])
{
	uint8_t sum = 0;
	uint8_t i;

	out[0] = 'L';
	out[1] = 'P';
	out[2] = 'M';
	out[3] = '2';
	out[4] = 5;
	to_bytes(s, out + 5);
	for (i = 0; i < SETTINGS_RECORD_LEN - 1; i++)
		sum = (uint8_t)(sum + out[i]);
	out[SETTINGS_RECORD_LEN - 1] = (uint8_t)(0u - sum);
}

uint8_t settings_decode(const uint8_t in[SETTINGS_RECORD_LEN], struct settings *s)
{
	struct settings t;
	uint8_t sum = 0;
	uint8_t i, len;

	if (in[0] != 'L' || in[1] != 'P' || in[2] != 'M' || in[3] != '2')
		return 0;
	if (in[4] == 5 || in[4] == 4)
		len = SETTINGS_RECORD_LEN;
	else if (in[4] == 3)
		len = SETTINGS_RECORD_V3_LEN;
	else if (in[4] == 2)
		len = SETTINGS_RECORD_V2_LEN;
	else if (in[4] == 1)
		len = SETTINGS_RECORD_V1_LEN;
	else
		return 0;
	for (i = 0; i < len; i++)
		sum = (uint8_t)(sum + in[i]);
	if (sum != 0)
		return 0;
	settings_defaults(&t);
	t.root = in[5];
	t.scale = in[6];
	t.octave = in[7];
	t.layout = in[8];
	t.absolute_root = in[9];
	t.velocity_mode = in[10];
	t.velocity_curve = in[11];
	t.pressure_mode = in[12];
	t.pressure_threshold = in[13];
	t.din_mirror = in[14];
	t.channel = in[15];
	t.pressure_smoothing = len >= SETTINGS_RECORD_V2_LEN ? in[16] : SMOOTH_LIGHT;
	if (len >= SETTINGS_RECORD_V3_LEN) {
		t.tuning = in[17];
		t.a4_tenths = (uint16_t)(in[18] | (in[19] << 8));
	}
	if (in[4] == 4) {
		static const uint8_t v4_palette[] = {
			PALETTE_OCEAN, PALETTE_COUNT, PALETTE_COUNT, PALETTE_COUNT,
			PALETTE_CITRUS, PALETTE_GLACIER, PALETTE_COUNT, PALETTE_CONTRAST,
		};

		if (in[20] >= sizeof v4_palette || in[21] > 1 || in[22] >= sizeof v4_palette)
			return 0;
		if (v4_palette[in[20]] != PALETTE_COUNT)
			t.palette = v4_palette[in[20]];
		if (v4_palette[in[22]] != PALETTE_COUNT)
			t.upper_palette = v4_palette[in[22]];
		t.split_mode = in[21] ? SPLIT_X : SPLIT_OFF;
		t.upper_channel = in[23];
	} else if (in[4] == 5) {
		t.palette = in[20];
		t.split_mode = in[21];
		t.upper_palette = in[22];
		t.upper_channel = in[23];
	}
	if (!settings_valid(&t))
		return 0;
	*s = t;
	return 1;
}

void settings_load(struct settings *s)
{
	uint8_t rec[SETTINGS_RECORD_LEN];

	io_flash_read(0, rec, SETTINGS_RECORD_LEN);
	if (!settings_decode(rec, s))
		settings_defaults(s);
	g_saved = *s;
	g_touch = 0;
}

void settings_touch(uint32_t now)
{
	g_touch = now;
}

void settings_poll(const struct settings *cur, uint32_t now, uint8_t notes_active)
{
	uint8_t rec[SETTINGS_RECORD_LEN];
	uint8_t a[FIELDS], b[FIELDS];

	if (notes_active || (uint32_t)(now - g_touch) < SETTINGS_SAVE_QUIET_MS)
		return;
	to_bytes(cur, a);
	to_bytes(&g_saved, b);
	if (memcmp(a, b, FIELDS) == 0)
		return;
	settings_encode(cur, rec);
	/* io_flash_write has no status: assume success, no retry. */
	io_flash_write(0, rec, SETTINGS_RECORD_LEN);
	g_saved = *cur;
}
