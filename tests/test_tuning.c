#include <string.h>
#include "test.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "instrument/settings.h"

static const struct tuning_def *T(uint8_t id)
{
	return tuning_find(id);
}

static void test_library(void)
{
	uint8_t i;

	CHECK_EQ(TUNING_COUNT, 29);
	for (i = 0; i < TUNING_COUNT; i++) {
		const struct tuning_def *t = &TUNINGS[i];
		uint8_t j;

		CHECK(tuning_find(t->id) == t);
		CHECK_EQ(tuning_index(t->id), i);
		CHECK_EQ(t->id / 32, t->family);
		if (i)
			CHECK(t->id > TUNINGS[i - 1].id);
		CHECK(t->degrees >= 1 && t->degrees <= 53);
		/* pitches ascend from 0 inside the period */
		CHECK_EQ(tuning_degree_fine(t, 0), 0);
		for (j = 1; j < t->degrees; j++)
			CHECK(tuning_degree_fine(t, j) > tuning_degree_fine(t, j - 1));
		CHECK_EQ(tuning_degree_fine(t, t->degrees), (int32_t)(t->period + (t->period_frac >= 128)));
		CHECK_EQ(tuning_degree_fine(t, -t->degrees), -(int32_t)(t->period + (t->period_frac >= 128)));
		CHECK(t->period == 393216 ? t->period_frac == 0 : t->family == TUNING_XENO);
		/* the default scale is offered, and every offered scale fits the
		 * tuning: twelve-note scales only in twelve-note tunings (or
		 * through the chroma map), tuned ones only in theirs */
		CHECK(tuning_offers(t, t->default_scale));
		for (j = 0; j < tuning_scale_slots(t); j++) {
			const struct scale_def *s = &SCALES[tuning_scale_at(t, j)];
			if (s->degrees)
				CHECK_EQ(s->degrees, t->degrees);
			else if (s->n)
				CHECK(t->degrees == 12 || t->chroma);
		}
		CHECK_EQ(tuning_scale_at(t, tuning_scale_slots(t)), 0xFF);
	}
	CHECK(tuning_find(12) == 0);
	CHECK(tuning_get(12) == &TUNINGS[0]);
	CHECK_EQ(tuning_index(200), -1);
	CHECK(strcmp(T(96)->name, "22 Shruti (Danielou)") == 0);
	/* the 3:1 tritave, 1901.955 c; ten of them land within a fine unit */
	CHECK_EQ(T(128)->period, 623232);
	CHECK_EQ(tuning_degree_fine(T(128), 130), 6232326);
}

static void test_degrees_and_slots(void)
{
	int32_t i;

	for (i = -30; i < 30; i++) {
		CHECK_EQ(tuning_degree_fine(T(0), i), i * 32768);
		CHECK_EQ(tuning_slot_to_degree(T(0), i), i);
		CHECK_EQ(tuning_slot_to_degree(T(32), i), i);
		CHECK_EQ(tuning_slot_to_degree(T(1), i), 2 * i);
	}
	CHECK_EQ(tuning_degree_fine(T(1), 1), 16384);
	CHECK_EQ(tuning_degree_fine(T(32), 4), 126587);  /* 5/4: 386.31 c */
	/* 17-EDO: 600 c sits midway between degrees 8 and 9, ties go down */
	CHECK_EQ(tuning_slot_to_degree(T(5), 6), 8);
	CHECK_EQ(tuning_slot_to_degree(T(11), 5), 22);   /* 53-EDO fourth */
	/* shruti: komal Re is the 16/15 shruti, not the nearest one */
	CHECK_EQ(tuning_slot_to_degree(T(96), 1), 2);
	CHECK_EQ(tuning_slot_to_degree(T(96), 13), 24);
	CHECK_EQ(tuning_slot_to_degree(T(96), -1), -2);
	/* Bohlen-Pierce: 1200 c is nearest degree 8 (1170 c) */
	CHECK_EQ(tuning_slot_to_degree(T(128), 12), 8);
	CHECK_EQ(tuning_slot_to_degree(T(128), 19), 13);
	/* Carlos Alpha: one 77.995 c step */
	CHECK_EQ(tuning_slot_to_degree(T(129), 5), 6);
}

static void test_a4(void)
{
	static const struct { uint16_t a4; int32_t fine; } REF[] = {
		{4400, 0}, {4150, -33184}, {4320, -10409}, {4330, -9098},
		{4420, 2573}, {4660, 32569}, {4401, 129}, {4399, -129},
	};
	unsigned i;

	for (i = 0; i < sizeof REF / sizeof REF[0]; i++) {
		int32_t d = tuning_a4_fine(REF[i].a4) - REF[i].fine;
		CHECK(d >= -1 && d <= 1);
	}
	CHECK(tuning_is_standard(0, 4400));
	CHECK(!tuning_is_standard(0, 4320));
	CHECK(!tuning_is_standard(32, 4400));
}

