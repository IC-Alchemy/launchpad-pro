#include "test.h"
#include "pads/pad_velocity.h"

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

void test_pad_velocity_all(void)
{
    RUN(test_pad_to_adcmax_slot);
    RUN(test_matches_library_table);
    RUN(test_slots_are_a_permutation);
    RUN(test_velocity16_range);
    RUN(test_velocity16_monotonic_and_fine);
    RUN(test_velocity16_bad_calibration);
}
