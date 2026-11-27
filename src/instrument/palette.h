#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>
#include "scales/scales.h"

/* Play-page colour palettes: one LED colour (0..63 per channel) per pad
 * role. Tables live in flash; nothing here uses RAM.
 *
 * Every palette follows the same rules, so switching palettes never changes
 * what a colour means:
 *   - Lightness carries the meaning, hue backs it up (readable with colour
 *     blindness and in daylight): root and fifth are the bright pair, the
 *     third is dim, scale notes are faint, out-of-scale notes fainter still.
 *   - Root: the palette's signature hue with one channel at full drive. No
 *     other role uses that hue.
 *   - Fifth: a neighbouring hue, as bright or brighter, so it reads as
 *     related to the root yet not the root, and clearly apart from the rest.
 *   - Third: a contrasting hue, kept dim so it shows without shouting.
 *   - Scale: a faint tint of the root's hue, so the scale reads as one.
 *   - Out of scale: a faint neutral grey, the same in every palette.
 *   - No role is white or near white: white means "sounding".
 * Indexes are saved in the settings and named in X-SurfaceSettings. */

enum {
	PALETTE_OCEAN,
	PALETTE_CITRUS,
	PALETTE_GLACIER,
	PALETTE_CONTRAST,
	PALETTE_COUNT,
};

/* The colour of a role in a palette; an unknown palette reads as Ocean. */
const uint8_t *palette_rgb(uint8_t palette, enum role r);
/* Property Exchange name ("ocean", "citrus", ...); "" for an unknown palette. */
const char *palette_name(uint8_t palette);

#endif
