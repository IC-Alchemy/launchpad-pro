#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

enum { VEL_PEAK_WINDOW, VEL_ZERO_LATENCY, VEL_FIXED };
enum { CURVE_SOFT, CURVE_LINEAR, CURVE_HARD };
enum { PRESS_POLY, PRESS_CHANNEL, PRESS_OFF };
enum { THRESH_LOW, THRESH_MED, THRESH_HIGH };

struct settings {
	uint8_t root, scale, octave, layout, absolute_root,
	        velocity_mode, velocity_curve, pressure_mode,
	        pressure_threshold, din_mirror, channel;
};

#define SETTINGS_RECORD_LEN 17
#define SETTINGS_SAVE_QUIET_MS 2000

void settings_defaults(struct settings *s);
uint8_t settings_valid(const struct settings *s);
void settings_encode(const struct settings *s, uint8_t out[SETTINGS_RECORD_LEN]);
/* 1 ok; 0 leaves *s untouched */
uint8_t settings_decode(const uint8_t in[SETTINGS_RECORD_LEN], struct settings *s);
/* flash offset 0; defaults on failure; remembers the result as "saved" */
void settings_load(struct settings *s);
/* a change happened now */
void settings_touch(uint32_t now);
void settings_poll(const struct settings *cur, uint32_t now, uint8_t notes_active);

#endif
