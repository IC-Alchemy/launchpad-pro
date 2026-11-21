#ifndef PAD_ENGINE_H
#define PAD_ENGINE_H

#include <stdint.h>

/* Pad engine: turns grid pad presses and releases into notes on the
 * instrument's output seam (instrument/io.h). It owns per-pad state, the
 * three velocity modes, note sharing between pads of the same pitch and
 * (Task 6) pressure. Runs only from the main loop: not reentrant. */

#define PEAK_WINDOW_MS 3        /* >= 2 ms after the press at 1 ms tick resolution */

/* Called when a pitch starts (sounding = 1) or stops (0) sounding: on the
 * same 0 -> 1 and 1 -> 0 edges that send the Note On and Note Off. */
typedef void (*pe_sounding_fn)(uint8_t pitch, uint8_t sounding);

/* adc_buffer: the raw 12-bit buffer from app_init (ADC_MAP order).
 * Configuration starts at the settings defaults (peak-window, linear,
 * poly pressure, medium threshold). on_sounding may be NULL. */
void pe_init(const uint16_t *adc_buffer, pe_sounding_fn on_sounding);

/* VEL_*, CURVE_*, PRESS_*, THRESH_* from instrument/settings.h. */
void pe_configure(uint8_t velocity_mode, uint8_t curve, uint8_t pressure_mode, uint8_t threshold);

/* pad: grid index 11..88. pitch -1 (or any value outside 0..127): ignored.
 * ch: 0..15. din_mirror: also send on DINMIDI. now: ms counter. */
void pe_press(uint8_t pad, int16_t pitch, uint8_t ch, uint8_t din_mirror, uint32_t now);
void pe_release(uint8_t pad, uint32_t now);

/* Peak windows (this task) and pressure (Task 6); call every main loop pass. */
void pe_poll(uint32_t now);

/* Note Off for every sounding note; drops pending captures without sending
 * anything; every pad idle (a later pe_release of those pads is silent). */
void pe_release_all(void);

/* Any pad capturing or sounding. */
uint8_t pe_notes_active(void);

#endif
