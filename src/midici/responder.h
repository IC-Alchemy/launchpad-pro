#ifndef MIDICI_RESPONDER_H
#define MIDICI_RESPONDER_H

/*
 * MIDI-CI Responder: Discovery, Endpoint Inquiry (Product Instance ID),
 * Invalidate MUID, MUID collision recovery, NAKs and Property Exchange (Get,
 * Set, Subscribe; see pe_session.h and ci_pe.h), built on Microsoft's
 * responder in src/midici/vendor. It is the handler for the transport in
 * midici.h and answers only the Standalone port (UMP group 0, Function
 * Block 0).
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
 * could not be queued after the MUID changed, sends the rest of a chunked
 * Property Exchange reply, and notifies PE subscribers of settings changes. */
void midici_responder_poll(void);

/* The instrument's settings for Property Exchange: get returns the current
 * ones, apply makes a Set take effect exactly like the device menu (release
 * notes, redraw, schedule the flash save). Unbound, the settings resources
 * answer 404. Call once at startup. */
struct settings;
typedef const struct settings *(*midici_settings_get_fn)(void);
typedef void (*midici_settings_apply_fn)(const struct settings *s);
void midici_pe_bind(midici_settings_get_fn get, midici_settings_apply_fn apply);

#ifdef __cplusplus
}
#endif

#endif
