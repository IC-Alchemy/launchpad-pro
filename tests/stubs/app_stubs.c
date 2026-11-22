#include "stubs/app_stubs.h"
#include "midi2.h"
#include "platform/pads.h"
#include <string.h>

void (*stub_irq_hook)(void) = 0;
int stub_irq_depth;

u16 ADC_Max[64];

struct app_ev app_ev_log[APP_EV_MAX];
int app_ev_n;
u8 app_pad_led[100];
int app_pause_rx_calls;
u8 app_pause_rx_last;

static void log_ev(int kind, u8 note, u32 value)
{
	if (app_ev_n < APP_EV_MAX) {
		app_ev_log[app_ev_n].kind = kind;
		app_ev_log[app_ev_n].note = note;
		app_ev_log[app_ev_n].value = value;
	}
	app_ev_n++;
}

void app_stubs_reset(void)
{
	memset(app_ev_log, 0, sizeof app_ev_log);
	app_ev_n = 0;
	memset(app_pad_led, 0, sizeof app_pad_led);
	memset(ADC_Max, 0, sizeof ADC_Max);
	app_pause_rx_calls = 0;
	app_pause_rx_last = 0;
	stub_irq_depth = 0;
}

int app_ev_count(int kind, int note)
{
	int i, n = 0;
	for (i = 0; i < app_ev_n && i < APP_EV_MAX; i++)
		if (app_ev_log[i].kind == kind && (note < 0 || app_ev_log[i].note == note))
			n++;
	return n;
}

void hal_plot_led(u8 type, u8 index, u8 red, u8 green, u8 blue)
{
	(void)green;
	(void)blue;
	if (type == TYPEPAD && index < 100)
		app_pad_led[index] = red;
}

void hal_send_midi(u8 port, u8 status, u8 d1, u8 d2)
{
	(void)port;
	log_ev(EV_MIDI, d1, (u32)status << 8 | d2);
}

void hal_send_note_on2(u8 port, u8 ch, u8 note, u16 velocity)
{
	(void)port;
	(void)ch;
	log_ev(EV_NOTE_ON, note, velocity);
}

void hal_send_note_off2(u8 port, u8 ch, u8 note, u16 velocity)
{
	(void)port;
	(void)ch;
	log_ev(EV_NOTE_OFF, note, velocity);
}

void hal_send_poly_pressure2(u8 port, u8 ch, u8 note, u32 value)
{
	(void)port;
	(void)ch;
	log_ev(EV_POLY, note, value);
}

u8 midi2_active(void) { return 0; }
u8 midi2_usb_alt(void) { return 0; }

void midi2_debug_pause_rx(u8 paused)
{
	app_pause_rx_calls++;
	app_pause_rx_last = paused;
}

void pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi)
{
	(void)threshold;
	(void)c1;
	(void)aftertouch_lo;
	(void)aftertouch_hi;
}
