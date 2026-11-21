#include "test.h"
#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "instrument/settings.h"
#include "stubs/io_stub.h"
#include "app_defs.h"

/* ms time of each io_log entry, filled in by poll() / press() / release() */
static uint32_t ev_t[256];

static void stamp(int from, uint32_t t)
{
    int i;

    for (i = from; i < io_n; i++)
        ev_t[i] = t;
}

static void setup(uint8_t pressure_mode, uint8_t midi2)
{
    io_stub_reset();
    pe_init(io_adc, NULL);
    pe_configure(VEL_ZERO_LATENCY, CURVE_LINEAR, pressure_mode, THRESH_MED);
    io_midi2[USBSTANDALONE] = midi2;
}

static void set_raw(uint8_t pad, uint16_t v)
{
    io_adc[pad_raw_index(pad)] = v;
}

static void press(uint8_t pad, int16_t pitch, uint8_t din, uint32_t t)
{
    int n = io_n;

    ADC_Max[pad_to_adcmax_slot(pad)] = 679;
    pe_press(pad, pitch, 0, din, t);
    stamp(n, t);
}

static void release(uint8_t pad, uint32_t t)
{
    int n = io_n;

    pe_release(pad, t);
    stamp(n, t);
}

static void poll(uint32_t t)
{
    int n = io_n;

    pe_poll(t);
    stamp(n, t);
}

static int count_kind(char kind, int from)
{
    int i, c = 0;

    for (i = from; i < io_n; i++)
        if (io_log[i].kind == kind)
            c++;
    return c;
}

/* index of the last entry of this kind (and port, and note if >= 0), or -1 */
static int last_of(char kind, uint8_t port, int note)
{
    int i;

    for (i = io_n - 1; i >= 0; i--)
        if (io_log[i].kind == kind && io_log[i].port == port &&
            (note < 0 || io_log[i].note == note))
            return i;
    return -1;
}

/* consecutive messages of this kind on this port are >= 5 ms apart */
static int spacing_ok(char kind, uint8_t port)
{
    int i, prev = -1;

    for (i = 0; i < io_n; i++) {
        if (io_log[i].kind != kind || io_log[i].port != port)
            continue;
        if (prev >= 0 && (uint32_t)(ev_t[i] - ev_t[prev]) < PRESSURE_MIN_INTERVAL_MS)
            return 0;
        prev = i;
    }
    return 1;
}

static void test_poly_pressure_follows_raw(void)
{
    uint32_t t, last = 0;
    int i, n = 0;

    setup(PRESS_POLY, 1);
    set_raw(45, 2048);
    press(45, 64, 0, 0);
    for (t = 1; t <= 50; t++) {
        set_raw(45, (uint16_t)(2048 + (1152 * t) / 50));
        poll(t);
    }
    for (t = 51; t <= 150; t++)
        poll(t);
    for (i = 0; i < io_n; i++) {
        if (io_log[i].kind != 'P')
            continue;
        CHECK_EQ(io_log[i].port, USBSTANDALONE);
        CHECK_EQ(io_log[i].note, 64);
        CHECK(io_log[i].v >= last);
        last = io_log[i].v;
        n++;
    }
    CHECK(n >= 5);
    CHECK_EQ(last, 0xFFFFFFFFu);
    CHECK(spacing_ok('P', USBSTANDALONE));
}

static void test_pressure_reaches_ends(void)
{
    uint32_t t;
    int i;

    /* creep up to full scale in steps below PRESSURE_HYST: 4095 is sent */
    setup(PRESS_POLY, 1);
    set_raw(45, 3074);
    press(45, 64, 0, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
    CHECK(io_log[0].v < 0xFFFFFFFFu);
    set_raw(45, 3100);
    for (t = 11; t <= 100; t++)
        poll(t);
    i = last_of('P', USBSTANDALONE, 64);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, 0xFFFFFFFFu);

    /* and down to nothing: 0 is sent (raw 2058 is p12 39, at least
     * PRESSURE_HYST above 0, so it goes out first) */
    setup(PRESS_POLY, 1);
    set_raw(45, 2058);
    press(45, 64, 0, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
    set_raw(45, 2040);
    for (t = 11; t <= 100; t++)
        poll(t);
    i = last_of('P', USBSTANDALONE, 64);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, 0);
}

