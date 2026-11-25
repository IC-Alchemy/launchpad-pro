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

/* Two-stage mode: the engine (pads/twostage.h) decides when a pad starts and
 * stops and how hard; this callback supplies what to play. Called at the
 * engine's Note On for a pad (grid index 11..88): fill *ch (0..15) and
 * *din_mirror and return the pitch, or -1 (or any value outside 0..127) for
 * no note (no Note On, and no Note Off later). No target set (the default
 * after pe_init, or NULL): no notes. */
typedef int16_t (*pe_target_fn)(uint8_t pad, uint8_t *ch, uint8_t *din_mirror);
void pe_set_target(pe_target_fn fn);

/* Intonation: called at each pitch's Note On with its note number. Return 1
 * and fill *pitch79 (MIDI 2.0 Pitch 7.9: semitones * 512) to send that pitch
 * as a Note On attribute to Standalone (DIN gets the plain note), or 0 for a
 * plain Note On. No hook (the default after pe_init, or NULL): plain. */
typedef uint8_t (*pe_pitch_fn)(uint8_t note, uint16_t *pitch79);
void pe_set_pitch(pe_pitch_fn fn);

/* adc_buffer: the raw 12-bit buffer from app_init (ADC_MAP order).
 * Configuration starts at the settings defaults (peak-window, linear,
 * poly pressure, medium threshold). on_sounding may be NULL. */
void pe_init(const uint16_t *adc_buffer, pe_sounding_fn on_sounding);

/* VEL_*, CURVE_*, PRESS_*, THRESH_* from instrument/settings.h. Switching
 * into VEL_TWO_STAGE resets the Two-stage engine (sends nothing): pads down
 * at that moment stay silent until released and pressed again.
 * Precondition: call pe_release_all() before changing the velocity mode while
 * notes are held. Leaving Two-stage stops the engine's Note Offs, and
 * entering it makes pe_release a no-op, so a held note would be stuck. */
void pe_configure(uint8_t velocity_mode, uint8_t curve, uint8_t pressure_mode, uint8_t threshold);

/* Aftertouch smoothing, SMOOTH_* from instrument/settings.h (default
 * SMOOTH_LIGHT after pe_init): the one-pole low-pass on each held pad's raw
 * sample. Takes effect on the next pressure pass; held pads keep their
 * filter state. */
void pe_set_smoothing(uint8_t smoothing);

/* pad: grid index 11..88. pitch -1 (or any value outside 0..127): ignored.
 * ch: 0..15. din_mirror: also send on DINMIDI. now: ms counter.
 * In VEL_TWO_STAGE both pe_press and pe_release are ignored: the engine in
 * pe_poll drives the notes (pe_set_target supplies pitch, channel and DIN
 * flag). */
void pe_press(uint8_t pad, int16_t pitch, uint8_t ch, uint8_t din_mirror, uint32_t now);
void pe_release(uint8_t pad, uint32_t now);

/* Peak windows, the Two-stage engine (VEL_TWO_STAGE: once per new ms, on
 * the raw buffer) and pressure; call every main loop pass. Pressure runs on
 * the first poll with a new now (so once per ms): each HELD pad's raw
 * sample is filtered, then poly pressure (max over the pads sharing a
 * pitch) or channel pressure (max over all held pads) is sent to
 * Standalone and, for pads latched with the DIN flag, DINMIDI. Capturing
 * pads send none. */
void pe_poll(uint32_t now);

/* Note Off for every sounding note; drops pending captures without sending
 * anything; every pad idle (a later pe_release of those pads is silent).
 * Channel pressure goes back to 0 where the last value sent wasn't 0 (as on
 * the last pe_release of a held pad).
 * (Two-stage: the engine restarts, so pads still down stay silent until
 * released and pressed again; same for pe_forget_all.) */
void pe_release_all(void);

/* The USB transport changed (alternate setting or protocol) and has ended
 * every Standalone note itself: forget every note without sending anything
 * on USB. Every pad goes idle (pads still held are ignored until their
 * release), the pitch counts and channel pressure start from 0, and the
 * sounding callback is not called. DIN never sees the change, so each DIN
 * note gets its Note Off (once per pitch) and DIN channel pressure goes back
 * to 0 if it isn't. */
void pe_forget_all(void);

/* Any pad capturing or sounding. */
uint8_t pe_notes_active(void);

#endif
