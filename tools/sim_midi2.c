// Host simulator stand-ins for the USB MIDI 2.0 API (include/midi2.h) and the
// vendor library's ADC_Max.  ump_scale_up comes from src/ump/ump_codec.c.

#include <stdio.h>
#include "app.h"
#include "midi2.h"
#include "platform/pads.h"

u16 ADC_Max[64];

void pad_setcalibration(u16 threshold, u16 c1, u16 aftertouch_lo, u16 aftertouch_hi)
{
	printf("pad_setcalibration(%u, 0x%x, 0x%x, 0x%x)\n", threshold, c1,
	       aftertouch_lo, aftertouch_hi);
}

u8 midi2_active(void)
{
	return 0;
}

u8 midi2_usb_alt(void)
{
	return 0;
}

void hal_send_note_on2(u8 port, u8 ch, u8 note, u16 velocity)
{
	printf("...hal_send_note_on2(%d, %d, %d, 0x%4.4x);\n", port, ch, note, velocity);
}

void hal_send_note_off2(u8 port, u8 ch, u8 note, u16 velocity)
{
	printf("...hal_send_note_off2(%d, %d, %d, 0x%4.4x);\n", port, ch, note, velocity);
}

void hal_send_poly_pressure2(u8 port, u8 ch, u8 note, u32 value)
{
	printf("...hal_send_poly_pressure2(%d, %d, %d, 0x%8.8lx);\n", port, ch, note, (unsigned long)value);
}

void hal_send_channel_pressure2(u8 port, u8 ch, u32 value)
{
	printf("...hal_send_channel_pressure2(%d, %d, 0x%8.8lx);\n", port, ch, (unsigned long)value);
}

void midi2_debug_pause_rx(u8 paused)
{
	printf("...midi2_debug_pause_rx(%d);\n", paused);
}
