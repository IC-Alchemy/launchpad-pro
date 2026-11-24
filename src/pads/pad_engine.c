#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "pads/twostage.h"
#include "platform/pads.h"
#include "instrument/io.h"
#include "instrument/settings.h"
#include "app_defs.h"

#include <stddef.h>
#include <string.h>

enum { PE_IDLE, PE_CAPTURING, PE_HELD };

/* pressure destinations: index into the per-destination send times */
enum { PE_DEST_USB, PE_DEST_DIN, PE_DESTS };

/* note value that makes pressure_send() send channel pressure */
#define PE_CHANNEL 0xFF

/* One per grid pad, indexed by ADC_Max slot (pad_to_adcmax_slot). Pitch,
 * channel and the DIN flag are latched at press so the Note Off always
 * matches the Note On, whatever the layout or settings do meanwhile.
 *
 * Poly pressure state (last values sent and when) belongs to a pitch, not a
 * pad. Rather than 128 per-pitch records, every HELD pad keeps a copy of its
 * pitch's state: a pad joining a sounding pitch copies it from a pad that
 * already holds the pitch, and each send updates every holder. So any
 * holder can leave and the others carry on with the right state. */
struct pe_pad
{
    union
    {
        uint32_t press;             /* PE_CAPTURING: ms counter at press */
        uint16_t sent[PE_DESTS];    /* PE_HELD: low 16 bits of the ms counter
                                     * at the pitch's last pressure send */
    } t;
    uint16_t peak;      /* velocity peak, raised while capturing. Peak-window:
                         * raw 12-bit (seeded ADC_Max * 4); Zero-latency:
                         * the library's 10-bit ADC_Max */
    uint16_t filt;      /* PE_HELD: filtered raw 12-bit ADC value x 16 */
    uint16_t sent12;    /* PE_HELD: pitch's last 12-bit pressure on Standalone */
    uint8_t state;      /* PE_IDLE / PE_CAPTURING / PE_HELD */
    uint8_t pitch;      /* latched 0..127 */
    uint8_t ch;         /* latched 0..15 */
    uint8_t din;        /* latched DIN mirror flag */
    uint8_t sent7;      /* PE_HELD: pitch's last 7-bit pressure on DIN */
};

/* compile-time check of the per-pad RAM budget */
typedef char pe_pad_size_check[(sizeof(struct pe_pad) <= 16) ? 1 : -1];

static struct pe_pad g_pad[64];

/* How many pads are sounding each pitch. Keyed by pitch only, not by
 * channel or DIN flag: those change only in the menu (or when settings are
 * applied), and both call pe_release_all() first, so every sounding pad
 * shares one channel and destination set. The latched ch/din are still used
 * for each Note Off. */
static uint8_t g_count[128];

/* HELD pads in all */
static uint8_t g_nheld;

/* Channel pressure state, same meaning as the pad fields. All sounding pads
 * share one channel and destination set (see g_count), so ch is the
 * channel the last value went out on. */
static struct
{
    uint16_t sent[PE_DESTS];
    uint16_t sent12;
    uint8_t sent7;
    uint8_t ch;
} g_chan;

/* the ms counter of the last pressure pass: one pass per new value */
static uint32_t g_tick;
static uint8_t g_ticked;

static const uint16_t *g_adc;
static pe_sounding_fn g_on_sounding;
static uint8_t g_vel_mode;
static uint8_t g_curve;
static uint8_t g_press_mode;
static uint8_t g_threshold;

/* Two-stage: the engine, the main loop's current ms (for its callback) and
 * the provider of pitch/channel/DIN at its Note On */
static ts_engine g_ts;
static uint32_t g_now;
static pe_target_fn g_target;

static uint8_t slot_to_pad(uint8_t slot)
{
    return (uint8_t)((8 - slot / 8) * 10 + 1 + slot % 8);
}

/* wide: peak is a raw 12-bit peak-window value, else a 10-bit ADC_Max */
static uint16_t velocity(uint16_t peak, uint8_t wide)
{
    uint16_t v;

    if (g_vel_mode == VEL_FIXED)
    {
        return 0xFFFF;
    }
    v = wide ? pad_velocity16_raw12(peak, PadCalibration[0], PadCalibration[1])
             : pad_velocity16(peak, PadCalibration[0], PadCalibration[1]);
    return pad_curve(v, g_curve);
}