static void test_pitch79(void)
{
	uint8_t i, n, root;

	for (n = 0; n < 128; n++)
		CHECK_EQ(tuning_pitch79(0, 5, 4400, n), n * 512);
	/* just major third over C4 */
	CHECK_EQ(tuning_pitch79(32, 0, 4400, 64), 32698);
	/* over D4, at 432 Hz */
	CHECK_EQ(tuning_pitch79(32, 2, 4320, 66), (uint16_t)((66 * 32768 + 126587 - 32768 * 4 - 10409 + 32) >> 6) + 0);
	/* 24-EDO: the root's note is the tonic; each note a quarter-tone */
	CHECK_EQ(tuning_pitch79(1, 2, 4400, 64), 62 * 512);
	CHECK_EQ(tuning_pitch79(1, 2, 4400, 65), 62 * 512 + 256);
	CHECK_EQ(tuning_pitch79(1, 2, 4400, 60), 60 * 512);
	/* the ends clamp */
	CHECK_EQ(tuning_pitch79(32, 11, 4660, 127), 0xFFFF);
	CHECK_EQ(tuning_pitch79(0, 0, 4150, 0), 0);
	/* higher notes never sound lower */
	for (i = 0; i < TUNING_COUNT; i++)
		for (root = 0; root < 12; root++)
			for (n = 1; n < 128; n++)
				CHECK(tuning_pitch79(TUNINGS[i].id, root, 4400, n) >=
				      tuning_pitch79(TUNINGS[i].id, root, 4400, n - 1));
}

static void test_tuned_layouts(void)
{
	struct scale_ctx c = {0, 32, 4, LAYOUT_INKEY_4THS, 0, 1}; /* 24-EDO Rast on C */

	CHECK_EQ(scale_pad_pitch(&c, 11), 60);
	CHECK_EQ(scale_pad_pitch(&c, 12), 64);
	CHECK_EQ(scale_pad_pitch(&c, 21), 70);
	CHECK_EQ(scale_pad_pitch(&c, 18), 84);   /* the octave: 24 degrees up */
	CHECK_EQ(scale_pitch_role(&c, 60), ROLE_ROOT);
	CHECK_EQ(scale_pitch_role(&c, 64), ROLE_SCALE);
	CHECK_EQ(scale_pitch_role(&c, 67), ROLE_THIRD);
	CHECK_EQ(scale_pitch_role(&c, 74), ROLE_FIFTH);
	CHECK_EQ(scale_pitch_role(&c, 61), ROLE_OUT);
	CHECK_EQ(scale_pitch_role(&c, 36), ROLE_ROOT);

	/* root D: the tonic is note 64 (C + 4 quarter-tones); absolute root
	 * starts at the first scale note at or above the octave's C */
	c.root = 2;
	CHECK_EQ(scale_pad_pitch(&c, 11), 64);
	c.absolute_root = 1;
	CHECK_EQ(scale_pad_pitch(&c, 11), 61);
	CHECK_EQ(scale_pad_pitch(&c, 12), 64);

	/* chromatic: one degree a column, a fourth a row */
	c.tuning = 11;
	c.scale = SCALE_ALL_DEGREES;
	c.layout = LAYOUT_CHROM_4THS;
	c.absolute_root = 0;
	c.root = 0;
	c.octave = 3;
	CHECK_EQ(scale_pad_pitch(&c, 11), 7);
	CHECK_EQ(scale_pad_pitch(&c, 12), 8);
	CHECK_EQ(scale_pad_pitch(&c, 21), 29);
	c.octave = 4;
	CHECK_EQ(scale_pad_pitch(&c, 88), -1);   /* past note 127 */
	c.layout = LAYOUT_CHROM_GUITAR;
	c.octave = 3;
	CHECK_EQ(scale_pad_pitch(&c, 51) - scale_pad_pitch(&c, 41), 18); /* a major third */

	/* All Degrees: every degree in a row, no 3rd or 5th highlight */
	c.layout = LAYOUT_INKEY_SEQ;
	c.tuning = 129;
	c.octave = 4;
	CHECK_EQ(scale_pad_pitch(&c, 11), 60);
	CHECK_EQ(scale_pad_pitch(&c, 21), 68);
	CHECK_EQ(scale_pitch_role(&c, 60), ROLE_ROOT);
	CHECK_EQ(scale_pitch_role(&c, 62), ROLE_SCALE);
}

static void test_every_tuning_scale_and_layout(void)
{
	uint8_t i, j, l, root, row, col;

	for (i = 0; i < TUNING_COUNT; i++)
		for (j = 0; j < tuning_scale_slots(&TUNINGS[i]); j++)
			for (l = 0; l < LAYOUT_COUNT; l++)
				for (root = 0; root < 12; root += 5) {
					struct scale_ctx c = {root, tuning_scale_at(&TUNINGS[i], j), 4, l, root & 1,
					                      TUNINGS[i].id};
					int lit = 0;

					for (row = 1; row <= 8; row++)
						for (col = 1; col <= 8; col++) {
							int16_t p = scale_pad_pitch(&c, row * 10 + col);
							if (p == -1)
								continue;
							lit++;
							CHECK(p >= 0 && p <= 127);
							if (l < LAYOUT_CHROM_4THS)
								CHECK(scale_pitch_role(&c, p) != ROLE_OUT);
						}
					CHECK(lit > 0);
				}
}

void test_tuning_all(void)
{
	RUN(test_library);
	RUN(test_degrees_and_slots);
	RUN(test_a4);
	RUN(test_pitch79);
	RUN(test_tuned_layouts);
	RUN(test_every_tuning_scale_and_layout);
}
