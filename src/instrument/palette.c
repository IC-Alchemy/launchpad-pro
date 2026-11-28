#include "instrument/palette.h"

/* [palette][role]: OFF, OUT, SCALE, THIRD, FIFTH, ROOT.
 * Each row keeps to one hue family at every brightness. Keep palette
 * indexes and protocol names stable for saved settings. */
static const uint8_t PALETTE_RGB[PALETTE_COUNT][ROLE_COUNT][3] = {
	[PALETTE_OCEAN]    = {{0, 0, 0}, {0, 1, 4}, {0, 3, 10}, {1, 8, 24}, {0, 37, 63}, {0, 19, 63}},
	[PALETTE_CITRUS]   = {{0, 0, 0}, {3, 3, 0}, {7, 7, 0}, {18, 17, 0}, {58, 63, 0}, {29, 28, 0}},
	[PALETTE_GLACIER]  = {{0, 0, 0}, {0, 3, 2}, {0, 8, 6}, {0, 17, 12}, {0, 56, 55}, {0, 29, 23}},
	[PALETTE_CONTRAST] = {{0, 0, 0}, {6, 1, 0}, {13, 2, 0}, {27, 5, 0}, {63, 28, 0}, {63, 10, 0}},
	[PALETTE_ROSE]     = {{0, 0, 0}, {5, 0, 2}, {13, 0, 6}, {32, 0, 14}, {63, 13, 28}, {63, 0, 24}},
	[PALETTE_AMETHYST] = {{0, 0, 0}, {2, 0, 6}, {6, 0, 17}, {15, 1, 30}, {44, 8, 63}, {24, 2, 63}},
};

static const char *const NAMES[PALETTE_COUNT] = {
	"ocean", "citrus", "glacier", "contrast", "rose", "amethyst",
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
