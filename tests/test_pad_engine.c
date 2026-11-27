#include "test.h"
#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "instrument/settings.h"
#include "stubs/io_stub.h"
#include "app_defs.h"

/* sounding edges reported through pe_sounding_fn */
static uint8_t rec_pitch[64];
static uint8_t rec_ch[64];
static uint8_t rec_on[64];
static int rec_n;

static void rec(uint8_t pitch, uint8_t ch, uint8_t sounding)
{
    if (rec_n < 64) {
        rec_pitch[rec_n] = pitch;
        rec_ch[rec_n] = ch;
        rec_on[rec_n] = sounding;
        rec_n++;
    }
}

static void setup(void)
{
    io_stub_reset();
    rec_n = 0;
    pe_init(io_adc, rec);
}

static void zero_latency(void)
{
    pe_configure(VEL_ZERO_LATENCY, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
}

static void set_adcmax(uint8_t pad, uint16_t v)
{
    ADC_Max[pad_to_adcmax_slot(pad)] = v;
}

static void set_raw(uint8_t pad, uint16_t v)
{
    io_adc[pad_raw_index(pad)] = v;
}

static void test_zero_latency_note_on_at_press(void)
{
    setup();
    zero_latency();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 100);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].port, USBSTANDALONE);
    CHECK_EQ(io_log[0].ch, 0);
    CHECK_EQ(io_log[0].note, 64);
    CHECK(io_log[0].v >= 0x7E00 && io_log[0].v <= 0x8200);
    CHECK_EQ(rec_n, 1);
    CHECK_EQ(rec_pitch[0], 64);
    CHECK_EQ(rec_on[0], 1);
    CHECK_EQ(pe_notes_active(), 1);
}

static void test_peak_window_waits_and_takes_max(void)
{
    uint32_t t = 1000;

    setup();
    set_adcmax(45, 400);
    pe_press(45, 64, 0, 0, t);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(pe_notes_active(), 1);
    set_raw(45, 4000);
    pe_poll(t + 1);
    CHECK_EQ(io_n, 0);
    set_raw(45, 1000); /* the peak is kept, not the latest sample */
    pe_poll(t + 2);
    CHECK_EQ(io_n, 0);
    pe_poll(t + 3);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].note, 64);
    CHECK(io_log[0].v > 0xF000);
    CHECK_EQ(rec_n, 1);
    /* sent once only */
    pe_poll(t + 4);
    pe_poll(t + 50);
    CHECK_EQ(io_n, 1);
}

/* the peak-window velocity for a 12-bit peak, with the default calibration */
static uint16_t pw_vel12(uint16_t raw12)
{
    return pad_curve(pad_velocity16_raw12(raw12, PadCalibration[0], PadCalibration[1]), CURVE_LINEAR);
}

static void test_peak_window_uses_12bit_peak(void)
{
    uint32_t v1, v2;

    /* raw 2000 and 2001 share one 10-bit bucket ((raw+2)>>2 = 500 for both),
     * but differ at 12 bits */
    CHECK_EQ((2000 + 2) >> 2, (2001 + 2) >> 2);
    setup();
    set_adcmax(45, 400);
    pe_press(45, 64, 0, 0, 0);
    set_raw(45, 2000);
    pe_poll(1);
    pe_poll(PEAK_WINDOW_MS);
    CHECK_EQ(io_n, 1);
    v1 = io_log[0].v;
    CHECK_EQ(v1, pw_vel12(2000));

    setup();
    set_adcmax(45, 400);
    pe_press(45, 64, 0, 0, 0);
    set_raw(45, 2001);
    pe_poll(1);
    pe_poll(PEAK_WINDOW_MS);
    CHECK_EQ(io_n, 1);
    v2 = io_log[0].v;
    CHECK_EQ(v2, pw_vel12(2001));
    CHECK(v2 > v1);
}

static void test_peak_window_seeds_from_adcmax_times_4(void)
{
    setup();
    set_adcmax(45, 679);
    set_raw(45, 100); /* never exceeds the seed */
    pe_press(45, 64, 0, 0, 0);
    pe_poll(1);
    pe_poll(PEAK_WINDOW_MS);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].v, pw_vel12(679 * 4));
    CHECK_EQ(io_log[0].v, pad_velocity16(679, PadCalibration[0], PadCalibration[1]));
}