/* current raw 12-bit ADC sample of a slot's pad */
static uint16_t raw(uint8_t slot)
{
    uint16_t v;

    if (g_adc == NULL)
    {
        return 0;
    }
    v = g_adc[pad_raw_index(slot_to_pad(slot))];
    return v > 4095 ? 4095 : v;
}

/* raise a capturing pad's 12-bit peak with the current raw sample */
static void sample(struct pe_pad *p, uint8_t slot)
{
    uint16_t v = raw(slot);

    if (v > p->peak)
    {
        p->peak = v;
    }
}

/* the pressure state a fresh pitch (or channel) starts from: 0 sent, and
 * the first send may go at once (16-bit ms, wrap-safe) */
static void pressure_start(uint16_t sent[PE_DESTS], uint16_t now16)
{
    sent[PE_DEST_USB] = (uint16_t)(now16 - PRESSURE_MIN_INTERVAL_MS);
    sent[PE_DEST_DIN] = (uint16_t)(now16 - PRESSURE_MIN_INTERVAL_MS);
}

static void pressure_copy(struct pe_pad *dst, const struct pe_pad *src)
{
    dst->t.sent[PE_DEST_USB] = src->t.sent[PE_DEST_USB];
    dst->t.sent[PE_DEST_DIN] = src->t.sent[PE_DEST_DIN];
    dst->sent12 = src->sent12;
    dst->sent7 = src->sent7;
}

/* capture (or zero-latency press) finished: the pad now sounds its pitch.
 * The press time is no longer needed: t now holds pressure send times. */
static void sound_v(struct pe_pad *p, uint16_t vel, uint32_t now)
{
    uint8_t slot = (uint8_t)(p - g_pad);

    p->state = PE_HELD;
    p->filt = (uint16_t)(raw(slot) * 16u);
    if (g_nheld++ == 0)
    {
        /* channel pressure is at 0 here: the last release sent it */
        pressure_start(g_chan.sent, (uint16_t)now);
    }
    if (g_count[p->pitch]++ != 0)
    {
        /* join the pitch: take over its pressure state from a holder */
        uint8_t s;

        for (s = 0; s < 64; s++)
        {
            if (s != slot && g_pad[s].state == PE_HELD && g_pad[s].pitch == p->pitch)
            {
                pressure_copy(p, &g_pad[s]);
                break;
            }
        }
        return;
    }
    pressure_start(p->t.sent, (uint16_t)now);
    p->sent12 = 0;
    p->sent7 = 0;
    io_note_on(USBSTANDALONE, p->ch, p->pitch, vel);
    if (p->din)
    {
        io_note_on(DINMIDI, p->ch, p->pitch, vel);
    }
    if (g_on_sounding != NULL)
    {
        g_on_sounding(p->pitch, 1);
    }
}

/* capture (or zero-latency press) finished: velocity from the peak */
static void sound(struct pe_pad *p, uint32_t now, uint8_t wide)
{
    sound_v(p, velocity(p->peak, wide), now);
}

/* a held pad stops sounding its pitch */
static void silence(struct pe_pad *p)
{
    p->state = PE_IDLE;
    if (g_nheld != 0)
    {
        g_nheld--;
    }
    if (g_count[p->pitch] == 0 || --g_count[p->pitch] != 0)
    {
        return;
    }
    io_note_off(USBSTANDALONE, p->ch, p->pitch);
    if (p->din)
    {
        io_note_off(DINMIDI, p->ch, p->pitch);
    }
    if (g_on_sounding != NULL)
    {
        g_on_sounding(p->pitch, 0);
    }
}

/* Standalone's last value counts as sent-nonzero (for the channel 0) */
static uint8_t usb_nonzero(uint16_t sent12)
{
    return io_port_is_midi2(USBSTANDALONE) ? sent12 != 0 : (sent12 >> 5) != 0;
}

/* channel pressure back to 0 on every destination that last got more */
static void channel_zero(void)
{
    if (usb_nonzero(g_chan.sent12))
    {
        io_channel_pressure(USBSTANDALONE, g_chan.ch, 0);
    }
    if (g_chan.sent7 != 0)
    {
        io_channel_pressure(DINMIDI, g_chan.ch, 0);
    }
    g_chan.sent12 = 0;
    g_chan.sent7 = 0;
}

