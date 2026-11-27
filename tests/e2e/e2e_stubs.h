#ifndef E2E_STUBS_H
#define E2E_STUBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define E2E_TX_FIFO_WORDS 2048u

/* HAL / Hardware observables */
extern uint8_t  e2e_leds[100][3];
extern uint8_t  e2e_setup_led[3];
extern uint8_t  e2e_flash[1024];
extern int      e2e_flash_writes;
extern uint16_t e2e_adc[64];

/* Optional legacy/DINMIDI capture from __real_hal_send_{midi,sysex} */
struct e2e_midi_ev {
	uint8_t port;
	uint8_t status;
	uint8_t d1;
	uint8_t d2;
};
extern struct e2e_midi_ev e2e_real_midi_log[64];
extern int e2e_real_midi_n;
extern int e2e_real_sysex_calls;
extern uint8_t e2e_real_sysex_port;
extern uint8_t e2e_real_sysex_buf[512];
extern uint16_t e2e_real_sysex_len;

/* Reset all USB, FIFO, HAL, LED, flash (0xFF), ADC, serial, entropy, and TX FIFO state */
void e2e_stubs_reset(void);

/* Platform configuration */
void e2e_set_serial(const char *serial); /* NULL restores default "TEST0001"; max 24 chars */
void e2e_set_entropy(const uint32_t *seq, int count); /* max 64 values; NULL/0 clears */

/* Host -> Device USB EP1 OUT injection (chunks into <=16-word LE 64B packets -> EP1_OUT_Callback;
 * before each chunk, if alt==1 && (ump_rx_free_words() < 16 || midici_rx_free_packets() < 8),
 * pumps ump_rx_poll() and drains CiTxQueue via __wrap_usbmidi_poll() when needed so bursts
 * do not overflow CiRxQueue or CiTxQueue). */
void e2e_host_send_ump(const uint32_t *words, uint16_t nwords);

/* Raw single-packet EP1 OUT injection (no auto-drain, clamps to 64B, allows odd lengths) */
void e2e_host_send_ep1_raw(const uint8_t *bytes, uint16_t len);

/* USB device/endpoint status helpers */
void e2e_set_device_state(uint32_t state);
uint8_t e2e_ep1_rx_is_valid(void);
uint8_t e2e_ep1_tx_is_valid(void);
void e2e_ep1_tx_ack(void);
void e2e_host_consume_in(void);

/* Device -> Host TX UMP FIFO (decoded from 32-bit LE bytes in USB_SIL_Write(0x81, ...)) */
uint16_t e2e_tx_count(void);
uint16_t e2e_tx_pop_ump(uint32_t *out_words, uint16_t max_words);
uint32_t e2e_tx_overflows(void);

/* Firmware wrap declarations for C/C++ harness convenience */
void __wrap_midi_parsers_init(void);
void __wrap_midi_parsers_poll(void);
void __wrap_usbmidi_poll(void);
void __wrap_midi_flush_windows_buffer(void);
void __wrap_hal_send_midi(uint8_t port, uint8_t status, uint8_t d1, uint8_t d2);
void __wrap_hal_send_sysex(uint8_t port, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