static void test_peak_window_seed_clamps_to_4095(void)
{
    setup();
    set_adcmax(45, 1100); /* above the 10-bit range */
    pe_press(45, 64, 0, 0, 0);
    pe_poll(1);
    pe_poll(PEAK_WINDOW_MS);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].v, 0xFFFF); /* seed 4095 -> full scale, linear curve */
}

static void test_peak_window_release_inside_window_keeps_12bit_peak(void)
{
    setup();
    set_adcmax(45, 400);
    pe_press(45, 64, 0, 0, 10);
    set_raw(45, 2002);
    pe_release(45, 11);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].v, pw_vel12(2002));
    CHECK_EQ(io_log[1].kind, 'F');
}

static void test_zero_latency_stays_10bit(void)
{
    setup();
    zero_latency();
    set_adcmax(45, 679);
    set_raw(45, 4000); /* the raw buffer is ignored at the press */
    pe_press(45, 64, 0, 0, 0);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].v, pad_velocity16(679, PadCalibration[0], PadCalibration[1]));
}

static void test_fast_tap_inside_window(void)
{
    setup();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 10);
    pe_release(45, 11);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[1].kind, 'F');
    CHECK_EQ(io_log[1].note, 64);
    CHECK(io_log[0].v >= 0x7E00 && io_log[0].v <= 0x8200);
    pe_poll(12);
    pe_poll(20);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(pe_notes_active(), 0);
    CHECK_EQ(rec_n, 2);
    CHECK_EQ(rec_on[0], 1);
    CHECK_EQ(rec_on[1], 0);
}

static void test_retrigger_within_window(void)
{
    uint32_t t;

    setup();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 0);
    pe_release(45, 1);
    pe_press(45, 64, 0, 0, 2);
    for (t = 2; t <= 10; t++)
        pe_poll(t);
    CHECK_EQ(io_n, 3);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[1].kind, 'F');
    CHECK_EQ(io_log[2].kind, 'N');
    CHECK_EQ(io_log[2].note, 64);
    CHECK_EQ(pe_notes_active(), 1);
    /* and it is not stuck */
    pe_release(45, 11);
    CHECK_EQ(io_n, 4);
    CHECK_EQ(io_log[3].kind, 'F');
    CHECK_EQ(pe_notes_active(), 0);
}

static void test_fixed_velocity(void)
{
    setup();
    pe_configure(VEL_FIXED, CURVE_HARD, PRESS_POLY, THRESH_MED);
    set_adcmax(45, 400);
    pe_press(45, 64, 0, 0, 0);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].v, 0xFFFF);
    pe_release(45, 5);
    pe_configure(VEL_FIXED, CURVE_SOFT, PRESS_POLY, THRESH_MED);
    set_adcmax(45, 1000);
    pe_press(45, 64, 0, 0, 6);
    CHECK_EQ(io_n, 3);
    CHECK_EQ(io_log[2].v, 0xFFFF);
}

static uint32_t vel_with_curve(uint8_t curve)
{
    setup();
    pe_configure(VEL_ZERO_LATENCY, curve, PRESS_POLY, THRESH_MED);
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 0);
    CHECK_EQ(io_n, 1);
    return io_log[0].v;
}

static void test_curves_applied(void)
{
    uint32_t soft = vel_with_curve(CURVE_SOFT);
    uint32_t lin = vel_with_curve(CURVE_LINEAR);
    uint32_t hard = vel_with_curve(CURVE_HARD);

    CHECK(soft > lin);
    CHECK(lin > hard);
    CHECK_EQ(lin, pad_velocity16(679, 256, 1023));
    CHECK_EQ(hard, pad_curve(pad_velocity16(679, 256, 1023), CURVE_HARD));

    /* the peak-window path uses the curve too */
    setup();
    pe_configure(VEL_PEAK_WINDOW, CURVE_HARD, PRESS_POLY, THRESH_MED);
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 0);
    pe_poll(PEAK_WINDOW_MS);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].v, hard);
}

