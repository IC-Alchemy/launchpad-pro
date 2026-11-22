#include "stubs/io_stub.h"
#include "instrument/io.h"
#include "app_defs.h"
#include <string.h>

struct io_ev io_log[256];
int io_n;
uint8_t io_midi2[3];
uint8_t io_leds[100][3];
uint8_t io_setup_led[3];
uint8_t io_flash[1024];
int io_flash_writes;
uint16_t ADC_Max[64];
uint16_t PadCalibration[4] = {256, 1023, 512, 769};
uint16_t io_adc[64];

void io_stub_reset(void)
{
	io_n = 0;
	memset(io_log, 0, sizeof io_log);
	memset(io_midi2, 0, sizeof io_midi2);
	memset(io_leds, 0, sizeof io_leds);
	memset(io_setup_led, 0, sizeof io_setup_led);
	memset(io_flash, 0xFF, sizeof io_flash);
	io_flash_writes = 0;
	memset(ADC_Max, 0, sizeof ADC_Max);
	memset(io_adc, 0, sizeof io_adc);
	PadCalibration[0] = 256;
	PadCalibration[1] = 1023;
	PadCalibration[2] = 512;
	PadCalibration[3] = 769;
}

static void log_ev(char kind, uint8_t port, uint8_t ch, uint8_t note, uint32_t v)
{
	if (io_n >= 256)
		return;
	io_log[io_n].kind = kind;
	io_log[io_n].port = port;
	io_log[io_n].ch = ch;
	io_log[io_n].note = note;
	io_log[io_n].v = v;
	io_n++;
}

void io_note_on(uint8_t port, uint8_t ch, uint8_t note, uint16_t vel)
{
	log_ev('N', port, ch, note, vel);
}

void io_note_off(uint8_t port, uint8_t ch, uint8_t note)
{
	log_ev('F', port, ch, note, 0);
}

void io_poly_pressure(uint8_t port, uint8_t ch, uint8_t note, uint32_t v)
{
	log_ev('P', port, ch, note, v);
}

void io_channel_pressure(uint8_t port, uint8_t ch, uint32_t v)
{
	log_ev('C', port, ch, 0, v);
}

uint8_t io_port_is_midi2(uint8_t port)
{
	return port < 3 ? io_midi2[port] : 0;
}

void io_led(uint8_t type, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *d;

	if (type == TYPESETUP)
		d = io_setup_led;
	else if (type == TYPEPAD && index < 100)
		d = io_leds[index];
	else
		return;
	d[0] = r;
	d[1] = g;
	d[2] = b;
}

void io_flash_read(uint16_t off, uint8_t *d, uint16_t n)
{
	if (off + n <= sizeof io_flash)
		memcpy(d, io_flash + off, n);
}

void io_flash_write(uint16_t off, const uint8_t *d, uint16_t n)
{
	if (off + n <= sizeof io_flash)
		memcpy(io_flash + off, d, n);
	io_flash_writes++;
}
