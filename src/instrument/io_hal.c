#include "instrument/io.h"
#include "app.h"
#include "midi2.h"

/* Firmware and simulator implementation of io.h. app.h's u32 is unsigned
 * long, so the fixed-width values are cast at each call. */

void io_note_on(uint8_t port, uint8_t ch, uint8_t note, uint16_t vel)
{
	hal_send_note_on2(port, ch, note, vel);
}

void io_note_on_pitch(uint8_t port, uint8_t ch, uint8_t note, uint16_t vel, uint16_t pitch79)
{
	hal_send_note_on2_pitch(port, ch, note, vel, pitch79);
}

void io_note_off(uint8_t port, uint8_t ch, uint8_t note)
{
	hal_send_note_off2(port, ch, note, 0);
}

void io_poly_pressure(uint8_t port, uint8_t ch, uint8_t note, uint32_t v)
{
	hal_send_poly_pressure2(port, ch, note, (u32)v);
}

void io_channel_pressure(uint8_t port, uint8_t ch, uint32_t v)
{
	hal_send_channel_pressure2(port, ch, (u32)v);
}

uint8_t io_port_is_midi2(uint8_t port)
{
	return (uint8_t)(port == USBSTANDALONE && midi2_active());
}

void io_led(uint8_t type, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	hal_plot_led(type, index, r, g, b);
}

void io_flash_read(uint16_t off, uint8_t *d, uint16_t n)
{
	hal_read_flash((u32)off, d, (u32)n);
}

void io_flash_write(uint16_t off, const uint8_t *d, uint16_t n)
{
	hal_write_flash((u32)off, d, (u32)n);
}