static void test_duplicate_pitch_shares_note(void)
{
    /* zero-latency */
    setup();
    zero_latency();
    set_adcmax(11, 679);
    set_adcmax(14, 679);
    pe_press(11, 60, 0, 0, 0);
    pe_press(14, 60, 0, 0, 1);
    pe_release(11, 2);
    CHECK_EQ(io_n, 1);
    pe_release(14, 3);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].note, 60);
    CHECK_EQ(io_log[1].kind, 'F');
    CHECK_EQ(io_log[1].note, 60);
    CHECK_EQ(rec_n, 2);
    CHECK_EQ(rec_pitch[0], 60);
    CHECK_EQ(rec_on[0], 1);
    CHECK_EQ(rec_pitch[1], 60);
    CHECK_EQ(rec_on[1], 0);

    /* peak-window: the second pad's capture ends while the first sounds */
    setup();
    set_adcmax(11, 679);
    set_adcmax(14, 679);
    pe_press(11, 60, 0, 0, 0);
    pe_poll(3);
    pe_press(14, 60, 0, 0, 4);
    pe_poll(7);
    pe_release(11, 8);
    CHECK_EQ(io_n, 1);
    pe_release(14, 9);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[1].kind, 'F');
    CHECK_EQ(rec_n, 2);

    /* a fast tap on one pad while the other holds the pitch: no output */
    setup();
    set_adcmax(11, 679);
    set_adcmax(14, 679);
    pe_press(11, 60, 0, 0, 0);
    pe_poll(3);
    pe_press(14, 60, 0, 0, 4);
    pe_release(14, 5);
    CHECK_EQ(io_n, 1);
    pe_release(11, 6);
    CHECK_EQ(io_n, 2);
    CHECK_EQ(io_log[1].kind, 'F');
    CHECK_EQ(pe_notes_active(), 0);
}

static void test_latched_pitch_survives_remap(void)
{
    setup();
    zero_latency();
    set_adcmax(11, 679);
    pe_press(11, 60, 3, 0, 0);
    /* the layout changes while pad 11 is held: other presses use new pitches */
    pe_press(12, 67, 3, 0, 1);
    pe_release(11, 2);
    CHECK_EQ(io_n, 3);
    CHECK_EQ(io_log[2].kind, 'F');
    CHECK_EQ(io_log[2].note, 60);
    CHECK_EQ(io_log[2].ch, 3);
    /* the next press of pad 11 uses its new pitch */
    pe_press(11, 65, 3, 0, 3);
    pe_release(11, 4);
    CHECK_EQ(io_n, 5);
    CHECK_EQ(io_log[3].note, 65);
    CHECK_EQ(io_log[4].kind, 'F');
    CHECK_EQ(io_log[4].note, 65);
}

static void test_din_mirror(void)
{
    setup();
    set_adcmax(45, 679);
    pe_press(45, 64, 5, 1, 0);
    pe_poll(PEAK_WINDOW_MS);
    pe_release(45, 10);
    CHECK_EQ(io_n, 4);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].port, USBSTANDALONE);
    CHECK_EQ(io_log[1].kind, 'N');
    CHECK_EQ(io_log[1].port, DINMIDI);
    CHECK_EQ(io_log[0].v, io_log[1].v);
    CHECK_EQ(io_log[2].kind, 'F');
    CHECK_EQ(io_log[2].port, USBSTANDALONE);
    CHECK_EQ(io_log[3].kind, 'F');
    CHECK_EQ(io_log[3].port, DINMIDI);
    CHECK_EQ(io_log[1].ch, 5);
    CHECK_EQ(io_log[3].ch, 5);
    CHECK_EQ(io_log[3].note, 64);
}

static void test_release_all(void)
{
    setup();
    zero_latency();
    set_adcmax(11, 679);
    set_adcmax(88, 679);
    pe_press(11, 60, 0, 0, 0);
    pe_press(88, 72, 0, 0, 0);
    CHECK_EQ(io_n, 2);
    pe_release_all();
    CHECK_EQ(io_n, 4);
    CHECK_EQ(io_log[2].kind, 'F');
    CHECK_EQ(io_log[3].kind, 'F');
    CHECK(io_log[2].note != io_log[3].note);
    CHECK(io_log[2].note == 60 || io_log[2].note == 72);
    CHECK(io_log[3].note == 60 || io_log[3].note == 72);
    CHECK_EQ(rec_n, 4);
    CHECK_EQ(rec_on[2], 0);
    CHECK_EQ(rec_on[3], 0);
    CHECK_EQ(pe_notes_active(), 0);
    pe_release(11, 5);
    pe_release(88, 5);
    pe_poll(10);
    CHECK_EQ(io_n, 4);
    CHECK_EQ(rec_n, 4);
    /* the pitch counts were reset: a new press sounds again */
    pe_press(11, 60, 0, 0, 20);
    CHECK_EQ(io_n, 5);
    CHECK_EQ(io_log[4].kind, 'N');
}

