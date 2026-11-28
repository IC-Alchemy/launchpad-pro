#include "test.h"
#include "instrument/palette.h"
#include <string.h>

/* The rules in instrument/palette.h, checked on every palette. LEDs mix
 * linear light, so relative luminance is a weighted sum of the channel
 * values (Rec. 709 weights x 10000); its order is the order of CIE L*. */
static uint32_t luma(const uint8_t *c)
{
	return 2126u * c[0] + 7152u * c[1] + 722u * c[2];
}

static uint8_t same(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a, b, 3) == 0;
}

static uint8_t max3(const uint8_t *c)
{
	uint8_t m = c[0] > c[1] ? c[0] : c[1];

	return m > c[2] ? m : c[2];
}

static uint8_t min3(const uint8_t *c)
{
	uint8_t m = c[0] < c[1] ? c[0] : c[1];

	return m < c[2] ? m : c[2];
}

static void test_palette_lightness_order(void)
{
	uint8_t p;

	for (p = 0; p < PALETTE_COUNT; p++) {
		const uint8_t *off = palette_rgb(p, ROLE_OFF);
		const uint8_t *out = palette_rgb(p, ROLE_OUT);
		const uint8_t *scale = palette_rgb(p, ROLE_SCALE);
		const uint8_t *third = palette_rgb(p, ROLE_THIRD);
		const uint8_t *fifth = palette_rgb(p, ROLE_FIFTH);
		const uint8_t *root = palette_rgb(p, ROLE_ROOT);
		uint32_t bright = luma(root) < luma(fifth) ? luma(root) : luma(fifth);

		CHECK_EQ(luma(off), 0);
		CHECK(luma(out) > 0);
		CHECK(luma(scale) > luma(out));
		CHECK(luma(third) > luma(scale));
		/* root and fifth clearly brighter than everything else: 1.5x the
		 * third's luminance is about 12 L* at the third's level */
		CHECK(luma(third) * 3 < bright * 2);
		/* the bright pair is also distinguishable by lightness */
		CHECK(luma(fifth) * 2 > luma(root) * 3);
	}
}

static void test_palette_roles_distinct(void)
{
	uint8_t p, a, b;

	for (p = 0; p < PALETTE_COUNT; p++) {
		const uint8_t *root = palette_rgb(p, ROLE_ROOT);
		const uint8_t *fifth = palette_rgb(p, ROLE_FIFTH);
		int d = 0, k;

		for (a = ROLE_OFF; a < ROLE_COUNT; a++)
			for (b = (uint8_t)(a + 1); b < ROLE_COUNT; b++)
				CHECK(!same(palette_rgb(p, (enum role)a), palette_rgb(p, (enum role)b)));
		/* Playable roles stay in the blue family; neutral states are exempt. */
		for (a = ROLE_SCALE; a < ROLE_COUNT; a++) {
			const uint8_t *c = palette_rgb(p, (enum role)a);

			CHECK(c[2] > c[0] && c[2] > c[1]);
		}
		/* the root drives one channel fully */
		CHECK_EQ(max3(root), 63);
		/* root and fifth are a related pair, but visibly apart */
		for (k = 0; k < 3; k++)
			d += root[k] > fifth[k] ? root[k] - fifth[k] : fifth[k] - root[k];
		CHECK(d >= 16);
		/* nothing is white or near white: white means sounding */
		for (a = ROLE_OFF; a < ROLE_COUNT; a++)
			CHECK(min3(palette_rgb(p, (enum role)a)) < 32);
		/* the root's colour is no other palette's root */
		for (a = (uint8_t)(p + 1); a < PALETTE_COUNT; a++)
			CHECK(!same(root, palette_rgb(a, ROLE_ROOT)));
	}
}

static void test_palette_out_is_one_grey(void)
{
	uint8_t p;
	const uint8_t *grey = palette_rgb(0, ROLE_OUT);

	CHECK(grey[0] == grey[1] && grey[1] == grey[2]);
	for (p = 1; p < PALETTE_COUNT; p++)
		CHECK(same(palette_rgb(p, ROLE_OUT), grey));
}

static void test_palette_names(void)
{
	uint8_t p, q;

	CHECK(strcmp(palette_name(PALETTE_OCEAN), "ocean") == 0);
	CHECK(strcmp(palette_name(PALETTE_CONTRAST), "contrast") == 0);
	CHECK(strcmp(palette_name(PALETTE_COUNT), "") == 0);
	for (p = 0; p < PALETTE_COUNT; p++) {
		CHECK(palette_name(p)[0] != 0);
		for (q = (uint8_t)(p + 1); q < PALETTE_COUNT; q++)
			CHECK(strcmp(palette_name(p), palette_name(q)) != 0);
	}
	/* an unknown palette or role reads safely */
	CHECK(same(palette_rgb(PALETTE_COUNT, ROLE_ROOT), palette_rgb(PALETTE_OCEAN, ROLE_ROOT)));
	CHECK(same(palette_rgb(0, (enum role)ROLE_COUNT), palette_rgb(0, ROLE_OFF)));
}

void test_palette_all(void)
{
	RUN(test_palette_lightness_order);
	RUN(test_palette_roles_distinct);
	RUN(test_palette_out_is_one_grey);
	RUN(test_palette_names);
}
