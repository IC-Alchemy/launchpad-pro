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
 *          range; set at boot by pad_setcalibration(256, 1023, 512, 769). */
extern u16 ADC_Max[64];
extern u16 PadCalibration[4];

#endif