static void test_release_all_held_and_capturing_same_pitch(void)
{
    uint8_t first, second;
    int k;

    /* one HELD and one CAPTURING pad on the same pitch: exactly one Note
     * Off, whichever slot comes first (pad 11 is slot 56, pad 14 slot 59) */
    for (k = 0; k < 2; k++) {
        first = k ? 11 : 14;
        second = k ? 14 : 11;
        setup();
        set_adcmax(first, 679);
        set_adcmax(second, 679);
        pe_press(first, 60, 0, 1, 0);
        pe_poll(PEAK_WINDOW_MS);
        CHECK_EQ(io_n, 2);
        pe_press(second, 60, 0, 1, 4);
        pe_poll(5);
        pe_release_all();
        CHECK_EQ(io_n, 4);
        CHECK_EQ(io_log[2].kind, 'F');
        CHECK_EQ(io_log[2].port, USBSTANDALONE);
        CHECK_EQ(io_log[3].kind, 'F');
        CHECK_EQ(io_log[3].port, DINMIDI);
        CHECK_EQ(io_log[3].note, 60);
        CHECK_EQ(rec_n, 2);
        CHECK_EQ(pe_notes_active(), 0);
        pe_poll(10);
        pe_release(first, 11);
        pe_release(second, 12);
        pe_poll(20);
        CHECK_EQ(io_n, 4);
        CHECK_EQ(rec_n, 2);
    }
}

static void test_setup_entry_during_capture_sends_nothing(void)
{
    setup();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 1, 0);
    pe_poll(1);
    pe_release_all();
    CHECK_EQ(pe_notes_active(), 0);
    pe_poll(2);
    pe_poll(3);
    pe_poll(10);
    pe_release(45, 11);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(rec_n, 0);
}

static void test_peak_window_across_ms_wrap(void)
{
    setup();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 0xFFFFFFFEu);
    pe_poll(0xFFFFFFFFu);
    CHECK_EQ(io_n, 0);
    pe_poll(0);
    pe_poll(1);
    CHECK_EQ(io_n, 1);
    CHECK_EQ(io_log[0].kind, 'N');
}

static void test_pitch_minus1_ignored(void)
{
    setup();
    zero_latency();
    set_adcmax(45, 679);
    pe_press(45, -1, 0, 1, 0);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(pe_notes_active(), 0);
    pe_poll(10);
    pe_release(45, 11);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(rec_n, 0);
    /* same in peak-window mode */
    setup();
    pe_press(45, -1, 0, 0, 0);
    pe_poll(10);
    pe_release(45, 11);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(pe_notes_active(), 0);
}

static void test_forget_all_sends_nothing(void)
{
    uint32_t t;
    int mark;

    /* two held pads with pressure flowing, one pad capturing */
    setup();
    zero_latency();
    set_adcmax(11, 679);
    set_adcmax(88, 679);
    set_raw(11, 3000);
    set_raw(88, 3000);
    pe_press(11, 60, 0, 0, 0);
    pe_press(88, 72, 0, 0, 0);
    for (t = 1; t <= 10; t++)
        pe_poll(t);
    pe_configure(VEL_PEAK_WINDOW, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 11);
    CHECK(io_n >= 4);
    mark = io_n;
    pe_forget_all();
    CHECK_EQ(io_n, mark);
    CHECK_EQ(rec_n, 2);
    CHECK_EQ(pe_notes_active(), 0);
    /* the pads still held: no pressure, no Note On from the capture, and
     * their release is silent */
    for (t = 12; t <= 40; t++) {
        set_raw(11, (uint16_t)(3000 - 10 * t));
        set_raw(45, 3000);
        pe_poll(t);
    }
    pe_release(11, 41);
    pe_release(88, 41);
    pe_release(45, 41);
    pe_poll(50);
    CHECK_EQ(io_n, mark);
    CHECK_EQ(rec_n, 2);
    /* the pitch counts were reset: a fresh press sounds and ends */
    zero_latency();
    pe_press(11, 60, 0, 0, 60);
    CHECK_EQ(io_n, mark + 1);
    CHECK_EQ(io_log[mark].kind, 'N');
    CHECK_EQ(io_log[mark].note, 60);
    pe_release(11, 70);
    CHECK_EQ(io_n, mark + 2);
    CHECK_EQ(io_log[mark + 1].kind, 'F');
    CHECK_EQ(pe_notes_active(), 0);
}

