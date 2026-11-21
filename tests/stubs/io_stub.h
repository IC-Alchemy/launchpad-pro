#ifndef IO_STUB_H
#define IO_STUB_H

#include <stdint.h>

struct io_ev {
	char kind; /* 'N' note on, 'F' note off, 'P' poly, 'C' channel pressure */
	uint8_t port, ch, note;
	uint32_t v;
};

extern struct io_ev io_log[256];
extern int io_n;
extern uint8_t io_midi2[3];
extern uint8_t io_leds[100][3];
extern uint8_t io_setup_led[3];
extern uint8_t io_flash[1024];
extern int io_flash_writes;
extern uint16_t ADC_Max[64];
extern uint16_t PadCalibration[4];
extern uint16_t io_adc[64];

void io_stub_reset(void);

#endif
