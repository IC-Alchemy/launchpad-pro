#include "pads/pad_velocity.h"

/* offset the library adds above the trigger threshold before its own
 * velocity starts counting (cx_pad.o pad_calculate: lr - threshold - 80) */
#define PAD_VELOCITY_OFFSET 80u

uint8_t pad_to_adcmax_slot(uint8_t pad)
{
    uint8_t row = pad / 10;  /* 1..8 */
    uint8_t col = pad % 10;  /* 1..8 */

    if (row < 1 || row > 8 || col < 1 || col > 8)
    {
        return PAD_NO_SLOT;
    }
    return (uint8_t)((8 - row) * 8 + (col - 1));
}

uint16_t pad_velocity16(uint16_t adc_max, uint16_t threshold, uint16_t full_scale)
{
    uint32_t lo = (uint32_t)threshold + PAD_VELOCITY_OFFSET;
    uint32_t v;

    if (adc_max <= lo)
    {
        return 1;
    }
    if (full_scale <= lo || adc_max >= full_scale)
    {
        return 0xFFFF;
    }
    /* (adc_max - lo) < 1024, so the product fits 32 bits; no 64-bit division */
    v = ((uint32_t)(adc_max - lo) * 0xFFFFu) / ((uint32_t)full_scale - lo);
    return v == 0 ? 1 : (uint16_t)v;
}
