/******************************************************************************
 
 Copyright (c) 2015, Focusrite Audio Engineering Ltd.
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 * Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 * Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 * Neither the name of Focusrite Audio Engineering Ltd., nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 *****************************************************************************/

//______________________________________________________________________________
//
// Headers
//______________________________________________________________________________

#include <string.h>
#include "app.h"
#include "midi2.h"
#include "ump/ump.h"
#include "platform/pads.h"
#include "platform/irq.h"
#include "pads/twostage.h"
#ifdef PADSCOPE
#include "pads/padscope.h"
#endif

//______________________________________________________________________________
//
// Stage 1 checkpoint app.  Pads send MIDI 2.0 notes with 16-bit velocity on
// the Standalone port, the USB "MIDI" port is bridged to the DIN port, and the
// Setup LED shows the USB MIDI state:
//   green  - MIDI 2.0 active (alt 1, MIDI 2.0 Protocol)
//   amber  - alt 1 but MIDI 1.0 Protocol
//   red    - alt 0 (USB MIDI 1.0)
// Holding Setup pauses UMP receive (RX drain) for the backpressure test.
//
// Velocity modes (hold Setup: pad 11 = Zero-latency, pad 12 = Two-stage):
//   Zero-latency - the library press event (ADC_Max peak) sounds the note
//   Two-stage    - src/pads/twostage.c detects the note from the raw ADC frame
//                  every 1 kHz timer tick; library press events are ignored
// Setup + pad press only selects a mode and never sounds a note.
//______________________________________________________________________________

#define PAD_INDEXES 100

// store ADC frame pointer
static const u16 *g_ADC = 0;

enum { VEL_ZERO_LATENCY, VEL_TWO_STAGE };

// g_vel_mode, g_setup_held, g_held, g_vel and g_ts are shared between the main
// loop (app_surface_event) and the 1 kHz timer interrupt (app_timer_event,
// Two-stage only).  A mode change runs in an irq_save section.
static volatile u8 g_vel_mode = VEL_TWO_STAGE;
static volatile u8 g_setup_held = 0;
static ts_engine g_ts;

#define MODE_PAD_ZERO 11
#define MODE_PAD_TWOSTAGE 12
#define MODE_LED_BRIGHT MAXLED
#define MODE_LED_DIM 8

// 16-bit velocity of each held pad (0 = not held by us)
static u16 g_vel[PAD_INDEXES];
static u8 g_held[PAD_INDEXES];

//______________________________________________________________________________

static void set_pad_led(u8 index, u8 level)
{
    hal_plot_led(TYPEPAD, index, level, level, level);
}

static void update_setup_led(void)
{
    if (midi2_active())
    {
        hal_plot_led(TYPESETUP, 0, 0, MAXLED, 0);   // green
    }
    else if (midi2_usb_alt() == 1)
    {
        hal_plot_led(TYPESETUP, 0, MAXLED, 20, 0);  // amber
    }
    else
    {
        hal_plot_led(TYPESETUP, 0, MAXLED, 0, 0);   // red
    }
}

#ifndef PADSCOPE
// pads 11 and 12 pick the mode while Setup is held: selected one bright
static void show_mode_leds(void)
{
    set_pad_led(MODE_PAD_ZERO, g_vel_mode == VEL_ZERO_LATENCY ? MODE_LED_BRIGHT : MODE_LED_DIM);
    set_pad_led(MODE_PAD_TWOSTAGE, g_vel_mode == VEL_TWO_STAGE ? MODE_LED_BRIGHT : MODE_LED_DIM);
}

// back to the note state (Setup released)
static void restore_pad_led(u8 index)
{
    set_pad_led(index, g_held[index] ? (u8)(g_vel[index] >> 10) : 0);
}
#endif

static void note_off(u8 index)
{
    g_held[index] = 0;
    g_vel[index] = 0;
#ifndef PADSCOPE
    if (g_setup_held && (index == MODE_PAD_ZERO || index == MODE_PAD_TWOSTAGE))
    {
        // keep the mode indicator lit
        show_mode_leds();
    }
    else
#endif
    {
        set_pad_led(index, 0);
    }
    hal_send_note_off2(USBSTANDALONE, 0, index, 0);
}

// engine callback (timer interrupt)
static void on_ts_note(void *ctx, u8 adc_ch, u8 on, u16 velocity)
{
    u8 index = ADC_MAP[adc_ch];
    
    if (on)
    {
        // selecting a mode with Setup + pad must not sound
        if (g_setup_held)
        {
            return;
        }
        g_vel[index] = velocity;
        g_held[index] = 1;
        set_pad_led(index, (u8)(velocity >> 10));
        hal_send_note_on2(USBSTANDALONE, 0, index, velocity);
    }
    else if (g_held[index])
    {
        // an Off for a note we never sounded would be an orphan
        note_off(index);
    }
}

#ifndef PADSCOPE
static void set_vel_mode(u8 mode)
{
    u8 i;
    u32 s;
    
    if (mode == g_vel_mode)
    {
        return;
    }
    
    // the timer interrupt must not run the engine between releasing the old
    // mode's notes and switching
    s = irq_save();
    if (g_vel_mode == VEL_TWO_STAGE)
    {
        ts_release_all(&g_ts, on_ts_note, 0);
    }
    for (i = 0; i < PAD_INDEXES; ++i)
    {
        if (g_held[i])
        {
            note_off(i);
        }
    }
    g_vel_mode = mode;
    if (mode == VEL_TWO_STAGE)
    {
        // fresh engine: a pad already down does not start a note
        ts_init(&g_ts);
    }
    show_mode_leds();
    irq_restore(s);
}
#endif

