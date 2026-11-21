#ifndef PAD_VELOCITY_H
#define PAD_VELOCITY_H

#include <stdint.h>

/* The vendor library keeps a per-pad peak in ADC_Max[64] (cx_pad.o), but NOT
 * in the raw ADC order that ADC_MAP in app_defs.h describes:
 *  - adc_runMaxSimple stores ADC_Max[i] = max((ADC_Direct[PADADC[i]] + 2) >> 2),
 *    i.e. a 10-bit value (0..1023), reordered through PADADC;
 *  - surface_pad_velocitycallback maps slot i to pad (8 - i/8)*10 + 1 + i%8,
 *    so slot 0..7 = pads 81..88 and slot 56..63 = pads 11..18;
 *  - pad_calculate zeroes ADC_Max[i] only after the velocity callback returns,
 *    so the peak is valid inside app_surface_event.
 * Sources: disassembly of cx_pad.o (pad_calculate), c_surface.o
 * (adc_runMaxSimple, surface_pad_velocitycallback) and main.o
 * (pad_setcalibration call) from lib/launchpad_pro.a. */

#define PAD_NO_SLOT 0xFF

/* ADC_Max slot for a grid pad index (11..88), or PAD_NO_SLOT. */
uint8_t pad_to_adcmax_slot(uint8_t pad);

/* 16-bit velocity (1..65535) from a 10-bit ADC_Max peak, using the same
 * starting point as the library's own 7-bit velocity (threshold + 80) and
 * full_scale as the top. threshold/full_scale come from PadCalibration[0]/[1]
 * (set at boot by pad_setcalibration(256, 1023, 512, 769)). */
uint16_t pad_velocity16(uint16_t adc_max, uint16_t threshold, uint16_t full_scale);

#endif