/* a HELD pad lets go: its pitch ends with the last holder, and channel
 * pressure goes to 0 at once when it was the last held pad */
static void release_held(struct pe_pad *p)
{
    silence(p);
    if (g_nheld == 0)
    {
        channel_zero();
    }
}

/* Two-stage engine callback: adc_ch is a raw buffer index (ADC_MAP order) */
static void ts_note(void *ctx, u8 adc_ch, u8 on, u16 vel)
{
    uint8_t pad;
    uint8_t slot;
    struct pe_pad *p;

    (void)ctx;
    if (adc_ch >= PAD_COUNT)
    {
        return;
    }
    pad = ADC_MAP[adc_ch];
    slot = pad_to_adcmax_slot(pad);
    if (slot == PAD_NO_SLOT)
    {
        return;
    }
    p = &g_pad[slot];
    if (!on)
    {
        /* only a pad this engine started is HELD: no orphan Off */
        if (p->state == PE_HELD)
        {
            release_held(p);
        }
        return;
    }
    if (p->state != PE_IDLE || g_target == NULL)
    {
        return;
    }
    {
        uint8_t ch = 0;
        uint8_t din = 0;
        int16_t pitch = g_target(pad, &ch, &din);

        if (pitch < 0 || pitch > 127)
        {
            return;
        }
        p->pitch = (uint8_t)pitch;
        p->ch = ch & 0x0F;
        p->din = din ? 1 : 0;
        p->peak = 0;
        sound_v(p, pad_curve(vel, g_curve), g_now);
    }
}

static uint8_t interval_passed(uint16_t sent, uint16_t now16)
{
    /* wrap-safe; a value left unsent for exactly n x 65536 ms can be held
     * back by up to PRESSURE_MIN_INTERVAL_MS, which is harmless */
    return (uint16_t)(now16 - sent) >= PRESSURE_MIN_INTERVAL_MS;
}

/* the MIDI 2.0 rule: moved by PRESSURE_HYST, or just reached an end */
static uint8_t changed12(uint16_t p12, uint16_t last)
{
    uint16_t d = (uint16_t)(p12 > last ? p12 - last : last - p12);

    return d >= PRESSURE_HYST || ((p12 == 0 || p12 == 4095) && p12 != last);
}

/* The 7-bit rule: hysteresis around the last value's step. The value moves
 * only when p12 leaves [last7 x 32 - PRESSURE_HYST7, last7 x 32 + 31 +
 * PRESSURE_HYST7], so a pad resting on a step boundary doesn't toggle;
 * 0 and 4095 always reach 0 and 127. */
static uint8_t changed7(uint16_t p12, uint8_t last7)
{
    uint16_t base = (uint16_t)(last7 << 5);

    if (p12 == 0 || p12 == 4095)
    {
        return (uint8_t)(p12 >> 5) != last7;
    }
    return p12 + PRESSURE_HYST7 < base || p12 > base + 31 + PRESSURE_HYST7;
}

static void pressure_out(uint8_t port, uint8_t ch, uint8_t note, uint16_t p12)
{
    if (note == PE_CHANNEL)
    {
        io_channel_pressure(port, ch, pad_pressure32(p12));
    }
    else
    {
        io_poly_pressure(port, ch, note, pad_pressure32(p12));
    }
}

/* Offer the value p12 for a pitch (or PE_CHANNEL) to each destination;
 * sent/sent12/sent7 are that pitch's (or the channel's) state. Standalone
 * follows the MIDI 2.0 or the 7-bit rule by io_port_is_midi2; DIN is always
 * 7-bit. Returns nonzero if the state changed. */
