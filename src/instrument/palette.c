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
	/* Citrus: gold root, yellow fifth, blue third */
	{{0, 0, 0}, OUT_GREY, {6, 4, 0}, {0, 7, 26}, {63, 56, 0}, {63, 34, 0}},
	/* Glacier: cyan root, aquamarine fifth, rose third */
	{{0, 0, 0}, OUT_GREY, {0, 5, 5}, {29, 0, 14}, {0, 63, 34}, {0, 52, 63}},
	/* Contrast: the orange/blue axis that stays apart under every common
	 * colour blindness (Okabe-Ito): vermilion root, amber fifth, sky-blue
	 * third */
	{{0, 0, 0}, OUT_GREY, {10, 3, 0}, {2, 8, 21}, {63, 40, 0}, {63, 16, 0}},
};

static const char *const NAMES[PALETTE_COUNT] = {
	"ocean", "citrus", "glacier", "contrast",
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