static void test_forget_all_resets_channel_pressure(void)
{
    uint32_t t;
    int mark;

    /* channel pressure above 0 when the transport changes: no 0 is sent on
     * USB, and the next note starts from 0 (a release with no pressure sent
     * sends no 0 either) */
    setup();
    pe_configure(VEL_ZERO_LATENCY, CURVE_LINEAR, PRESS_CHANNEL, THRESH_MED);
    set_adcmax(11, 679);
    set_raw(11, 3000);
    pe_press(11, 60, 0, 0, 0);
    for (t = 1; t <= 10; t++)
        pe_poll(t);
    CHECK_EQ(io_log[io_n - 1].kind, 'C');
    mark = io_n;
    pe_forget_all();
    pe_release(11, 11);
    CHECK_EQ(io_n, mark);
    set_raw(11, 2000);
    pe_press(11, 60, 0, 0, 20);
    for (t = 21; t <= 30; t++)
        pe_poll(t);
    pe_release(11, 31);
    CHECK_EQ(io_n, mark + 2);
    CHECK_EQ(io_log[mark].kind, 'N');
    CHECK_EQ(io_log[mark + 1].kind, 'F');
}

static void test_forget_all_ends_din_notes(void)
{
    int mark;

    /* DIN never sees the USB transport change: its notes get their Note
     * Off (once per pitch), USB gets nothing */
    setup();
    zero_latency();
    set_adcmax(11, 679);
    set_adcmax(14, 679);
    set_adcmax(88, 679);
    pe_press(11, 60, 3, 1, 0);
    pe_press(14, 60, 3, 1, 0);
    pe_press(88, 72, 3, 0, 0);
    mark = io_n;
    pe_forget_all();
    CHECK_EQ(io_n, mark + 1);
    CHECK_EQ(io_log[mark].kind, 'F');
    CHECK_EQ(io_log[mark].port, DINMIDI);
    CHECK_EQ(io_log[mark].ch, 3);
    CHECK_EQ(io_log[mark].note, 60);
    pe_release(11, 5);
    pe_release(14, 5);
    pe_release(88, 5);
    CHECK_EQ(io_n, mark + 1);

    /* DIN channel pressure above 0 goes back to 0 there, not on USB */
    setup();
    pe_configure(VEL_ZERO_LATENCY, CURVE_LINEAR, PRESS_CHANNEL, THRESH_MED);
    set_adcmax(11, 679);
    set_raw(11, 3000);
    pe_press(11, 60, 3, 1, 0);
    pe_poll(1);
    mark = io_n;
    pe_forget_all();
    CHECK_EQ(io_n, mark + 2);
    CHECK_EQ(io_log[mark].kind, 'F');
    CHECK_EQ(io_log[mark].port, DINMIDI);
    CHECK_EQ(io_log[mark + 1].kind, 'C');
    CHECK_EQ(io_log[mark + 1].port, DINMIDI);
    CHECK_EQ(io_log[mark + 1].ch, 3);
    CHECK_EQ(io_log[mark + 1].v, 0);
}

/* ---- Two-stage ---- */

static int16_t tgt_pitch;
static uint8_t tgt_ch;
static uint8_t tgt_din;
static int tgt_calls;

static int16_t target(uint8_t pad, uint8_t *ch, uint8_t *din)
{
    (void)pad;
    tgt_calls++;
    *ch = tgt_ch;
    *din = tgt_din;
    return tgt_pitch;
}

static uint32_t g_t;

