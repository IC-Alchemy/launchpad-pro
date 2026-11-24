#include "test.h"
#include "pads/pad_velocity.h"
#include "instrument/settings.h"
#include "app_defs.h"

/* ADC_Max slot order, from the vendor library's surface_pad_velocitycallback
 * table (c_surface.o .rodata+0x50): slot 0..7 = pads 81..88, slot 8..15 =
 * pads 71..78, ..., slot 56..63 = pads 11..18. */
static void test_pad_to_adcmax_slot(void)
{
    CHECK_EQ(pad_to_adcmax_slot(81), 0);
    CHECK_EQ(pad_to_adcmax_slot(88), 7);
    CHECK_EQ(pad_to_adcmax_slot(71), 8);
    CHECK_EQ(pad_to_adcmax_slot(11), 56);
    CHECK_EQ(pad_to_adcmax_slot(18), 63);
    CHECK_EQ(pad_to_adcmax_slot(45), 36);
    /* not grid pads */
    CHECK_EQ(pad_to_adcmax_slot(10), PAD_NO_SLOT);
    CHECK_EQ(pad_to_adcmax_slot(19), PAD_NO_SLOT);
    CHECK_EQ(pad_to_adcmax_slot(91), PAD_NO_SLOT);
    CHECK_EQ(pad_to_adcmax_slot(1), PAD_NO_SLOT);
    CHECK_EQ(pad_to_adcmax_slot(0), PAD_NO_SLOT);
    CHECK_EQ(pad_to_adcmax_slot(99), PAD_NO_SLOT);
}

/* The library's slot→pad table, copied verbatim from c_surface.o
 * .rodata+0x50 (64 bytes) as used by surface_pad_velocitycallback. */
static const unsigned char LIB_SLOT_TO_PAD[64] = {
    0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58, 0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,
    0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44, 0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,
    0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30, 0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,
    0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c, 0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,
};

static void test_matches_library_table(void)
{
    for (int slot = 0; slot < 64; slot++)
        CHECK_EQ(pad_to_adcmax_slot(LIB_SLOT_TO_PAD[slot]), slot);
}

/* Every grid pad maps to a distinct slot. */
static void test_slots_are_a_permutation(void)
{
    unsigned char seen[64] = {0};
    int n = 0;
    for (int pad = 0; pad < 100; pad++) {
        unsigned char s = pad_to_adcmax_slot((unsigned char)pad);
        if (s == PAD_NO_SLOT) continue;
        CHECK(s < 64);
        CHECK_EQ(seen[s], 0);
        seen[s] = 1;
        n++;
    }
    CHECK_EQ(n, 64);
}

/* ADC_Max is 10-bit ((raw + 2) >> 2). Library calibration at boot:
 * threshold 256, full scale 1023; the library's own velocity starts at
 * ADC_Max - threshold - 80. */
static void test_velocity16_range(void)
{
    CHECK_EQ(pad_velocity16(0, 256, 1023), 1);          /* below trigger → floor 1 */
    CHECK_EQ(pad_velocity16(336, 256, 1023), 1);        /* at trigger point */
    CHECK_EQ(pad_velocity16(1023, 256, 1023), 0xFFFF);  /* full scale */
    CHECK_EQ(pad_velocity16(2000, 256, 1023), 0xFFFF);  /* clamp above */
    /* midpoint of 336..1023 → about half scale */
    unsigned v = pad_velocity16(679, 256, 1023);
    CHECK(v > 0x7E00 && v < 0x8200);
}

static void test_velocity16_monotonic_and_fine(void)
{
    unsigned prev = 0, distinct = 0;
    for (unsigned a = 336; a <= 1023; a++) {
        unsigned v = pad_velocity16((unsigned short)a, 256, 1023);
        CHECK(v >= prev);
        if (v != prev) distinct++;
        prev = v;
    }
    /* every ADC step gives a new velocity: far finer than 7-bit */
    CHECK(distinct > 600);
}

static void test_velocity16_bad_calibration(void)
{
    /* full scale at or below the trigger point must not divide by zero */
    CHECK_EQ(pad_velocity16(500, 256, 300), 0xFFFF);
    CHECK_EQ(pad_velocity16(100, 256, 300), 1);
}

/* 12-bit peak mapping: start (thr+80)*4, top full*4 */
static void test_velocity16_raw12_range(void)
{
    CHECK_EQ(pad_velocity16_raw12(0, 256, 1023), 1);
    CHECK_EQ(pad_velocity16_raw12(336 * 4, 256, 1023), 1);      /* at start */
    CHECK(pad_velocity16_raw12(336 * 4 + 1, 256, 1023) >= 1);   /* just above */
    CHECK_EQ(pad_velocity16_raw12(4092, 256, 1023), 0xFFFF);    /* top */
    CHECK_EQ(pad_velocity16_raw12(4095, 256, 1023), 0xFFFF);    /* clamp */
    CHECK_EQ(pad_velocity16_raw12(4095, 256, 1000), 0xFFFF);    /* above a lower top */
    unsigned v = pad_velocity16_raw12(2716, 256, 1023);
    CHECK(v > 0x7E00 && v < 0x8200);
}

