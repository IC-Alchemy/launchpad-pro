#ifndef PADS_H
#define PADS_H

#include "app_defs.h"

/* Vendor library (cx_pad.o) pad state, declared with the app's u16 (unsigned
 * short on both the firmware and the host simulator) so the app does not need
 * the ST/USB headers. The simulator defines both in tools/sim_midi2.c.
 *
 * ADC_Max: per-pad 10-bit peak ((raw + 2) >> 2), in the library's own slot
 *          order -- use pad_to_adcmax_slot() from pads/pad_velocity.h, NOT
 *          ADC_MAP, to find a pad's slot.
 * PadCalibration: [0] trigger threshold, [1] full scale, [2]/[3] aftertouch
 *          range; set at boot by pad_setcalibration(256, 1023, 512, 769);
 *          app_init then overrides [0] with PAD_LIB_THRESHOLD (20). */
extern u16 ADC_Max[64];
extern u16 PadCalibration[4];

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