static uint8_t pressure_send(uint16_t sent[PE_DESTS], uint16_t *sent12, uint8_t *sent7,
                             uint8_t ch, uint8_t note, uint8_t din, uint16_t p12, uint16_t now16)
{
    uint8_t changed = 0;

    if (interval_passed(sent[PE_DEST_USB], now16) &&
        (io_port_is_midi2(USBSTANDALONE) ? changed12(p12, *sent12)
                                         : changed7(p12, (uint8_t)(*sent12 >> 5))))
    {
        pressure_out(USBSTANDALONE, ch, note, p12);
        *sent12 = p12;
        sent[PE_DEST_USB] = now16;
        changed = 1;
    }
    if (din && interval_passed(sent[PE_DEST_DIN], now16) && changed7(p12, *sent7))
    {
        pressure_out(DINMIDI, ch, note, p12);
        *sent7 = (uint8_t)(p12 >> 5);
        sent[PE_DEST_DIN] = now16;
        changed = 1;
    }
    return changed;
}

/* Once per new ms value: filter every HELD pad's raw sample, then send
 * poly (max over the pads sharing a pitch) or channel (max over all)
 * pressure. CAPTURING pads take no part. */
static void pressure_pass(uint32_t now)
{
    uint8_t list[64];   /* HELD slots, ascending */
    uint16_t val[64];   /* their 12-bit pressure */
    uint8_t n = 0;
    uint8_t slot;
    uint8_t k;
    uint8_t j;
    uint16_t now16 = (uint16_t)now;

    for (slot = 0; slot < 64; slot++)
    {
        struct pe_pad *p = &g_pad[slot];
        int32_t d;

        if (p->state != PE_HELD)
        {
            continue;
        }
        d = (int32_t)raw(slot) * 16 - (int32_t)p->filt;
        p->filt = (uint16_t)((int32_t)p->filt + d / 8);
        list[n] = slot;
        val[n] = pad_pressure12((uint16_t)(((uint32_t)p->filt + 8u) >> 4), g_threshold);
        n++;
    }
    if (n == 0 || g_press_mode == PRESS_OFF)
    {
        return;
    }

    if (g_press_mode == PRESS_CHANNEL)
    {
        uint16_t max = 0;
        const struct pe_pad *p = &g_pad[list[0]];

        for (k = 0; k < n; k++)
        {
            if (val[k] > max)
            {
                max = val[k];
            }
        }
        g_chan.ch = p->ch;
        (void)pressure_send(g_chan.sent, &g_chan.sent12, &g_chan.sent7, p->ch, PE_CHANNEL,
                            p->din, max, now16);
        return;
    }

    /* poly: the first holder of each pitch in slot order speaks for it */
    for (k = 0; k < n; k++)
    {
        struct pe_pad *r = &g_pad[list[k]];
        uint16_t max = val[k];
        uint8_t shared = g_count[r->pitch] > 1;

        if (shared)
        {
            for (j = 0; j < k && g_pad[list[j]].pitch != r->pitch; j++)
            {
            }
            if (j < k)
            {
                continue; /* done with its group */
            }
            for (j = (uint8_t)(k + 1); j < n; j++)
            {
                if (g_pad[list[j]].pitch == r->pitch && val[j] > max)
                {
                    max = val[j];
                }
            }
        }
        if (pressure_send(r->t.sent, &r->sent12, &r->sent7, r->ch, r->pitch, r->din, max, now16) &&
            shared)
        {
            for (j = (uint8_t)(k + 1); j < n; j++)
            {
                if (g_pad[list[j]].pitch == r->pitch)
                {
                    pressure_copy(&g_pad[list[j]], r);
                }
            }
        }
    }
}

