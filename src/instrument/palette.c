#include "instrument/palette.h"

#define OUT_GREY {2, 2, 2}

/* [palette][role]: OFF, OUT, SCALE, THIRD, FIFTH, ROOT. Within each
 * palette, lightness distinguishes roles even when the blue hues look alike.
 * Keep the palette indexes and protocol names stable for saved settings. */
static const uint8_t PALETTE_RGB[PALETTE_COUNT][ROLE_COUNT][3] = {
	/* Ocean: electric blue root, azure fifth, steel-blue third */
	{{0, 0, 0}, OUT_GREY, {0, 3, 10}, {5, 8, 15}, {0, 44, 58}, {0, 19, 63}},
	/* Citrus (legacy ID): indigo root, cornflower fifth, denim third */
	{{0, 0, 0}, OUT_GREY, {2, 2, 12}, {2, 9, 16}, {12, 39, 63}, {13, 15, 63}},
	/* Glacier: azure root, ice-blue fifth, indigo third */
	{{0, 0, 0}, OUT_GREY, {0, 4, 8}, {9, 6, 24}, {21, 55, 63}, {0, 30, 63}},
	/* Contrast: cobalt root, cyan-blue fifth, slate-blue third */
	{{0, 0, 0}, OUT_GREY, {1, 3, 11}, {8, 10, 17}, {0, 58, 63}, {4, 17, 63}},
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
