#include "test.h"
#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "instrument/settings.h"
#include "stubs/io_stub.h"
#include "app_defs.h"

/* sounding edges reported through pe_sounding_fn */
static uint8_t rec_pitch[64];
static uint8_t rec_on[64];
static int rec_n;

static void rec(uint8_t pitch, uint8_t sounding)
{
    if (rec_n < 64) {
        rec_pitch[rec_n] = pitch;
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

void test_pad_engine_all(void)
{
    RUN(test_zero_latency_note_on_at_press);
    RUN(test_peak_window_waits_and_takes_max);
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
}