static void two_stage_setup(void)
{
    setup();
    memset(io_adc, 0, sizeof io_adc);
    tgt_pitch = 64;
    tgt_ch = 2;
    tgt_din = 0;
    tgt_calls = 0;
    pe_set_target(target);
    pe_configure(VEL_TWO_STAGE, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
    g_t = 1000;
    pe_poll(g_t); /* primes the engine on an all-zero frame */
}

/* one ms per step, each pad's raw value set before the poll */
static void tick(void)
{
    g_t++;
    pe_poll(g_t);
}

static void ramp(uint8_t pad, uint16_t from, uint16_t to, uint16_t step)
{
    uint16_t v = from;

    while (v < to) {
        set_raw(pad, v);
        tick();
        v = (uint16_t)(v + step);
    }
    set_raw(pad, to);
    tick();
}

static void let_go(uint8_t pad)
{
    int i;

    set_raw(pad, 0);
    for (i = 0; i < 6; i++)
        tick();
}

static int count_ev(char kind)
{
    int i, n = 0;

    for (i = 0; i < io_n; i++)
        if (io_log[i].kind == kind)
            n++;
    return n;
}

static void test_ts_note_on_and_off(void)
{
    two_stage_setup();
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 1);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[0].port, USBSTANDALONE);
    CHECK_EQ(io_log[0].ch, 2);
    CHECK_EQ(io_log[0].note, 64);
    CHECK(io_log[0].v > 0 && io_log[0].v <= 0xFFFF);
    CHECK_EQ(rec_n, 1);
    CHECK_EQ(rec_on[0], 1);
    CHECK_EQ(pe_notes_active(), 1);
    CHECK_EQ(count_ev('F'), 0);
    let_go(45);
    CHECK_EQ(count_ev('N'), 1);
    CHECK_EQ(count_ev('F'), 1);
    CHECK_EQ(io_log[io_n - 1].kind, 'F');
    CHECK_EQ(io_log[io_n - 1].ch, 2);
    CHECK_EQ(io_log[io_n - 1].note, 64);
    CHECK_EQ(pe_notes_active(), 0);
}

static void test_ts_velocity_curve_applied(void)
{
    uint32_t lin;

    two_stage_setup();
    ramp(45, 0, 700, 10);
    lin = io_log[0].v;
    CHECK(lin != 0);
    two_stage_setup();
    pe_configure(VEL_TWO_STAGE, CURVE_SOFT, PRESS_POLY, THRESH_MED);
    ramp(45, 0, 700, 10);
    CHECK_EQ(io_log[0].v, pad_curve((uint16_t)lin, CURVE_SOFT));
    CHECK(io_log[0].v != lin);
}

static void test_ts_faster_is_louder(void)
{
    uint32_t slow, fast;

    two_stage_setup();
    ramp(45, 0, 700, 10);
    slow = io_log[0].v;
    two_stage_setup();
    ramp(45, 0, 700, 120);
    fast = io_log[0].v;
    CHECK(fast > slow);
}

static void test_ts_library_press_ignored(void)
{
    two_stage_setup();
    set_adcmax(45, 900);
    pe_press(45, 64, 0, 0, g_t);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(pe_notes_active(), 0);
    pe_release(45, g_t + 1);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(rec_n, 0);
}

static void test_ts_target_minus1(void)
{
    two_stage_setup();
    tgt_pitch = -1;
    ramp(45, 0, 700, 10);
    CHECK(tgt_calls >= 1);
    CHECK_EQ(io_n, 0);
    CHECK_EQ(pe_notes_active(), 0);
    let_go(45);
    CHECK_EQ(io_n, 0);
}

static void test_ts_no_target_no_notes(void)
{
    two_stage_setup();
    pe_set_target(NULL);
    ramp(45, 0, 700, 10);
    let_go(45);
    CHECK_EQ(io_n, 0);
}

static void test_ts_din_flag(void)
{
    two_stage_setup();
    tgt_din = 1;
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 2);
    CHECK_EQ(io_log[1].port, DINMIDI);
    let_go(45);
    CHECK_EQ(count_ev('F'), 2);
}

static void test_ts_same_pitch_shared(void)
{
    two_stage_setup();
    ramp(45, 0, 700, 10);
    ramp(46, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 1);
    CHECK_EQ(rec_n, 1);
    let_go(45);
    CHECK_EQ(count_ev('F'), 0);
    CHECK_EQ(pe_notes_active(), 1);
    let_go(46);
    CHECK_EQ(count_ev('F'), 1);
    CHECK_EQ(rec_n, 2);
    CHECK_EQ(rec_on[1], 0);
}