//______________________________________________________________________________

void app_surface_event(u8 type, u8 index, u8 value)
{
    switch (type)
    {
        case TYPEPAD:
        {
            if (index >= PAD_INDEXES)
            {
                break;
            }
            
            u8 slot = pad_slot(index);
            
            // Two-stage detects grid pad notes itself from the raw frame
            if (g_vel_mode == VEL_TWO_STAGE && slot != PAD_NO_SLOT && !g_setup_held)
            {
                break;
            }
            
#ifndef PADSCOPE
            if (g_setup_held && slot != PAD_NO_SLOT)
            {
                // Setup + pad selects a mode, it never sounds
                if (value)
                {
                    if (index == MODE_PAD_ZERO)
                    {
                        set_vel_mode(VEL_ZERO_LATENCY);
                    }
                    else if (index == MODE_PAD_TWOSTAGE)
                    {
                        set_vel_mode(VEL_TWO_STAGE);
                    }
                }
                else if (g_held[index])
                {
                    // a note sounding before Setup still gets its Off
                    note_off(index);
                }
                break;
            }
#endif
            
            if (value)
            {
                // ADC_Max is indexed by library pad slot and still holds the
                // peak that triggered this press (see platform/pads.h)
                u16 v = 0;
                
                if (slot != PAD_NO_SLOT)
                {
                    v = pad_peak_to_velocity(ADC_Max[slot]);
                }
                
                g_vel[index] = v;
                g_held[index] = 1;
                set_pad_led(index, (u8)(v >> 10));
                hal_send_note_on2(USBSTANDALONE, 0, index, v);
            }
            else if (g_held[index])
            {
                note_off(index);
            }
        }
        break;
            
        case TYPESETUP:
        {
#ifdef PADSCOPE
            // measurement build: Setup press dumps the idle stats
            if (value)
            {
                padscope_dump();
            }
#else
            // held: stop draining received UMP, released: resume
            midi2_debug_pause_rx(value ? 1 : 0);
            
            // held: pads 11/12 pick the velocity mode, released: back to off
            {
                // the timer interrupt plots LEDs too: keep pad 11/12 writes whole
                u32 s = irq_save();
                g_setup_held = value ? 1 : 0;
                if (value)
                {
                    show_mode_leds();
                }
                else
                {
                    restore_pad_led(MODE_PAD_ZERO);
                    restore_pad_led(MODE_PAD_TWOSTAGE);
                }
                irq_restore(s);
            }
#endif
        }
        break;
    }
}

//______________________________________________________________________________

void app_midi_event(u8 port, u8 status, u8 d1, u8 d2)
{
    // MIDI interface functionality for USB "MIDI" port -> DIN port
    if (port == USBMIDI)
    {
        hal_send_midi(DINMIDI, status, d1, d2);
    }
    
    // MIDI interface functionality for DIN -> USB "MIDI" port
    if (port == DINMIDI)
    {
        hal_send_midi(USBMIDI, status, d1, d2);
    }
}

//______________________________________________________________________________

void app_sysex_event(u8 port, u8 * data, u16 count)
{
}

//______________________________________________________________________________

void app_aftertouch_event(u8 index, u8 value)
{
    // poly pressure on the Standalone port, with the pad notes (32-bit and
    // coalesced in MIDI 2.0, 7-bit otherwise); only for pads we sounded
    if (index >= PAD_INDEXES || !g_held[index])
    {
        return;
    }
    hal_send_poly_pressure2(USBSTANDALONE, 0, index, ump_scale_up(value, 7, 32));
}

//______________________________________________________________________________

void app_cable_event(u8 type, u8 value)
{
    // the Setup LED shows the USB MIDI state instead (see app_timer_event)
}

//______________________________________________________________________________

void app_timer_event()
{
    // send MIDI clock at 125bpm
#define TICK_MS 20
#define SETUP_LED_MS 100
    
    static u8 ms = TICK_MS;
    static u8 led_ms = SETUP_LED_MS;
    
    if (++ms >= TICK_MS)
    {
        ms = 0;
        
        // send a clock pulse up the USB
        hal_send_midi(USBSTANDALONE, MIDITIMINGCLOCK, 0, 0);
    }
    
#ifdef PADSCOPE
    // g_ADC is null until app_init has run
    padscope_tick(g_ADC);
#endif
    
    // g_ADC is null until app_init has run
    if (g_vel_mode == VEL_TWO_STAGE && g_ADC)
    {
        ts_tick(&g_ts, g_ADC, on_ts_note, 0);
    }
    
    if (++led_ms >= SETUP_LED_MS)
    {
        led_ms = 0;
        update_setup_led();
    }
}

//______________________________________________________________________________

void app_init(const u16 *adc_raw)
{
    // store off the raw ADC frame pointer for later use
    g_ADC = adc_raw;
    
    // defaults: Two-stage, nothing held
    g_vel_mode = VEL_TWO_STAGE;
    g_setup_held = 0;
    memset(g_held, 0, sizeof g_held);
    memset(g_vel, 0, sizeof g_vel);
    ts_init(&g_ts);

    // lower the library press threshold (main passed 256) so soft hits report
    pad_setcalibration(PAD_LIB_THRESHOLD, 0x3ff, 0x200, 0x301);
    
    update_setup_led();
}
