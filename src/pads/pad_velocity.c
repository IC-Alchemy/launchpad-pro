#include "pads/pad_velocity.h"
#include "instrument/settings.h"
#include "platform/pads.h"

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

uint16_t pad_velocity16_raw12(uint16_t raw12, uint16_t threshold, uint16_t full_scale)
{
    uint32_t lo = ((uint32_t)threshold + PAD_VELOCITY_OFFSET) * 4u;
    uint32_t top = (uint32_t)full_scale * 4u;
    uint32_t v;

    if (raw12 <= lo)
    {
        return 1;
    }
    if (top <= lo || raw12 >= top)
    {
        return 0xFFFF;
    }
    /* (raw12 - lo) < 4096, so the product is below 4096 * 65535 and fits
     * 32 bits; no 64-bit division */
    v = ((uint32_t)(raw12 - lo) * 0xFFFFu) / (top - lo);
    return v == 0 ? 1 : (uint16_t)v;
}

uint8_t pad_raw_index(uint8_t pad)
{
    uint8_t row = pad / 10;  /* 1..8 */
    uint8_t col = pad % 10;  /* 1..8 */

    if (row < 1 || row > 8 || col < 1 || col > 8)
    {
        return PAD_NO_SLOT;
    }
    if (row <= 4)
    {
        return (uint8_t)((row - 1) * 16 + (col - 1) * 2);
    }
    return (uint8_t)((row - 5) * 16 + (col - 1) * 2 + 1);
}

static uint32_t curve_hard(uint32_t v)
{
    return (v * v + v) >> 16;
}

uint16_t pad_curve(uint16_t v, uint8_t curve)
{
    uint32_t r;

    switch (curve)
    {
    case CURVE_HARD:
        r = curve_hard(v);
        break;
    case CURVE_SOFT:
        r = 65535u - curve_hard(65535u - v);
        break;
    default:
        r = v;
        break;
    }
    return r == 0 ? 1 : (uint16_t)r;
}

uint16_t pad_pressure_start(uint8_t threshold, uint16_t trigger10)
{
    uint32_t s;

    switch (threshold)
    {
    case THRESH_LOW:
        return PRESSURE_START_LOW;
    case THRESH_HIGH:
        return PRESSURE_START_HIGH;
    case THRESH_INSTANT:
        s = ((uint32_t)trigger10 + PAD_VELOCITY_OFFSET) * 4u;
        return (uint16_t)(s < PRESSURE_START_LOW ? s : PRESSURE_START_LOW);
    default:
        return PRESSURE_START_MED;
    }
}

uint16_t pad_pressure12(uint16_t raw12, uint8_t threshold)
{
    return pad_pressure12_from(raw12, pad_pressure_start(threshold, PAD_LIB_THRESHOLD));
}

uint16_t pad_pressure12_from(uint16_t raw12, uint16_t start16)
{
    uint32_t start = start16 < PRESSURE_FULL_RAW ? start16 : PRESSURE_FULL_RAW - 1;

    if (raw12 <= start)
    {
        return 0;
    }
    if (raw12 >= PRESSURE_FULL_RAW)
    {
        return 4095;
    }
    /* (raw - start) * 4095 < 3076 * 4095, fits 32 bits */
    return (uint16_t)(((uint32_t)(raw12 - start) * 4095u) / (PRESSURE_FULL_RAW - start));
}

uint32_t pad_pressure32(uint16_t p12)
{
    uint32_t p = p12;

    return (p << 20) | (p << 8) | (p >> 4);
}
