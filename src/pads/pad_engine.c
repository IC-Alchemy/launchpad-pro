#include "pads/pad_engine.h"
#include "pads/pad_velocity.h"
#include "platform/pads.h"
#include "instrument/io.h"
#include "instrument/settings.h"
#include "app_defs.h"

#include <stddef.h>
#include <string.h>

enum { PE_IDLE, PE_CAPTURING, PE_HELD };

/* One per grid pad, indexed by ADC_Max slot (pad_to_adcmax_slot). Pitch,
 * channel and the DIN flag are latched at press so the Note Off always
 * matches the Note On, whatever the layout or settings do meanwhile. */
struct pe_pad
{
    uint32_t t_press;   /* ms counter at press */
    uint16_t peak;      /* 10-bit velocity peak, raised while capturing */
    uint8_t state;      /* PE_IDLE / PE_CAPTURING / PE_HELD */
    uint8_t pitch;      /* latched 0..127 */
    uint8_t ch;         /* latched 0..15 */
    uint8_t din;        /* latched DIN mirror flag */
    /* Task 6 adds the pressure fields here (a 16-bit filter value and a
     * little bookkeeping); the struct must stay <= 16 bytes. */
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

static const uint16_t *g_adc;
static pe_sounding_fn g_on_sounding;
static uint8_t g_vel_mode;
static uint8_t g_curve;
static uint8_t g_press_mode;
static uint8_t g_threshold;

static uint8_t slot_to_pad(uint8_t slot)
{
    return (uint8_t)((8 - slot / 8) * 10 + 1 + slot % 8);
}

static uint16_t velocity(uint16_t peak10)
{
    if (g_vel_mode == VEL_FIXED)
    {
        return 0xFFFF;
    }
    return pad_curve(pad_velocity16(peak10, PadCalibration[0], PadCalibration[1]), g_curve);
}

/* raise a capturing pad's peak with the current raw sample */
static void sample(struct pe_pad *p, uint8_t slot)
{
    uint16_t v;

    if (g_adc == NULL)
    {
        return;
    }
    v = (uint16_t)((g_adc[pad_raw_index(slot_to_pad(slot))] + 2) >> 2);
    if (v > p->peak)
    {
        p->peak = v;
    }
}

/* capture (or zero-latency press) finished: the pad now sounds its pitch */
static void sound(struct pe_pad *p)
{
    p->state = PE_HELD;
    if (g_count[p->pitch]++ != 0)
    {
        return;
    }
    {
        uint16_t vel = velocity(p->peak);

        io_note_on(USBSTANDALONE, p->ch, p->pitch, vel);
        if (p->din)
        {
            io_note_on(DINMIDI, p->ch, p->pitch, vel);
        }
    }
    if (g_on_sounding != NULL)
    {
        g_on_sounding(p->pitch, 1);
    }
}

/* a held pad stops sounding its pitch */
static void silence(struct pe_pad *p)
{
    p->state = PE_IDLE;
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

void pe_init(const uint16_t *adc_buffer, pe_sounding_fn on_sounding)
{
    memset(g_pad, 0, sizeof g_pad);
    memset(g_count, 0, sizeof g_count);
    g_adc = adc_buffer;
    g_on_sounding = on_sounding;
    pe_configure(VEL_PEAK_WINDOW, CURVE_LINEAR, PRESS_POLY, THRESH_MED);
}

void pe_configure(uint8_t velocity_mode, uint8_t curve, uint8_t pressure_mode, uint8_t threshold)
{
    g_vel_mode = velocity_mode;
    g_curve = curve;
    g_press_mode = pressure_mode;
    g_threshold = threshold;
}

void pe_press(uint8_t pad, int16_t pitch, uint8_t ch, uint8_t din_mirror, uint32_t now)
{
    uint8_t slot = pad_to_adcmax_slot(pad);
    struct pe_pad *p;

    if (slot == PAD_NO_SLOT || pitch < 0 || pitch > 127)
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
    p->t_press = now;
    /* ADC_Max holds this hit's peak until the press callback returns */
    p->peak = ADC_Max[slot];
    if (g_vel_mode == VEL_PEAK_WINDOW)
    {
        p->state = PE_CAPTURING;
    }
    else
    {
        sound(p);
    }
}

void pe_release(uint8_t pad, uint32_t now)
{
    uint8_t slot = pad_to_adcmax_slot(pad);
    struct pe_pad *p;

    (void)now;
    if (slot == PAD_NO_SLOT)
    {
        return;
    }
    p = &g_pad[slot];
    if (p->state == PE_CAPTURING)
    {
        /* released inside the window: finish the capture now */
        sample(p, slot);
        sound(p);
    }
    if (p->state == PE_HELD)
    {
        silence(p);
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
        if ((uint32_t)(now - p->t_press) >= PEAK_WINDOW_MS)
        {
            sound(p);
        }
    }

    /* Task 6: pressure pass over PE_HELD pads (g_press_mode, g_threshold) */
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
