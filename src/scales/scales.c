#include "scales/scales.h"

const struct scale_def SCALES[SCALE_COUNT] = {
	{"Major",            7, {0, 2, 4, 5, 7, 9, 11}},
	{"Minor",            7, {0, 2, 3, 5, 7, 8, 10}},
	{"Dorian",           7, {0, 2, 3, 5, 7, 9, 10}},
	{"Mixolydian",       7, {0, 2, 4, 5, 7, 9, 10}},
	{"Lydian",           7, {0, 2, 4, 6, 7, 9, 11}},
	{"Phrygian",         7, {0, 1, 3, 5, 7, 8, 10}},
	{"Locrian",          7, {0, 1, 3, 5, 6, 8, 10}},
	{"Diminished",       8, {0, 1, 3, 4, 6, 7, 9, 10}},
	{"Whole-half",       8, {0, 2, 3, 5, 6, 8, 9, 11}},
	{"Whole Tone",       6, {0, 2, 4, 6, 8, 10}},
	{"Minor Blues",      6, {0, 3, 5, 6, 7, 10}},
	{"Minor Pentatonic", 5, {0, 3, 5, 7, 10}},
	{"Major Pentatonic", 5, {0, 2, 4, 7, 9}},
	{"Harmonic Minor",   7, {0, 2, 3, 5, 7, 8, 11}},
	{"Melodic Minor",    7, {0, 2, 3, 5, 7, 9, 11}},
	{"Super Locrian",    7, {0, 1, 3, 4, 6, 8, 10}},
	{"Bhairav",          7, {0, 1, 4, 5, 7, 8, 11}},
	{"Hungarian Minor",  7, {0, 2, 3, 6, 7, 8, 11}},
	{"Minor Gypsy",      7, {0, 1, 4, 5, 7, 8, 10}},
	{"Hirojoshi",        5, {0, 2, 3, 7, 8}},
	{"In-Sen",           5, {0, 1, 5, 7, 10}},
	{"Iwato",            5, {0, 1, 5, 6, 10}},
	{"Kumoi",            5, {0, 2, 3, 7, 9}},
	{"Pelog",            5, {0, 1, 3, 7, 8}},
	{"Spanish",          8, {0, 1, 3, 4, 5, 6, 8, 10}},
};

static const uint8_t GUITAR_ROW[8] = {0, 5, 10, 15, 19, 24, 29, 34};

int16_t scale_pad_pitch(const struct scale_ctx *c, uint8_t pad)
{
	uint8_t r, col;
	int16_t pitch;
	int16_t base = 12 * (c->octave + 1);

	if (pad < 11 || pad > 88)
		return -1;
	r = pad / 10;
	col = pad % 10;
	if (r < 1 || r > 8 || col < 1 || col > 8)
		return -1;
	r--;
	col--;

	if (c->layout == LAYOUT_CHROM_4THS || c->layout == LAYOUT_CHROM_GUITAR) {
		pitch = base + (c->absolute_root ? 0 : c->root) + col;
		pitch += (c->layout == LAYOUT_CHROM_4THS) ? 5 * r : GUITAR_ROW[r];
	} else {
		const struct scale_def *s = &SCALES[c->scale];
		uint8_t k = c->layout == LAYOUT_INKEY_4THS ? 3
		          : c->layout == LAYOUT_INKEY_3RDS ? 2 : 8;
		int16_t anchor, d, d0 = 0;

		if (c->absolute_root) {
			anchor = 12 * c->octave + c->root;
			while (anchor + 12 * (d0 / s->n) + s->steps[d0 % s->n] < base)
				d0++;
		} else {
			anchor = base + c->root;
		}
		d = d0 + col + k * r;
		pitch = anchor + 12 * (d / s->n) + s->steps[d % s->n];
	}
	return (pitch < 0 || pitch > 127) ? -1 : pitch;
}

enum role scale_pitch_role(const struct scale_ctx *c, int16_t pitch)
{
	const struct scale_def *s = &SCALES[c->scale];
	uint8_t pc, i;

	if (pitch < 0)
		return ROLE_OFF;
	pc = (uint8_t)((pitch + 12 - c->root % 12) % 12);
	if (pc == 0)
		return ROLE_ROOT;
	if (pc == s->steps[2] || pc == s->steps[4])
		return ROLE_HIGHLIGHT;
	for (i = 0; i < s->n; i++)
		if (pc == s->steps[i])
			return ROLE_SCALE;
	return ROLE_OUT;
}
