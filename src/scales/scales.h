#ifndef SCALES_H
#define SCALES_H

#include <stdint.h>

#define SCALE_COUNT 25

enum layout {
	LAYOUT_INKEY_4THS,
	LAYOUT_INKEY_3RDS,
	LAYOUT_INKEY_SEQ,
	LAYOUT_CHROM_4THS,
	LAYOUT_CHROM_GUITAR,
	LAYOUT_COUNT
};

enum role { ROLE_OFF, ROLE_OUT, ROLE_SCALE, ROLE_HIGHLIGHT, ROLE_ROOT };

struct scale_def {
	const char *name;
	uint8_t n;
	uint8_t steps[8];
};

extern const struct scale_def SCALES[SCALE_COUNT];

struct scale_ctx {
	uint8_t root;          /* 0..11 */
	uint8_t scale;         /* index into SCALES */
	uint8_t octave;        /* 0..7 */
	uint8_t layout;        /* enum layout */
	uint8_t absolute_root; /* nonzero: pads keep absolute pitch when root changes */
};

/* Pitch of a grid pad (11..88), or -1 if not a grid pad or outside 0..127. */
int16_t scale_pad_pitch(const struct scale_ctx *c, uint8_t pad);

/* Colour role of a pitch within the scale; ROLE_OFF for -1. */
enum role scale_pitch_role(const struct scale_ctx *c, int16_t pitch);

#endif