void pe_init(const uint16_t *adc_buffer, pe_sounding_fn on_sounding)
{
    memset(g_pad, 0, sizeof g_pad);
    memset(g_count, 0, sizeof g_count);
    memset(&g_chan, 0, sizeof g_chan);
    g_nheld = 0;
    g_ticked = 0;
    g_target = NULL;
    ts_init(&g_ts);
    g_adc = adc_buffer;
    g_on_sounding = on_sounding;
    pe_configure(VEL_PEAK_WINDOW, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
}

void pe_configure(uint8_t velocity_mode, uint8_t curve, uint8_t pressure_mode, uint8_t threshold)
{
    if (velocity_mode == VEL_TWO_STAGE && g_vel_mode != VEL_TWO_STAGE)
    {
        ts_init(&g_ts);
    }
    g_vel_mode = velocity_mode;
    g_curve = curve;
    g_press_mode = pressure_mode;
    g_threshold = threshold;
}

void pe_set_target(pe_target_fn fn)
{
    g_target = fn;
}

void pe_press(uint8_t pad, int16_t pitch, uint8_t ch, uint8_t din_mirror, uint32_t now)
{
    uint8_t slot = pad_to_adcmax_slot(pad);
    struct pe_pad *p;

    if (g_vel_mode == VEL_TWO_STAGE || slot == PAD_NO_SLOT || pitch < 0 || pitch > 127)
    {
        return;
    }
    p = &g_pad[slot];
    if (p->state != PE_IDLE)
    {
        /* a press without a release: end the old note first */
        pe_release(pad, now);
    }
    p->pitch = (uint8_t)pitch;
    p->ch = ch & 0x0F;
    p->din = din_mirror ? 1 : 0;
    p->t.press = now;
    /* ADC_Max holds this hit's peak until the press callback returns */
    p->peak = ADC_Max[slot];
    if (g_vel_mode == VEL_PEAK_WINDOW)
    {
        /* the window keeps a raw 12-bit peak: seed with the 10-bit one x4 */
        p->peak = p->peak > 1023 ? 4095 : (uint16_t)(p->peak * 4);
        p->state = PE_CAPTURING;
    }
    else
    {
        sound(p, now, 0);
    }
}

void pe_release(uint8_t pad, uint32_t now)
{
    uint8_t slot = pad_to_adcmax_slot(pad);
    struct pe_pad *p;

    if (g_vel_mode == VEL_TWO_STAGE || slot == PAD_NO_SLOT)
    {
        return;
    }
    p = &g_pad[slot];
    if (p->state == PE_CAPTURING)
    {
        /* released inside the window: finish the capture now */
        sample(p, slot);
        sound(p, now, 1);
    }
    if (p->state == PE_HELD)
    {
        release_held(p);
    }
}

void pe_poll(uint32_t now)
{
    uint8_t slot;

    /* peak windows */
    for (slot = 0; slot < 64; slot++)
    {
        struct pe_pad *p = &g_pad[slot];

        if (p->state != PE_CAPTURING)
        {
            continue;
        }
        sample(p, slot);
        if ((uint32_t)(now - p->t.press) >= PEAK_WINDOW_MS)
        {
            sound(p, now, 1);
        }
    }

    /* pressure: once per millisecond (the first poll with a new now) */
    if (g_ticked && now == g_tick)
    {
        return;
    }
    g_ticked = 1;
    g_tick = now;
    if (g_vel_mode == VEL_TWO_STAGE && g_adc != NULL)
    {
        g_now = now;
        ts_tick(&g_ts, g_adc, now, ts_note, NULL);
    }
    pressure_pass(now);
}

void pe_release_all(void)
{
    uint8_t slot;

    for (slot = 0; slot < 64; slot++)
    {
        struct pe_pad *p = &g_pad[slot];

        if (p->state == PE_HELD)
        {
            silence(p);
        }
        /* a pending capture is dropped: no Note On, so no Note Off */
        p->state = PE_IDLE;
    }
    memset(g_count, 0, sizeof g_count);
    g_nheld = 0;
    channel_zero();
    ts_init(&g_ts);
}

void pe_forget_all(void)
{
    uint8_t slot;

    for (slot = 0; slot < 64; slot++)
    {
        struct pe_pad *p = &g_pad[slot];

        /* DIN: the last holder of a pitch ends it, as silence() would */
        if (p->state == PE_HELD && g_count[p->pitch] != 0 && --g_count[p->pitch] == 0 && p->din)
        {
            io_note_off(DINMIDI, p->ch, p->pitch);
        }
        p->state = PE_IDLE;
    }
    memset(g_count, 0, sizeof g_count);
    g_nheld = 0;
    if (g_chan.sent7 != 0)
    {
        io_channel_pressure(DINMIDI, g_chan.ch, 0);
    }
    g_chan.sent12 = 0;
    g_chan.sent7 = 0;
    ts_init(&g_ts);
}

uint8_t pe_notes_active(void)
{
    uint8_t slot;

    for (slot = 0; slot < 64; slot++)
    {
        if (g_pad[slot].state != PE_IDLE)
        {
            return 1;
        }
    }
    return 0;
}
