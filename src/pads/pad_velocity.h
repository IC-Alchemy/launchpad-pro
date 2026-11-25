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
 * (set at boot by pad_setcalibration(256, 1023, 512, 769); app_init then
 * overrides the threshold with PAD_LIB_THRESHOLD, 20). */
uint16_t pad_velocity16(uint16_t adc_max, uint16_t threshold, uint16_t full_scale);

/* Same mapping from a raw 12-bit peak (0..4095): start = (threshold + 80) * 4,
 * top = full_scale * 4, so for raw12 = 4 * peak10 the result equals
 * pad_velocity16(peak10, ...), and each raw step is a finer velocity step.
 * Result 1..65535. */
uint16_t pad_velocity16_raw12(uint16_t raw12, uint16_t threshold, uint16_t full_scale);

/* Raw 12-bit adc_buffer index (ADC_MAP order, app_defs.h) for a grid pad
 * index (11..88), or PAD_NO_SLOT. The inverse of ADC_MAP. */
uint8_t pad_raw_index(uint8_t pad);

/* Velocity curve on a 16-bit value; curve is CURVE_* from
 * instrument/settings.h. Result is >= 1. */
uint16_t pad_curve(uint16_t v, uint8_t curve);

/* Pressure range in raw 12-bit ADC counts: full scale and the start point
 * for each threshold setting. */
#define PRESSURE_FULL_RAW  3076
#define PRESSURE_START_LOW  1792
#define PRESSURE_START_MED  2048
#define PRESSURE_START_HIGH 2304

/* Normalise a raw 12-bit reading to 0..4095; threshold is THRESH_*
 * (THRESH_INSTANT assumes the app's PAD_LIB_THRESHOLD trigger calibration;
 * pad_engine uses the live one through pad_pressure_start). */
uint16_t pad_pressure12(uint16_t raw12, uint8_t threshold);

/* Raw 12-bit start of the pressure range for a threshold setting. For
 * THRESH_INSTANT it is where velocity sensing starts, (trigger10 + 80) * 4
 * (pad_velocity16_raw12), capped at PRESSURE_START_LOW, so aftertouch follows
 * the finger from first contact. trigger10 is PadCalibration[0]. */
uint16_t pad_pressure_start(uint8_t threshold, uint16_t trigger10);

/* pad_pressure12 with an explicit raw start (below PRESSURE_FULL_RAW). */
uint16_t pad_pressure12_from(uint16_t raw12, uint16_t start);

/* Replicate 12 bits across 32 for MIDI 2.0: (p<<20)|(p<<8)|(p>>4). */
uint32_t pad_pressure32(uint16_t p12);

#endif
