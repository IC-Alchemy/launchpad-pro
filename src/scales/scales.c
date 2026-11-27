#include "scales/scales.h"
#include "scales/tuning.h"

const struct scale_def SCALES[SCALE_COUNT] = {
	{"Major",            7, 0, {0, 2, 4, 5, 7, 9, 11}},
	{"Minor",            7, 0, {0, 2, 3, 5, 7, 8, 10}},
	{"Dorian",           7, 0, {0, 2, 3, 5, 7, 9, 10}},
	{"Mixolydian",       7, 0, {0, 2, 4, 5, 7, 9, 10}},
	{"Lydian",           7, 0, {0, 2, 4, 6, 7, 9, 11}},
	{"Phrygian",         7, 0, {0, 1, 3, 5, 7, 8, 10}},
	{"Locrian",          7, 0, {0, 1, 3, 5, 6, 8, 10}},
	{"Diminished",       8, 0, {0, 1, 3, 4, 6, 7, 9, 10}},
	{"Whole-half",       8, 0, {0, 2, 3, 5, 6, 8, 9, 11}},
	{"Whole Tone",       6, 0, {0, 2, 4, 6, 8, 10}},
	{"Minor Blues",      6, 0, {0, 3, 5, 6, 7, 10}},
	{"Minor Pentatonic", 5, 0, {0, 3, 5, 7, 10}},
	{"Major Pentatonic", 5, 0, {0, 2, 4, 7, 9}},
	{"Harmonic Minor",   7, 0, {0, 2, 3, 5, 7, 8, 11}},
	{"Melodic Minor",    7, 0, {0, 2, 3, 5, 7, 9, 11}},
	{"Super Locrian",    7, 0, {0, 1, 3, 4, 6, 8, 10}},
	{"Bhairav",          7, 0, {0, 1, 4, 5, 7, 8, 11}},
	{"Hungarian Minor",  7, 0, {0, 2, 3, 6, 7, 8, 11}},
	{"Minor Gypsy",      7, 0, {0, 1, 4, 5, 7, 8, 10}},
	{"Hirojoshi",        5, 0, {0, 2, 3, 7, 8}},
	{"In-Sen",           5, 0, {0, 1, 5, 7, 10}},
	{"Iwato",            5, 0, {0, 1, 5, 6, 10}},
	{"Kumoi",            5, 0, {0, 2, 3, 7, 9}},
	{"Pelog",            5, 0, {0, 1, 3, 7, 8}},
	{"Spanish",          8, 0, {0, 1, 3, 4, 5, 6, 8, 10}},
	/* from Pico2Seq (src/pico2seq-core/scales), MIT licence */
	{"Phrygian Dominant", 7, 0, {0, 1, 4, 5, 7, 8, 10}},
	{"Lydian Dominant",   7, 0, {0, 2, 4, 6, 7, 9, 10}},
	{"Marwa Thaat",       7, 0, {0, 1, 4, 6, 7, 9, 11}},
	{"Poorvi Thaat",      7, 0, {0, 1, 4, 6, 7, 8, 11}},
	{"Todi Thaat",        7, 0, {0, 1, 3, 6, 7, 8, 11}},
	{"Chromatic",        12, 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
	{"All Degrees",       0, 0, {0}},
	{"Maqam Rast",        7, 24, {0, 4, 7, 10, 14, 18, 21}},
	{"Maqam Bayati",      7, 24, {0, 3, 6, 10, 14, 16, 20}},
	{"Maqam Hijaz",       7, 24, {0, 2, 8, 10, 14, 16, 20}},
	{"Maqam Saba",        7, 24, {0, 3, 6, 8, 14, 16, 20}},
	{"19-EDO Major",      7, 19, {0, 3, 6, 8, 11, 14, 17}},
	{"19-EDO Minor",      7, 19, {0, 3, 5, 8, 11, 13, 16}},
	{"19-EDO Pentatonic", 5, 19, {0, 3, 6, 11, 14}},
	{"31-EDO Major",      7, 31, {0, 5, 10, 13, 18, 23, 28}},
	{"31-EDO Minor",      7, 31, {0, 5, 8, 13, 18, 21, 26}},
	{"31-EDO Pentatonic", 5, 31, {0, 5, 10, 18, 23}},
	{"22-EDO Major",      7, 22, {0, 4, 8, 9, 13, 17, 21}},
	{"22-EDO Minor",      7, 22, {0, 4, 5, 9, 13, 14, 18}},
	{"22-EDO Pentatonic", 5, 22, {0, 4, 8, 13, 17}},
	{"17-EDO Major",      7, 17, {0, 3, 6, 7, 10, 13, 16}},
	{"17-EDO Minor",      7, 17, {0, 3, 4, 7, 10, 11, 14}},
	{"15-EDO Heptatonic", 7, 15, {0, 2, 4, 6, 8, 11, 13}},
	{"15-EDO Pentatonic", 5, 15, {0, 3, 6, 9, 12}},
	{"10-EDO Pentatonic", 5, 10, {0, 2, 4, 6, 8}},
	{"41-EDO Major",      7, 41, {0, 7, 14, 17, 24, 31, 38}},
	{"41-EDO Minor",      7, 41, {0, 7, 10, 17, 24, 27, 34}},
	{"41-EDO Pentatonic", 5, 41, {0, 7, 14, 24, 31}},
	{"53-EDO Major",      7, 53, {0, 9, 18, 22, 31, 40, 49}},
	{"53-EDO Minor",      7, 53, {0, 9, 13, 22, 31, 35, 44}},
	{"53-EDO Pentatonic", 5, 53, {0, 9, 18, 31, 40}},
	{"Overtone Heptatonic", 7, 16, {0, 2, 4, 6, 8, 11, 14}},
	{"Overtone Pentatonic", 5, 16, {0, 2, 4, 8, 11}},
	{"Partch Major",      7, 43, {0, 8, 14, 18, 25, 31, 39}},
	{"Partch Minor",      7, 43, {0, 8, 12, 18, 25, 29, 36}},
	{"Bohlen-Pierce Lambda", 9, 13, {0, 2, 3, 4, 6, 7, 9, 10, 12}},
};

static const uint8_t GUITAR_ROW[8] = {0, 5, 10, 15, 19, 24, 29, 34};

/* steps in one period of this scale in this tuning */
static int32_t steps_of(const struct tuning_def *t, const struct scale_def *s)
{
	return s->n ? s->n : t->degrees;
}

/* ladder degree of step i (0..steps_of - 1) above the tonic */
static int32_t step_degree(const struct tuning_def *t, const struct scale_def *s, int32_t i)
{
	if (!s->n)
		return i;
	if (s->degrees)
		return s->steps[i];
	return tuning_slot_to_degree(t, s->steps[i]);
}

/* degree of the d-th scale step (d >= 0) above the tonic */
static int32_t ladder(const struct tuning_def *t, const struct scale_def *s, int32_t d)
{
	int32_t n = steps_of(t, s);

	return t->degrees * (d / n) + step_degree(t, s, d % n);
}

int16_t scale_pad_pitch(const struct scale_ctx *c, uint8_t pad)
{
	const struct tuning_def *t = tuning_get(c->tuning);
	int32_t n = t->degrees;
	int32_t tonic = tuning_tonic_note(t, c->root);
	/* the note of the octave's C: 12 * (octave + 1) in a twelve-note tuning */
	int32_t base = 60 + n * ((int32_t)c->octave - 4);
	int32_t pitch;
	uint8_t r, col;

	if (pad < 11 || pad > 88)
		return -1;
	r = pad / 10;
	col = pad % 10;
	if (r < 1 || r > 8 || col < 1 || col > 8)
		return -1;
	r--;
	col--;

	if (c->layout == LAYOUT_CHROM_4THS || c->layout == LAYOUT_CHROM_GUITAR) {
		pitch = base + (c->absolute_root ? 0 : tonic - 60) + col;
		pitch += (c->layout == LAYOUT_CHROM_4THS) ? r * tuning_slot_to_degree(t, 5)
		                                          : tuning_slot_to_degree(t, GUITAR_ROW[r]);
	} else {
		const struct scale_def *s = &SCALES[c->scale < SCALE_COUNT ? c->scale : 0];
		uint8_t k = c->layout == LAYOUT_INKEY_4THS ? 3
		          : c->layout == LAYOUT_INKEY_3RDS ? 2 : 8;
		int32_t anchor, d0 = 0;

		if (c->absolute_root) {
			/* the tonic an octave below: the pads start at its first
			 * scale note at or above the octave's C */
			anchor = tonic + n * ((int32_t)c->octave - 5);
			while (anchor + ladder(t, s, d0) < base)
				d0++;
		} else {
			anchor = tonic + n * ((int32_t)c->octave - 4);
		}
		pitch = anchor + ladder(t, s, d0 + col + k * r);
	}
	return (pitch < 0 || pitch > 127) ? -1 : (int16_t)pitch;
}

enum role scale_pitch_role(const struct scale_ctx *c, int16_t pitch)
{
	const struct tuning_def *t = tuning_get(c->tuning);
	const struct scale_def *s = &SCALES[c->scale < SCALE_COUNT ? c->scale : 0];
	int32_t n = steps_of(t, s), deg, i;

	if (pitch < 0)
		return ROLE_OFF;
	deg = (int32_t)pitch - tuning_tonic_note(t, c->root);
	/* a one-degree tuning (Carlos) repeats every step: only the tonic
	 * itself shows as the root */
	if (t->degrees == 1)
		return deg == 0 ? ROLE_ROOT : ROLE_SCALE;
	deg -= t->degrees * tuning_floor_div(deg, t->degrees);
	if (deg == 0)
		return ROLE_ROOT;
	/* the third and fifth steps; All Degrees and Chromatic have none */
	if (s->n > 4 && s->n < 12) {
		if (deg == step_degree(t, s, 2))
			return ROLE_THIRD;
		if (deg == step_degree(t, s, 4))
			return ROLE_FIFTH;
	}
	for (i = 0; i < n; i++)
		if (deg == step_degree(t, s, i))
			return ROLE_SCALE;
	return ROLE_OUT;
}
