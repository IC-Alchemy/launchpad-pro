#ifndef PAD_ENGINE_H
#define PAD_ENGINE_H

#include <stdint.h>

/* Pad engine: turns grid pad presses and releases into notes on the
 * instrument's output seam (instrument/io.h). It owns per-pad state, the
 * three velocity modes, note sharing between pads of the same pitch and
 * pressure. Runs only from the main loop: not reentrant. */

#define PEAK_WINDOW_MS 3        /* >= 2 ms after the press at 1 ms tick resolution */

/* Pressure output: a MIDI 2.0 destination gets a new value when it moved by
 * at least PRESSURE_HYST (12-bit units) or just reached 0 or 4095; a 7-bit
 * destination when the 12-bit value left its last 7-bit step widened by
 * PRESSURE_HYST7 on each side (so it reads as a new step), or just reached 0
 * or 4095. Either way at most one message per pitch (or for channel
 * pressure) per destination every PRESSURE_MIN_INTERVAL_MS. */
#define PRESSURE_HYST 32
#define PRESSURE_HYST7 16
#define PRESSURE_MIN_INTERVAL_MS 5

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

/* Peak windows and pressure; call every main loop pass. Pressure runs on
 * the first poll with a new now (so once per ms): each HELD pad's raw
 * sample is filtered, then poly pressure (max over the pads sharing a
 * pitch) or channel pressure (max over all held pads) is sent to
 * Standalone and, for pads latched with the DIN flag, DINMIDI. Capturing
 * pads send none. */
void pe_poll(uint32_t now);

/* Note Off for every sounding note; drops pending captures without sending
 * anything; every pad idle (a later pe_release of those pads is silent).
 * Channel pressure goes back to 0 where the last value sent wasn't 0 (as on
 * the last pe_release of a held pad). */
void pe_release_all(void);

/* Any pad capturing or sounding. */
uint8_t pe_notes_active(void);

#endif