static void test_pressure_rate_cap(void)
{
    uint32_t t;

    setup(PRESS_POLY, 1);
    set_raw(45, 2048);
    press(45, 64, 0, 0);
    for (t = 1; t <= 20; t++) {
        set_raw(45, (uint16_t)(2048 + 100 * t));
        poll(t);
    }
    CHECK(count_kind('P', 0) >= 1);
    CHECK(count_kind('P', 0) <= 5);
    CHECK(spacing_ok('P', USBSTANDALONE));
}

static void test_pressure_rate_cap_across_ms_wrap(void)
{
    uint32_t t0 = 0xFFFFFFF0u, i;
    int before, last;

    setup(PRESS_POLY, 1);
    set_raw(45, 2048);
    press(45, 64, 0, t0);
    for (i = 1; i <= 40; i++) {
        set_raw(45, (uint16_t)(2048 + 25 * i));
        poll(t0 + i);
    }
    CHECK(spacing_ok('P', USBSTANDALONE));
    /* sends continue after the wrap at i = 16 */
    before = count_kind('P', 0);
    CHECK(before >= 6);
    CHECK(before <= 9);
    last = last_of('P', USBSTANDALONE, 64);
    CHECK(last >= 0);
    if (last >= 0)
        CHECK(ev_t[last] < 0x100u);
}

static void test_pressure_jitter_does_not_flood(void)
{
    uint32_t t;
    int n0;

    setup(PRESS_POLY, 1);
    set_raw(45, 2500);
    press(45, 64, 0, 0);
    for (t = 1; t <= 50; t++)
        poll(t);
    CHECK(count_kind('P', 0) >= 1);
    n0 = io_n;
    for (t = 51; t <= 1050; t++) {
        set_raw(45, (t & 1) ? 2490 : 2510);
        poll(t);
    }
    CHECK(count_kind('P', n0) <= 3);
}

/* Fixed-seed LCG (deterministic): uniform noise in [-amp, amp] */
static uint32_t lcg;

