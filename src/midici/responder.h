#ifndef MIDICI_RESPONDER_H
#define MIDICI_RESPONDER_H

/*
 * MIDI-CI Responder (M2-101 minimum): Discovery, Endpoint Inquiry (Product
 * Instance ID), Invalidate MUID, MUID collision recovery, and NAKs, built on
 * Microsoft's responder in src/midici/vendor. It is the handler for the
 * transport in midici.h and answers only the Standalone port (UMP group 0,
 * Function Block 0).
 */

/* The one MIDI-CI message format we speak; also advertised in Function Block 0. */
#define MIDI_CI_VERSION 0x02

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the responder as the midici handler. Call once at startup. */
void midici_responder_init(void);

/* Drops a pending notification (alt setting change, USB reset). Interrupt-safe. */
void midici_responder_reset(void);

/* Main loop (from ump_rx_poll): retries a Function Block 0 notification that
 * could not be queued after the MUID changed. */
void midici_responder_poll(void);

#ifdef __cplusplus
}
#endif

#endif
