#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>
#include "scales/scales.h"

/* Play-page colour palettes: one LED colour (0..63 per channel) per pad
 * role. Tables live in flash; nothing here uses RAM.
 *
 * Hue identifies the palette across every lit role, including out-of-scale
 * notes. Lightness identifies the role within the palette: fifth is
 * brightest, then root, third, scale, and out of scale. Off is black, and
 * the play page uses white for sounding pads.
 * Indexes are saved in the settings and named in X-SurfaceSettings. */

enum {
	PALETTE_OCEAN,
	PALETTE_CITRUS,
	PALETTE_GLACIER,
	PALETTE_CONTRAST,
	PALETTE_ROSE,
	PALETTE_AMETHYST,
	PALETTE_COUNT,
};

/* The colour of a role in a palette; an unknown palette reads as Ocean. */
const uint8_t *palette_rgb(uint8_t palette, enum role r);
/* Property Exchange name ("ocean", "citrus", ...); "" for an unknown palette. */
const char *palette_name(uint8_t palette);

#endif