static void test_ts_release_all_while_held(void)
{
    int mark;

    two_stage_setup();
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 1);
    pe_release_all();
    CHECK_EQ(count_ev('F'), 1);
    CHECK_EQ(pe_notes_active(), 0);
    /* the pad is still down: silent, and no stray Off when it goes up */
    mark = io_n;
    set_raw(45, 3000);
    tick();
    tick();
    let_go(45);
    CHECK_EQ(io_n, mark);
    /* a new press sounds again */
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 2);
    CHECK_EQ(pe_notes_active(), 1);
}

static void test_ts_forget_all_stays_silent(void)
{
    int mark;

    two_stage_setup();
    ramp(45, 0, 700, 10);
    pe_forget_all();
    CHECK_EQ(pe_notes_active(), 0);
    mark = io_n;
    set_raw(45, 3000);
    tick();
    tick();
    let_go(45);
    CHECK_EQ(io_n, mark);
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 2);
}

static void test_ts_configure_into_two_stage_resets_engine(void)
{
    two_stage_setup();
    pe_configure(VEL_ZERO_LATENCY, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
    set_raw(45, 3000); /* pad is down when the mode changes */
    tick();
    pe_configure(VEL_TWO_STAGE, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
    CHECK_EQ(io_n, 0); /* the switch itself sends nothing */
    tick();
    tick();
    CHECK_EQ(io_n, 0); /* held through the reset: silent */
    let_go(45);
    CHECK_EQ(io_n, 0);
    ramp(45, 0, 700, 10);
    CHECK_EQ(count_ev('N'), 1);
}

static void test_ts_pressure_flows(void)
{
    int i;

    two_stage_setup();
    ramp(45, 0, 700, 10);
    set_raw(45, 3000);
    for (i = 0; i < 40; i++)
        tick();
    CHECK(count_ev('P') >= 1);
    for (i = 0; i < io_n; i++)
        if (io_log[i].kind == 'P') {
            CHECK_EQ(io_log[i].note, 64);
            CHECK_EQ(io_log[i].ch, 2);
        }
}

static void test_ts_other_modes_unaffected_by_engine(void)
{
    setup();
    memset(io_adc, 0, sizeof io_adc);
    pe_set_target(target);
    zero_latency();
    set_adcmax(45, 679);
    pe_press(45, 64, 0, 0, 100);
    CHECK_EQ(io_n, 1);
    set_raw(45, 700);
    pe_poll(101);
    pe_poll(102);
    CHECK_EQ(count_ev('N'), 1);
    pe_release(45, 110);
    CHECK_EQ(count_ev('F'), 1);
}

void test_pad_engine_all(void)
{
    RUN(test_zero_latency_note_on_at_press);
    RUN(test_peak_window_waits_and_takes_max);
    RUN(test_peak_window_uses_12bit_peak);
    RUN(test_peak_window_seeds_from_adcmax_times_4);
    RUN(test_peak_window_seed_clamps_to_4095);
    RUN(test_peak_window_release_inside_window_keeps_12bit_peak);
    RUN(test_zero_latency_stays_10bit);
    RUN(test_fast_tap_inside_window);
    RUN(test_retrigger_within_window);
    RUN(test_fixed_velocity);
    RUN(test_curves_applied);
    RUN(test_duplicate_pitch_shares_note);
    RUN(test_latched_pitch_survives_remap);
    RUN(test_din_mirror);
    RUN(test_release_all);
    RUN(test_release_all_held_and_capturing_same_pitch);
    RUN(test_setup_entry_during_capture_sends_nothing);
    RUN(test_peak_window_across_ms_wrap);
    RUN(test_pitch_minus1_ignored);
    RUN(test_forget_all_sends_nothing);
    RUN(test_forget_all_resets_channel_pressure);
    RUN(test_forget_all_ends_din_notes);
    RUN(test_ts_note_on_and_off);
    RUN(test_ts_velocity_curve_applied);
    RUN(test_ts_faster_is_louder);
    RUN(test_ts_library_press_ignored);
    RUN(test_ts_target_minus1);
    RUN(test_ts_no_target_no_notes);
    RUN(test_ts_din_flag);
    RUN(test_ts_same_pitch_shared);
    RUN(test_ts_release_all_while_held);
    RUN(test_ts_forget_all_stays_silent);
    RUN(test_ts_configure_into_two_stage_resets_engine);
    RUN(test_ts_pressure_flows);
    RUN(test_ts_other_modes_unaffected_by_engine);
}
