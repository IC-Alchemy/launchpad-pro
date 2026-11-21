#include "instrument/settings.h"
#include "instrument/io.h"
#include "scales/scales.h"
#include <string.h>

#define FIELDS 11

static struct settings g_saved;
static uint32_t g_touch;

void settings_defaults(struct settings *s)
{
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
}

uint8_t settings_valid(const struct settings *s)
{
	return s->root < 12 && s->scale < SCALE_COUNT && s->octave < 8 &&
	       s->layout < LAYOUT_COUNT && s->absolute_root <= 1 &&
	       s->din_mirror <= 1 && s->velocity_mode < 3 &&
	       s->velocity_curve < 3 && s->pressure_mode < 3 &&
	       s->pressure_threshold < 3 && s->channel < 16;
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
}

void settings_encode(const struct settings *s, uint8_t out[SETTINGS_RECORD_LEN])
{
	uint8_t sum = 0;
	uint8_t i;

	out[0] = 'L';
	out[1] = 'P';
	out[2] = 'M';
	out[3] = '2';
	out[4] = 1;
	to_bytes(s, out + 5);
	for (i = 0; i < SETTINGS_RECORD_LEN - 1; i++)
		sum = (uint8_t)(sum + out[i]);
	out[SETTINGS_RECORD_LEN - 1] = (uint8_t)(0u - sum);
}

uint8_t settings_decode(const uint8_t in[SETTINGS_RECORD_LEN], struct settings *s)
{
	struct settings t;
	uint8_t sum = 0;
	uint8_t i;

	if (in[0] != 'L' || in[1] != 'P' || in[2] != 'M' || in[3] != '2' || in[4] != 1)
		return 0;
	for (i = 0; i < SETTINGS_RECORD_LEN; i++)
		sum = (uint8_t)(sum + in[i]);
	if (sum != 0)
		return 0;
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
	io_flash_write(0, rec, SETTINGS_RECORD_LEN);
	g_saved = *cur;
}
