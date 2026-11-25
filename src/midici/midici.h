#ifndef MIDICI_H
#define MIDICI_H

#include <stdint.h>

/*
 * MIDI-CI over USB UMP (SysEx7, MT 0x3): transport plumbing.
 *
 *   USB RX interrupt --midici_rx_push--> CiRxQueue --midici_poll (main loop,
 *   inside ump_rx_poll)--> reassembly --> handler (the Responder, later)
 *   handler --midici_send_sysex--> CiTxQueue --midici_tx_drain (USB TX path)
 *   --> UMP TX control ring --> EP1 IN
 *
 * Nothing here allocates; every buffer is static. All MIDI-CI work runs in the
 * main loop, never in an interrupt.
 */

#define CI_RX_QUEUE_PACKETS 64		/* 2 words each */
#define CI_TX_QUEUE_PACKETS 128
#define CI_REASSEMBLY_BYTES 512
#define CI_REPLY_BYTES 512
/* Max CiTxQueue packets moved to the UMP TX ring per USB TX pass. */
#define CI_TX_DRAIN_MAX 4

/* A whole MIDI-CI message: payload starts at the 0x7E and excludes F0/F7
 * (the form MidiCiMessage.h Parse() takes). Main-loop context. */
typedef void (*midici_handler_fn)(uint8_t group, const uint8_t *payload, uint16_t len);
void midici_set_handler(midici_handler_fn fn);

/* Drops queued packets and reassembly state. Interrupt-safe (SET_INTERFACE,
 * USB reset); the reassembly state is discarded by the next midici_poll. */
void midici_reset(void);

/* Interrupt context: copies one MT 0x3 packet into CiRxQueue. 0, or -1 if the
 * queue was full (counted, never blocks). */
int midici_rx_push(const uint32_t pkt[2]);

/* Free CiRxQueue packets: EP1 OUT is re-armed only with room for a full USB packet. */
uint16_t midici_rx_free_packets(void);

/* Main loop (DoEvents): drains CiRxQueue, reassembles, hands whole MIDI-CI
 * messages to the handler and every other SysEx to app_sysex_event. */
void midici_poll(void);

/* Main loop: queues a whole F0..F7 SysEx7 message (or its payload alone) for
 * the given UMP group, all or nothing. 0, or -1 if CiTxQueue has no room. */
int midici_send_sysex(uint8_t group, const uint8_t *data, uint16_t len);

/* Packets still waiting in CiTxQueue (main loop). */
uint16_t midici_tx_pending(void);
/* Free CiTxQueue packets (main loop): a message of n payload bytes needs (n + 5) / 6. */
uint16_t midici_tx_free_packets(void);

/* USB TX path: moves up to max_packets queued packets into the UMP TX control
 * ring (stopping early if it has no room). Returns the number moved. */
uint8_t midici_tx_drain(uint8_t max_packets);

/* The 512-byte static reply buffer for the Responder. */
uint8_t *midici_reply_buffer(uint16_t *capacity);

/* Valid MUIDs: 28-bit, not 0, not 0x0FFFFF00..0x0FFFFFFF. */
int midici_muid_valid(uint32_t muid);
/* Generated from platform_entropy32() on first use after boot. */
uint32_t midici_muid(void);
/* New MUID (after a collision / invalidation). */
uint32_t midici_muid_regenerate(void);

/* Counters (diagnostics). */
uint32_t midici_rx_overflows(void);
uint32_t midici_tx_overflows(void);
uint32_t midici_messages_received(void);
uint32_t midici_reassembly_drops(void);

#endif
