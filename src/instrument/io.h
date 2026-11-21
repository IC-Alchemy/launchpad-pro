#ifndef IO_H
#define IO_H

#include <stdint.h>

/* Output seam: everything the instrument writes goes through here, so the
 * pad engine and UI are host-testable. Implemented by io_hal.c on the
 * device and by tests/stubs/io_stub.c on the host. */

void io_note_on(uint8_t port, uint8_t ch, uint8_t note, uint16_t vel);
void io_note_off(uint8_t port, uint8_t ch, uint8_t note);
void io_poly_pressure(uint8_t port, uint8_t ch, uint8_t note, uint32_t v);
void io_channel_pressure(uint8_t port, uint8_t ch, uint32_t v);
uint8_t io_port_is_midi2(uint8_t port); /* USBSTANDALONE && midi2_active() */
void io_led(uint8_t type, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void io_flash_read(uint16_t off, uint8_t *d, uint16_t n);
void io_flash_write(uint16_t off, const uint8_t *d, uint16_t n);

#endif
