#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "instrument/settings.h"

/* The instrument's user interface: the play page (grid pads to the pad
 * engine, octave and root arrows), the Setup menu (tap toggles, hold peeks)
 * with its Scale, Options, Tuning and Surface pages, and every LED. It owns
 * the settings.
 *
 * Surface: the play page lights each pad by its role (scales/scales.h) in
 * the chosen palette (instrument/palette.h). With split on, rows 1..4 use
 * the palette and channel, rows 5..8 the upper palette and upper channel.
 * Runs only from the main loop: not reentrant.
 *
 * The caller runs pe_init(adc_buffer, ui_on_sounding) before ui_init. */

/* Setup held at least this long, then released, closes the menu it opened. */
#define SETUP_PEEK_MS 300

/* settings_load, pe_configure, draw the play page */
void ui_init(uint32_t now);
/* every TYPEPAD event: index 1..98, value 0 = release */
void ui_pad(uint8_t index, uint8_t value, uint32_t now);
/* TYPESETUP events: value 0 = release */
void ui_setup(uint8_t value, uint32_t now);
/* pe_poll, Setup LED every 100 ms, settings_poll; every main loop pass.
 * The settings save (a flash page erase that stalls the device) also waits
 * until no note sounds and, on the play page, no grid pad was pressed or
 * released for SETTINGS_SAVE_QUIET_MS. */
void ui_poll(uint32_t now);
const struct settings *ui_settings(void);
/* Invalid or identical settings: no change. Otherwise release every note,
 * copy, reconfigure the pad engine, redraw and settings_touch. */
void ui_apply_settings(const struct settings *s, uint32_t now);
uint8_t ui_menu_open(void);
/* The USB alternate setting or protocol changed (the transport has ended
 * every Standalone note): pe_forget_all, nothing lights as sounding, redraw
 * the play page (if the menu is closed) and the Setup LED. */
void ui_transport_changed(uint32_t now);
/* the pad engine's pe_sounding_fn */
void ui_on_sounding(uint8_t pitch, uint8_t ch, uint8_t on);

#endif
