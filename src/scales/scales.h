#ifndef SCALES_H
#define SCALES_H

#include <stdint.h>

/* Twelve-note scales first, then All Degrees, then tuning-specific scales.
 * Indexes are used by saved settings and X-ScaleSettings. */
#define SCALE_COUNT 56
#define SCALE_HARMONIC_MINOR 15
#define SCALE_PHRYGIAN_DOMINANT 19
#define SCALE_CHROMATIC 25
#define SCALE_TWELVE_COUNT (SCALE_CHROMATIC + 1)
#define SCALE_ALL_DEGREES SCALE_TWELVE_COUNT
#define SCALE_MAQAM_RAST (SCALE_ALL_DEGREES + 1)

enum layout {
	LAYOUT_INKEY_4THS,
	LAYOUT_INKEY_3RDS,
	LAYOUT_INKEY_SEQ,
	LAYOUT_CHROM_4THS,
	LAYOUT_CHROM_GUITAR,
	LAYOUT_COUNT
};

/* Colour role of a pad: off (no note), out of the scale, a scale note, the
 * scale's third and fifth steps, the root. */
enum role { ROLE_OFF, ROLE_OUT, ROLE_SCALE, ROLE_THIRD, ROLE_FIFTH, ROLE_ROOT, ROLE_COUNT };

struct scale_def {
	const char *name;
	uint8_t n;          /* steps; 0: All Degrees (every degree of the tuning) */
	uint8_t degrees;    /* 0: steps are semitone slots; else steps are degrees
	                     * of a tuning with this many per period */
	uint8_t steps[12];
};

extern const struct scale_def SCALES[SCALE_COUNT];

struct scale_ctx {
	uint8_t root;          /* 0..11 */
	uint8_t scale;         /* index into SCALES, offered by the tuning */
	uint8_t octave;        /* 0..7 */
	uint8_t layout;        /* enum layout */
	uint8_t absolute_root; /* nonzero: pads keep absolute pitch when root changes */
	uint8_t tuning;        /* tuning id (scales/tuning.h) */
};

/* Note number of a grid pad (11..88), or -1 if not a grid pad or outside
 * 0..127. Note numbers are degree numbers: see scales/tuning.h. */
int16_t scale_pad_pitch(const struct scale_ctx *c, uint8_t pad);

/* Colour role of a note within the scale; ROLE_OFF for -1. */
enum role scale_pitch_role(const struct scale_ctx *c, int16_t pitch);

#endif
