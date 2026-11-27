#include "instrument/palette.h"

#define OUT_GREY {2, 2, 2}

/* [palette][role]: OFF, OUT, SCALE, THIRD, FIFTH, ROOT. See palette.h for the
 * rules each row follows. LEDs mix linear light, so the values were set by
 * CIE L* computed from them as linear RGB:
 *   out of scale ~21, scale ~30, third ~37..42 (at least 12 below both the
 *   root and the fifth), root and fifth 52..94, each pair of roles at least
 *   20 apart in CIE76 colour difference.
 * tests/test_palette.c checks the same order with integer luminance. */
static const uint8_t PALETTE_RGB[PALETTE_COUNT][ROLE_COUNT][3] = {
	/* Ocean: blue root, azure fifth, amber third */
	{{0, 0, 0}, OUT_GREY, {0, 4, 15}, {13, 6, 0}, {0, 40, 58}, {0, 16, 63}},
	/* Ember: red root, orange fifth, teal third */
	{{0, 0, 0}, OUT_GREY, {12, 2, 0}, {0, 9, 10}, {63, 20, 0}, {63, 0, 0}},
	/* Forest: green root, lime fifth, violet third */
	{{0, 0, 0}, OUT_GREY, {0, 5, 1}, {22, 0, 33}, {30, 63, 0}, {0, 63, 0}},
	/* Amethyst: violet root, orchid fifth, mint third */
	{{0, 0, 0}, OUT_GREY, {11, 0, 21}, {0, 8, 3}, {63, 14, 63}, {40, 0, 63}},
	/* Citrus: gold root, yellow fifth, blue third */
	{{0, 0, 0}, OUT_GREY, {6, 4, 0}, {0, 7, 26}, {63, 56, 0}, {63, 34, 0}},
	/* Glacier: cyan root, aquamarine fifth, rose third */
	{{0, 0, 0}, OUT_GREY, {0, 5, 5}, {29, 0, 14}, {0, 63, 34}, {0, 52, 63}},
	/* Rose: pink root, light rose fifth, olive third */
	{{0, 0, 0}, OUT_GREY, {17, 0, 6}, {4, 9, 0}, {63, 14, 34}, {63, 0, 22}},
	/* Contrast: the orange/blue axis that stays apart under every common
	 * colour blindness (Okabe-Ito): vermilion root, amber fifth, sky-blue
	 * third */
	{{0, 0, 0}, OUT_GREY, {10, 3, 0}, {2, 8, 21}, {63, 40, 0}, {63, 16, 0}},
};

static const char *const NAMES[PALETTE_COUNT] = {
	"ocean", "ember", "forest", "amethyst", "citrus", "glacier", "rose", "contrast",
};

const uint8_t *palette_rgb(uint8_t palette, enum role r)
{
	if (palette >= PALETTE_COUNT)
		palette = PALETTE_OCEAN;
	if ((unsigned)r >= ROLE_COUNT)
		r = ROLE_OFF;
	return PALETTE_RGB[palette][r];
}

const char *palette_name(uint8_t palette)
{
	return palette < PALETTE_COUNT ? NAMES[palette] : "";
}
