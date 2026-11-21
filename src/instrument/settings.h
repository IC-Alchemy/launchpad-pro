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
/* Caller contract: every change to a settings field must be followed by
 * settings_touch(now). settings_poll debounces from the last touch, which
 * starts at 0 after settings_load, so an untouched change could be written
 * as soon as now >= SETTINGS_SAVE_QUIET_MS.
 * Known limit: io_flash_write returns no status, so a write is assumed to
 * succeed and the saved copy is updated; there is no retry. */
/* a change happened now */
void settings_touch(uint32_t now);
void settings_poll(const struct settings *cur, uint32_t now, uint8_t notes_active);

#endif
