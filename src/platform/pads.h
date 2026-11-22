#ifndef PADS_H
#define PADS_H

#include "app_defs.h"
#include "ump/ump.h"

/* Per-pad peak ADC value, kept by the vendor library (cx_pad.o).
 * Declared here with the app's u16 (unsigned short on both the firmware and
 * the host simulator) so the app does not need the ST/USB headers.
 * The simulator defines it in tools/sim_midi2.c.
 *
 * From the archive disassembly:
 * - Indexed by library pad slot, not ADC channel: pad_calculate loops slots
 *   0..63 over ADC_Max[slot] and passes the slot to
 *   surface_pad_velocitycallback, which maps it to the grid index through
 *   c_surface.o .rodata + 0x50 (slot 0 = pad 81 ... slot 63 = pad 18, row by
 *   row from the top). Use pad_slot().
 * - Not 12 bit: adc_runMaxSimple (1 kHz) stores
 *   max(ADC_Max, (ADC_Direct + 2) >> 2), so the range is 0..1024.
 * - pad_calculate (200 Hz) calls the velocity callback, which reaches
 *   app_surface_event synchronously, before it clears ADC_Max[slot], so the
 *   press event can read the peak that triggered it. */
extern u16 ADC_Max[64];

#define PAD_NO_SLOT 0xFF
#define PAD_PEAK_MAX 1023

/* library pad slot (ADC_Max index) of grid pad index 11..88, else PAD_NO_SLOT */
static inline u8 pad_slot(u8 index)
{
	u8 row = index / 10;
	u8 col = index % 10;

	if (row < 1 || row > 8 || col < 1 || col > 8)
		return PAD_NO_SLOT;
	return (u8)((8 - row) * 8 + (col - 1));
}

/* 16-bit MIDI 2.0 velocity from an ADC_Max peak (10 bit, clamped) */
static inline u16 pad_peak_to_velocity(u16 peak)
{
	if (peak > PAD_PEAK_MAX)
		peak = PAD_PEAK_MAX;
	return (u16)ump_scale_up(peak, 10, 16);
}

/* Library press threshold. main passes 256 (idle press at ADC_Max >= 358, 35%
 * of full scale); app_init lowers it so softer presses are reported.
 * pad_setcalibration lives in the vendor archive (cx_pad.o). */
#define PAD_LIB_THRESHOLD 20

void pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi);

/* Smallest ADC_Max m (10 bit) with m - sub >= 80 and
 * 400 * (m - sub - 80) / (t + 320) >= 15, the library's press condition
 * (velocity table is zero below index 15). Integer math only. */
static inline u16 pad_lib_level(u16 t, int sub)
{
	int need = (15 * ((int)t + 320) + 399) / 400;	/* ceil */

	return (u16)(sub + 80 + need);
}

/* ADC_Max needed to start a press from idle */
static inline u16 pad_lib_trigger_level(u16 t)
{
	return pad_lib_level(t, (int)t);
}

/* lowest ADC_Max that still counts as pressed once held: threshold t' =
 * max(t - 10, 4) in the subtraction (signed, t < 10 must not wrap) */
static inline u16 pad_lib_release_level(u16 t)
{
	int tp = (int)t - 10;

	if (tp < 4)
		tp = 4;
	return pad_lib_level(t, tp);
}

#endif