static int noise(int amp)
{
    lcg = lcg * 1103515245u + 12345u;
    return (int)((lcg >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

/* A pad resting at raw `rest` with uniform +-amp raw noise every ms: settle
 * for 200 ms, then count the poly pressure messages per port in the next
 * 1000 ms. */
static void noise_run(uint16_t rest, int amp, uint8_t midi2, int *usb, int *din)
{
    uint32_t t;
    int i;

    setup(PRESS_POLY, midi2);
    lcg = 12345u + rest;
    set_raw(45, rest);
    press(45, 64, 1, 0);
    for (t = 1; t <= 200; t++) {
        set_raw(45, (uint16_t)(rest + noise(amp)));
        poll(t);
    }
    io_n = 0;
    for (t = 201; t <= 1200; t++) {
        set_raw(45, (uint16_t)(rest + noise(amp)));
        poll(t);
    }
    *usb = 0;
    *din = 0;
    for (i = 0; i < io_n; i++) {
        if (io_log[i].kind != 'P')
            continue;
        if (io_log[i].port == USBSTANDALONE)
            (*usb)++;
        else if (io_log[i].port == DINMIDI)
            (*din)++;
    }
}

/* the lowest raw value whose pressure is at least p12 */
static uint16_t raw_for(uint16_t p12)
{
    uint16_t r = PRESSURE_START_MED;

    while (r < PRESSURE_FULL_RAW && pad_pressure12(r, THRESH_MED) < p12)
        r++;
    return r;
}

static void test_pressure_noise_does_not_flood_7bit(void)
{
    /* rest values on a 7-bit step boundary (p12 = k * 32) and in between */
    static const uint16_t p12_rest[] = {
        32 * 10, 32 * 10 + 16, 32 * 40, 32 * 40 + 8, 32 * 64, 32 * 64 + 24,
        32 * 100, 32 * 100 + 16, 32 * 120,
    };
    unsigned k;
    int usb, din;

    for (k = 0; k < sizeof p12_rest / sizeof p12_rest[0]; k++) {
        uint16_t rest = raw_for(p12_rest[k]);

        /* (a) Standalone in MIDI 1.0 Protocol, (b) DIN mirror */
        noise_run(rest, 10, 0, &usb, &din);
        if (usb > 5 || din > 5)
            printf("  7-bit rest raw %u (p12 %u): usb %d din %d\n", rest,
                   pad_pressure12(rest, THRESH_MED), usb, din);
        CHECK(usb <= 5);
        CHECK(din <= 5);
    }
}

static void test_pressure_noise_does_not_flood_midi2(void)
{
    static const uint16_t p12_rest[] = {32 * 10, 32 * 40 + 8, 32 * 64, 32 * 100 + 16, 32 * 120};
    unsigned k;
    int usb, din;

    for (k = 0; k < sizeof p12_rest / sizeof p12_rest[0]; k++) {
        uint16_t rest = raw_for(p12_rest[k]);

        noise_run(rest, 10, 1, &usb, &din);
        if (usb > 5 || din > 5)
            printf("  midi2 rest raw %u (p12 %u): usb %d din %d\n", rest,
                   pad_pressure12(rest, THRESH_MED), usb, din);
        CHECK(usb <= 5);
        CHECK(din <= 5);
    }
}

static void test_seven_bit_dest_sends_on_change_only(void)
{
    uint32_t t;
    int i, prev = -1, n = 0;

    setup(PRESS_POLY, 0);
    set_raw(45, 2048);
    press(45, 64, 0, 0);
    for (t = 1; t <= 200; t++) {
        set_raw(45, (uint16_t)(2048 + (1152 * t) / 200));
        poll(t);
    }
    for (t = 201; t <= 300; t++)
        poll(t);
    for (i = 0; i < io_n; i++) {
        if (io_log[i].kind != 'P')
            continue;
        if (prev >= 0)
            CHECK(io_log[i].v >> 25 != io_log[prev].v >> 25);
        prev = i;
        n++;
    }
    CHECK(n >= 10);
    CHECK(prev >= 0);
    if (prev >= 0)
        CHECK_EQ(io_log[prev].v >> 25, 127);
    CHECK(spacing_ok('P', USBSTANDALONE));

    /* a move of more than PRESSURE_HYST that stays inside the 7-bit
     * hysteresis band is not sent: raw 2081 and 2091 give p12 131 and 171
     * (7-bit 4 and 5); step 4's band is 4 x 32 - 16 .. 4 x 32 + 31 + 16,
     * 112..175 */
    setup(PRESS_POLY, 0);
    CHECK_EQ(pad_pressure12(2081, THRESH_MED), 131);
    CHECK_EQ(pad_pressure12(2091, THRESH_MED), 171);
    set_raw(45, 2081);
    press(45, 64, 1, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 2);
    set_raw(45, 2091);
    for (t = 11; t <= 100; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 2);
    /* the same on DIN while Standalone is MIDI 2.0 and does send it */
    setup(PRESS_POLY, 1);
    set_raw(45, 2081);
    press(45, 64, 1, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    set_raw(45, 2091);
    for (t = 11; t <= 100; t++)
        poll(t);
    i = last_of('P', DINMIDI, 64);
    CHECK(i >= 0);
    CHECK_EQ(count_kind('P', 0), 3);
    CHECK(last_of('P', USBSTANDALONE, 64) > i);
}

/* hold raw at v for 60 ms (the filter settles), from *t on */
static void hold_raw(uint16_t v, uint32_t *t)
{
    uint32_t end = *t + 60;

    set_raw(45, v);
    for (; *t < end; (*t)++)
        poll(*t);
}

/* the last 7-bit poly pressure value sent on port, or -1 */
static int last7(uint8_t port)
{
    int i = last_of('P', port, 64);

    return i < 0 ? -1 : (int)(io_log[i].v >> 25);
}

static void test_seven_bit_hysteresis_and_ends(void)
{
    uint32_t t = 1;
    int n;

    /* Standalone in MIDI 1.0 Protocol and DIN, both 7-bit */
    setup(PRESS_POLY, 0);
    set_raw(45, raw_for(32 * 20 + 4));
    press(45, 64, 1, 0);
    hold_raw(raw_for(32 * 20 + 4), &t);
    CHECK_EQ(last7(USBSTANDALONE), 20);
    CHECK_EQ(last7(DINMIDI), 20);
    n = count_kind('P', 0);
    /* into step 21 but inside step 20's band (upper edge 20 x 32 + 47) */
    hold_raw(raw_for(32 * 21 + 8), &t);
    CHECK_EQ(count_kind('P', 0), n);
    /* past the band: 21 goes out */
    hold_raw(raw_for(32 * 21 + 16), &t);
    CHECK_EQ(last7(USBSTANDALONE), 21);
    CHECK_EQ(last7(DINMIDI), 21);
    n = count_kind('P', 0);
    /* back into step 20 but inside step 21's band (lower edge 21 x 32 - 16) */
    hold_raw(raw_for(32 * 21 - 12), &t);
    CHECK_EQ(count_kind('P', 0), n);
    /* below the band: 20 goes out */
    hold_raw(raw_for(32 * 21 - 24), &t);
    CHECK_EQ(last7(USBSTANDALONE), 20);
    CHECK_EQ(last7(DINMIDI), 20);
    /* the ends are always reached */
    hold_raw(PRESSURE_FULL_RAW, &t);
    CHECK_EQ(last7(USBSTANDALONE), 127);
    CHECK_EQ(last7(DINMIDI), 127);
    hold_raw(PRESSURE_START_MED, &t);
    CHECK_EQ(last7(USBSTANDALONE), 0);
    CHECK_EQ(last7(DINMIDI), 0);
    /* a slow creep to the top: 127 at the end, from inside step 126's band */
    hold_raw(raw_for(32 * 126 + 20), &t);
    CHECK_EQ(last7(DINMIDI), 126);
    hold_raw(raw_for(4080), &t);
    CHECK_EQ(last7(DINMIDI), 127);
    CHECK(spacing_ok('P', USBSTANDALONE));
    CHECK(spacing_ok('P', DINMIDI));
}

static void test_din_pressure_is_seven_bit(void)
{
    uint32_t t;
    int i, prev = -1, n_din = 0, n_usb = 0;

    /* Standalone is MIDI 2.0, DIN is always 7-bit: separate state */
    setup(PRESS_POLY, 1);
    set_raw(45, 2048);
    press(45, 64, 1, 0);
    for (t = 1; t <= 200; t++) {
        set_raw(45, (uint16_t)(2048 + (1152 * t) / 200));
        poll(t);
    }
    for (t = 201; t <= 300; t++)
        poll(t);
    for (i = 0; i < io_n; i++) {
        if (io_log[i].kind != 'P')
            continue;
        if (io_log[i].port == USBSTANDALONE) {
            n_usb++;
            continue;
        }
        CHECK_EQ(io_log[i].port, DINMIDI);
        CHECK_EQ(io_log[i].note, 64);
        if (prev >= 0)
            CHECK(io_log[i].v >> 25 != io_log[prev].v >> 25);
        prev = i;
        n_din++;
    }
    CHECK(n_din >= 10);
    CHECK(n_usb >= 10);
    i = last_of('P', DINMIDI, 64);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v >> 25, 127);
    i = last_of('P', USBSTANDALONE, 64);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, 0xFFFFFFFFu);
    CHECK(spacing_ok('P', USBSTANDALONE));
    CHECK(spacing_ok('P', DINMIDI));
}

static void test_shared_pitch_uses_max(void)
{
    uint32_t t;
    int i;

    setup(PRESS_POLY, 1);
    set_raw(11, 2500);
    set_raw(14, 3000);
    press(11, 60, 0, 0);
    press(14, 60, 0, 0);
    for (t = 1; t <= 30; t++)
        poll(t);
    i = last_of('P', USBSTANDALONE, 60);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, pad_pressure32(pad_pressure12(3000, THRESH_MED)));
    CHECK(spacing_ok('P', USBSTANDALONE));
    /* the remaining pad's value takes over */
    release(14, 31);
    for (t = 32; t <= 60; t++)
        poll(t);
    i = last_of('P', USBSTANDALONE, 60);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, pad_pressure32(pad_pressure12(2500, THRESH_MED)));
    CHECK_EQ(count_kind('F', 0), 0);

    /* the lowest slot (pad 81) leaves the group: the pitch's state stays
     * with the pad still holding it, so an equal value is not sent again */
    setup(PRESS_POLY, 1);
    set_raw(81, 3000);
    set_raw(11, 3000);
    press(81, 62, 0, 0);
    press(11, 62, 0, 0);
    for (t = 1; t <= 30; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
    release(81, 31);
    for (t = 32; t <= 60; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
    /* and a change on the remaining pad is still sent */
    set_raw(11, 2500);
    for (t = 61; t <= 150; t++)
        poll(t);
    CHECK(count_kind('P', 0) >= 2);
    i = last_of('P', USBSTANDALONE, 62);
    CHECK(i >= 0);
    if (i >= 0) {
        uint32_t want = pad_pressure12(2500, THRESH_MED);
        uint32_t got = io_log[i].v >> 20;

        CHECK(got >= want && got < want + PRESSURE_HYST);
    }
    CHECK(spacing_ok('P', USBSTANDALONE));

    /* a pad joining a sounding pitch on a lower slot (81) takes over its
     * state: an equal value is not sent again */
    setup(PRESS_POLY, 1);
    set_raw(11, 3000);
    set_raw(81, 3000);
    press(11, 62, 0, 0);
    for (t = 1; t <= 30; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
    press(81, 62, 0, 31);
    for (t = 32; t <= 60; t++)
        poll(t);
    CHECK_EQ(count_kind('P', 0), 1);
}

static void test_channel_mode_max_and_zero(void)
{
    uint32_t t;
    int i;

    setup(PRESS_CHANNEL, 1);
    set_raw(11, 2500);
    set_raw(12, 3000);
    press(11, 60, 0, 0);
    press(12, 62, 0, 0);
    for (t = 1; t <= 20; t++)
        poll(t);
    CHECK(count_kind('C', 0) >= 1);
    CHECK_EQ(count_kind('P', 0), 0);
    i = last_of('C', USBSTANDALONE, -1);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, pad_pressure32(pad_pressure12(3000, THRESH_MED)));
    release(12, 21);
    for (t = 22; t <= 40; t++)
        poll(t);
    i = last_of('C', USBSTANDALONE, -1);
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(io_log[i].v, pad_pressure32(pad_pressure12(2500, THRESH_MED)));
    /* the last release: a 0 goes out straight away */
    release(11, 41);
    CHECK(io_n > 0);
    CHECK_EQ(io_log[io_n - 1].kind, 'C');
    CHECK_EQ(io_log[io_n - 1].v, 0);
    CHECK_EQ(count_kind('P', 0), 0);
    i = io_n;
    for (t = 42; t <= 60; t++)
        poll(t);
    CHECK_EQ(io_n, i);
    CHECK(spacing_ok('C', USBSTANDALONE));
}

static void test_channel_zero_on_release_all(void)
{
    uint32_t t;
    int n;

    setup(PRESS_CHANNEL, 1);
    set_raw(11, 3000);
    press(11, 60, 1, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    CHECK(last_of('C', USBSTANDALONE, -1) >= 0);
    CHECK(last_of('C', DINMIDI, -1) >= 0);
    pe_release_all();
    CHECK(io_n >= 2);
    CHECK_EQ(io_log[io_n - 2].kind, 'C');
    CHECK_EQ(io_log[io_n - 2].v, 0);
    CHECK_EQ(io_log[io_n - 1].kind, 'C');
    CHECK_EQ(io_log[io_n - 1].v, 0);
    CHECK(io_log[io_n - 1].port != io_log[io_n - 2].port);
    /* nothing more, and no second 0 */
    n = io_n;
    release(11, 11);
    pe_release_all();
    poll(20);
    CHECK_EQ(io_n, n);

    /* no 0 when the last value sent was already 0 */
    setup(PRESS_CHANNEL, 1);
    set_raw(11, 2000);
    press(11, 60, 0, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    release(11, 11);
    CHECK_EQ(count_kind('C', 0), 0);
}

static void test_pressure_off(void)
{
    uint32_t t;

    setup(PRESS_OFF, 1);
    set_raw(11, 2048);
    set_raw(12, 2048);
    press(11, 60, 1, 0);
    press(12, 62, 1, 0);
    for (t = 1; t <= 100; t++) {
        set_raw(11, (uint16_t)(2048 + 10 * t));
        set_raw(12, (uint16_t)(2048 + 10 * t));
        poll(t);
    }
    release(11, 101);
    release(12, 102);
    pe_release_all();
    CHECK_EQ(count_kind('P', 0), 0);
    CHECK_EQ(count_kind('C', 0), 0);
}

static void test_no_pressure_while_capturing(void)
{
    uint32_t t;
    int n;

    setup(PRESS_POLY, 1);
    pe_configure(VEL_PEAK_WINDOW, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
    set_raw(45, 3000);
    press(45, 64, 1, 0);
    for (t = 1; t <= 20; t++)
        poll(t);
    CHECK(io_n >= 3);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK_EQ(io_log[1].kind, 'N');
    CHECK(count_kind('P', 0) >= 2);

    /* pressed again: the old filter value must not leak into the capture */
    release(45, 21);
    n = io_n;
    set_raw(45, 2600);
    press(45, 64, 1, 30);
    for (t = 31; t <= 50; t++)
        poll(t);
    CHECK(io_n >= n + 3);
    CHECK_EQ(io_log[n].kind, 'N');
    CHECK_EQ(io_log[n + 1].kind, 'N');
    CHECK(count_kind('P', n) >= 2);

    /* channel mode too */
    setup(PRESS_CHANNEL, 1);
    pe_configure(VEL_PEAK_WINDOW, CURVE_LINEAR, PRESS_CHANNEL, THRESH_MED);
    set_raw(45, 3000);
    press(45, 64, 0, 0);
    for (t = 1; t <= 20; t++)
        poll(t);
    CHECK(io_n >= 2);
    CHECK_EQ(io_log[0].kind, 'N');
    CHECK(count_kind('C', 0) >= 1);
}

static void test_release_stops_pressure(void)
{
    uint32_t t;
    int f;

    setup(PRESS_POLY, 1);
    set_raw(45, 2500);
    set_raw(46, 2500);
    press(45, 64, 1, 0);
    press(46, 65, 1, 0);
    for (t = 1; t <= 10; t++)
        poll(t);
    CHECK(last_of('P', USBSTANDALONE, 64) >= 0);
    CHECK(last_of('P', DINMIDI, 64) >= 0);
    release(45, 11);
    f = last_of('F', DINMIDI, 64);
    CHECK(f >= 0);
    for (t = 12; t <= 60; t++) {
        set_raw(45, (uint16_t)(2500 + 10 * t));
        set_raw(46, (uint16_t)(2500 + 10 * t));
        poll(t);
    }
    CHECK(last_of('P', USBSTANDALONE, 64) < f);
    CHECK(last_of('P', DINMIDI, 64) < f);
    /* the other pitch keeps going */
    CHECK(last_of('P', USBSTANDALONE, 65) > f);
    CHECK(last_of('P', DINMIDI, 65) > f);
}

void test_pressure_all(void)
{
    RUN(test_poly_pressure_follows_raw);
    RUN(test_pressure_reaches_ends);
    RUN(test_pressure_rate_cap);
    RUN(test_pressure_rate_cap_across_ms_wrap);
    RUN(test_pressure_jitter_does_not_flood);
    RUN(test_pressure_noise_does_not_flood_7bit);
    RUN(test_pressure_noise_does_not_flood_midi2);
    RUN(test_seven_bit_dest_sends_on_change_only);
    RUN(test_seven_bit_hysteresis_and_ends);
    RUN(test_din_pressure_is_seven_bit);
    RUN(test_shared_pitch_uses_max);
    RUN(test_channel_mode_max_and_zero);
    RUN(test_channel_zero_on_release_all);
    RUN(test_pressure_off);
    RUN(test_no_pressure_while_capturing);
    RUN(test_release_stops_pressure);
}
