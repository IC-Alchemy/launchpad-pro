#include <string.h>
#include "test.h"
#include "scales/scales.h"

static const struct scale_ctx DEF = {0, 0, 4, LAYOUT_INKEY_4THS, 0};

static void test_default_bottom_left_is_c3(void)
{
	CHECK_EQ(scale_pad_pitch(&DEF, 11), 60);
	CHECK_EQ(scale_pad_pitch(&DEF, 12), 62);
	CHECK_EQ(scale_pad_pitch(&DEF, 21), 65);
	CHECK_EQ(scale_pad_pitch(&DEF, 18), 72);
}

static void test_inkey_layouts_only_scale_notes(void)
{
	static const uint8_t scales[] = {0, 9, 11, 24};
	uint8_t l, si, root, row, col;

	for (l = 0; l < 3; l++)
		for (si = 0; si < 4; si++)
			for (root = 0; root < 12; root++) {
				struct scale_ctx c = {root, scales[si], 4, l, 0};
				for (row = 1; row <= 8; row++)
					for (col = 1; col <= 8; col++) {
						int16_t p = scale_pad_pitch(&c, row * 10 + col);
						if (p != -1)
							CHECK(scale_pitch_role(&c, p) != ROLE_OUT);
					}
			}
}

static void test_inkey_3rds_and_seq_rows(void)
{
	struct scale_ctx c = DEF;
	c.layout = LAYOUT_INKEY_3RDS;
	CHECK_EQ(scale_pad_pitch(&c, 21), 64);
	c.layout = LAYOUT_INKEY_SEQ;
	CHECK_EQ(scale_pad_pitch(&c, 21), 74);
}

static void test_chromatic_4ths(void)
{
	struct scale_ctx c = {2, 0, 4, LAYOUT_CHROM_4THS, 0};
	CHECK_EQ(scale_pad_pitch(&c, 11), 62);
	CHECK_EQ(scale_pad_pitch(&c, 12), 63);
	CHECK_EQ(scale_pad_pitch(&c, 21), 67);
}

static void test_chromatic_guitar(void)
{
	struct scale_ctx c = DEF;
	c.layout = LAYOUT_CHROM_GUITAR;
	CHECK_EQ(scale_pad_pitch(&c, 51), 79);
	CHECK_EQ(scale_pad_pitch(&c, 41), 75);
}

static void test_absolute_root_inkey(void)
{
	struct scale_ctx c = {2, 0, 4, LAYOUT_INKEY_4THS, 1};
	CHECK_EQ(scale_pad_pitch(&c, 11), 61);
	CHECK_EQ(scale_pad_pitch(&c, 12), 62);
}

static void test_absolute_root_chromatic(void)
{
	struct scale_ctx c = {2, 0, 4, LAYOUT_CHROM_4THS, 1};
	CHECK_EQ(scale_pad_pitch(&c, 11), 60);
}

static void test_octave_positions(void)
{
	uint8_t o;
	for (o = 0; o < 8; o++) {
		struct scale_ctx c = {0, 0, o, LAYOUT_INKEY_4THS, 0};
		CHECK_EQ(scale_pad_pitch(&c, 11), 12 * (o + 1));
	}
}

static void test_out_of_range_is_minus1(void)
{
	struct scale_ctx c = {7, 0, 7, LAYOUT_CHROM_4THS, 0};
	CHECK_EQ(scale_pad_pitch(&c, 88), -1);
	CHECK_EQ(scale_pad_pitch(&DEF, 10), -1);
	CHECK_EQ(scale_pad_pitch(&DEF, 19), -1);
	CHECK_EQ(scale_pad_pitch(&DEF, 91), -1);
}

static void test_roles(void)
{
	CHECK_EQ(scale_pitch_role(&DEF, 60), ROLE_ROOT);
	CHECK_EQ(scale_pitch_role(&DEF, 64), ROLE_THIRD);
	CHECK_EQ(scale_pitch_role(&DEF, 67), ROLE_FIFTH);
	CHECK_EQ(scale_pitch_role(&DEF, 62), ROLE_SCALE);
	CHECK_EQ(scale_pitch_role(&DEF, 61), ROLE_OUT);
	CHECK_EQ(scale_pitch_role(&DEF, -1), ROLE_OFF);
	/* every octave */
	CHECK_EQ(scale_pitch_role(&DEF, 52), ROLE_THIRD);
	CHECK_EQ(scale_pitch_role(&DEF, 79), ROLE_FIFTH);
	CHECK_EQ(scale_pitch_role(&DEF, 72), ROLE_ROOT);
}

static void test_roles_follow_the_scale_steps(void)
{
	/* the third and fifth are the scale's third and fifth steps: minor
	 * on A has C as its third, E as its fifth */
	struct scale_ctx c = {9, 1, 4, LAYOUT_INKEY_4THS, 0, 0};
	struct scale_ctx chrom = {0, SCALE_CHROMATIC, 4, LAYOUT_CHROM_4THS, 0, 0};

	CHECK_EQ(scale_pitch_role(&c, 69), ROLE_ROOT);
	CHECK_EQ(scale_pitch_role(&c, 72), ROLE_THIRD);
	CHECK_EQ(scale_pitch_role(&c, 76), ROLE_FIFTH);
	CHECK_EQ(scale_pitch_role(&c, 74), ROLE_SCALE);
	CHECK_EQ(scale_pitch_role(&c, 73), ROLE_OUT);
	/* chromatic has no third or fifth: only the root stands out */
	CHECK_EQ(scale_pitch_role(&chrom, 60), ROLE_ROOT);
	CHECK_EQ(scale_pitch_role(&chrom, 64), ROLE_SCALE);
	CHECK_EQ(scale_pitch_role(&chrom, 67), ROLE_SCALE);
}

static void test_scale_table_sane(void)
{
	int i, j;
	for (i = 0; i < SCALE_COUNT; i++) {
		const struct scale_def *s = &SCALES[i];
		/* twelve-note scales first, then All Degrees, then tuned ones */
		CHECK_EQ(s->degrees == 0, i <= SCALE_ALL_DEGREES);
		if (i == SCALE_ALL_DEGREES) {
			CHECK_EQ(s->n, 0);
			continue;
		}
		CHECK(s->n >= 5 && s->n <= 12);
		CHECK_EQ(s->steps[0], 0);
		for (j = 1; j < s->n; j++)
			CHECK(s->steps[j] > s->steps[j - 1]);
		CHECK(s->steps[s->n - 1] < (s->degrees ? s->degrees : 12));
	}
	CHECK(strcmp(SCALES[0].name, "Major") == 0);
	CHECK(strcmp(SCALES[24].name, "Spanish") == 0);
	CHECK(strcmp(SCALES[SCALE_CHROMATIC].name, "Chromatic") == 0);
	CHECK(strcmp(SCALES[SCALE_ALL_DEGREES].name, "All Degrees") == 0);
}

void test_scales_all(void)
{
	RUN(test_default_bottom_left_is_c3);
	RUN(test_inkey_layouts_only_scale_notes);
	RUN(test_inkey_3rds_and_seq_rows);
	RUN(test_chromatic_4ths);
	RUN(test_chromatic_guitar);
	RUN(test_absolute_root_inkey);
	RUN(test_absolute_root_chromatic);
	RUN(test_octave_positions);
	RUN(test_out_of_range_is_minus1);
	RUN(test_roles);
	RUN(test_roles_follow_the_scale_steps);
	RUN(test_scale_table_sane);
}
