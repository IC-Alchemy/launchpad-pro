#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

enum { VEL_PEAK_WINDOW, VEL_ZERO_LATENCY, VEL_FIXED, VEL_TWO_STAGE };
enum { CURVE_SOFT, CURVE_LINEAR, CURVE_HARD };
enum { PRESS_POLY, PRESS_CHANNEL, PRESS_OFF };
/* THRESH_INSTANT: aftertouch from first contact (the level where velocity
 * sensing starts), with no dead zone before it */
enum { THRESH_LOW, THRESH_MED, THRESH_HIGH, THRESH_INSTANT };
#define THRESH_COUNT 4
/* aftertouch low-pass: one pole per held pad at 1 kHz, coefficient 1/2^k with
 * k = 1, 3, 5, 6 (settle to ~2% in about 6, 30, 125, 250 ms) */
enum { SMOOTH_NONE, SMOOTH_LIGHT, SMOOTH_MEDIUM, SMOOTH_HEAVY };

struct settings {
	uint8_t root, scale, octave, layout, absolute_root,
	        velocity_mode, velocity_curve, pressure_mode,
	        pressure_threshold, din_mirror, channel, pressure_smoothing;
	uint8_t tuning;      /* tuning id (scales/tuning.h); offers scale */
	uint16_t a4_tenths;  /* reference pitch in 0.1 Hz, A4_TENTHS_MIN..MAX */
};

/* Record: 'LPM2', version, 12 one-byte fields, tuning, A4 (low byte first),
 * checksum. Version 2 (18 bytes) had no tuning or A4, version 1 (17 bytes)
 * no smoothing either; settings_load still accepts both. */
#define SETTINGS_RECORD_LEN 21
#define SETTINGS_RECORD_V2_LEN 18
#define SETTINGS_RECORD_V1_LEN 17
#define SETTINGS_SAVE_QUIET_MS 2000

void settings_defaults(struct settings *s);
/* The scale a change to this tuning keeps: the current one if the tuning
 * offers it, else the tuning's default. */
uint8_t settings_scale_for(uint8_t tuning, uint8_t scale);
uint8_t settings_valid(const struct settings *s);
void settings_encode(const struct settings *s, uint8_t out[SETTINGS_RECORD_LEN]);
/* 1 ok; 0 leaves *s untouched. Older records decode with the defaults for
 * the fields they lack (12-EDO at 440 Hz, light smoothing); the bytes past
 * their length are ignored. */
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