static void test_velocity16_raw12_bad_calibration(void)
{
    CHECK_EQ(pad_velocity16_raw12(2000, 256, 300), 0xFFFF);
    CHECK_EQ(pad_velocity16_raw12(400, 256, 300), 1);
}

static void test_velocity16_raw12_monotonic_and_finer(void)
{
    unsigned prev = 0, distinct = 0;
    for (unsigned a = 0; a <= 4095; a++) {
        unsigned v = pad_velocity16_raw12((unsigned short)a, 256, 1023);
        CHECK(v >= 1);
        CHECK(v >= prev);
        if (v != prev) distinct++;
        prev = v;
    }
    /* about 4x the 10-bit steps (688 in 336..1023) */
    CHECK(distinct > 2600);
    /* two raw values 1 apart: the 10-bit mapping cannot tell them apart */
    CHECK_EQ((2800 + 2) >> 2, (2801 + 2) >> 2); /* same 10-bit bucket: 700 */
    CHECK(pad_velocity16_raw12(2800, 256, 1023) < pad_velocity16_raw12(2801, 256, 1023));
}

/* raw12 = 4 * peak10 agrees with the 10-bit mapping exactly */
static void test_velocity16_raw12_matches_10bit(void)
{
    for (unsigned p = 0; p <= 1023; p++) {
        CHECK_EQ(pad_velocity16_raw12((unsigned short)(p * 4), 256, 1023),
                 pad_velocity16((unsigned short)p, 256, 1023));
        CHECK_EQ(pad_velocity16_raw12((unsigned short)(p * 4), 20, 1023),
                 pad_velocity16((unsigned short)p, 20, 1023));
    }
    /* values between the 4-steps lie between the neighbouring 10-bit results */
    for (unsigned p = 340; p < 1023; p++) {
        unsigned lo = pad_velocity16((unsigned short)p, 20, 1023);
        unsigned hi = pad_velocity16((unsigned short)(p + 1), 20, 1023);
        for (unsigned k = 0; k < 4; k++) {
            unsigned v = pad_velocity16_raw12((unsigned short)(p * 4 + k), 20, 1023);
            CHECK(v >= lo && v <= hi);
        }
    }
}

static void test_raw_index_inverts_adc_map(void)
{
    for (int i = 0; i < 64; i++)
        CHECK_EQ(pad_raw_index(ADC_MAP[i]), i);
    CHECK_EQ(pad_raw_index(10), PAD_NO_SLOT);
    CHECK_EQ(pad_raw_index(99), PAD_NO_SLOT);
}

static void test_curve_endpoints(void)
{
    for (int c = CURVE_SOFT; c <= CURVE_HARD; c++) {
        CHECK(pad_curve(1, (uint8_t)c) >= 1);
        CHECK_EQ(pad_curve(65535, (uint8_t)c), 65535);
    }
}

static void test_curve_shape(void)
{
    CHECK(pad_curve(32768, CURVE_SOFT) > 40000);
    CHECK_EQ(pad_curve(32768, CURVE_LINEAR), 32768);
    CHECK(pad_curve(32768, CURVE_HARD) < 20000);
    for (int c = CURVE_SOFT; c <= CURVE_HARD; c++) {
        unsigned prev = 0;
        for (unsigned v = 0; v <= 65535; v += 64) {
            unsigned r = pad_curve((uint16_t)v, (uint8_t)c);
            CHECK(r >= prev);
            prev = r;
        }
    }
}

static void test_pressure12_thresholds(void)
{
    CHECK_EQ(pad_pressure12(2048, THRESH_MED), 0);
    CHECK_EQ(pad_pressure12(3076, THRESH_MED), 4095);
    CHECK(pad_pressure12(2048, THRESH_LOW) > 0);
    CHECK_EQ(pad_pressure12(2304, THRESH_HIGH), 0);
    for (int t = THRESH_LOW; t <= THRESH_HIGH; t++) {
        unsigned prev = 0;
        for (unsigned r = 0; r <= 4095; r++) {
            unsigned p = pad_pressure12((uint16_t)r, (uint8_t)t);
            CHECK(p >= prev);
            CHECK(p <= 4095);
            prev = p;
        }
    }
}

static void test_pressure32_endpoints(void)
{
    CHECK_EQ(pad_pressure32(0), 0u);
    CHECK_EQ(pad_pressure32(4095), 0xFFFFFFFFu);
    CHECK_EQ(pad_pressure32(2048), 0x80080080u);
}

void test_pad_velocity_all(void)
{
    RUN(test_pad_to_adcmax_slot);
    RUN(test_matches_library_table);
    RUN(test_slots_are_a_permutation);
    RUN(test_velocity16_range);
    RUN(test_velocity16_monotonic_and_fine);
    RUN(test_velocity16_bad_calibration);
    RUN(test_velocity16_raw12_range);
    RUN(test_velocity16_raw12_bad_calibration);
    RUN(test_velocity16_raw12_monotonic_and_finer);
    RUN(test_velocity16_raw12_matches_10bit);
    RUN(test_raw_index_inverts_adc_map);
    RUN(test_curve_endpoints);
    RUN(test_curve_shape);
    RUN(test_pressure12_thresholds);
    RUN(test_pressure32_endpoints);
}
