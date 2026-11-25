#ifndef MIDICI_PE_SESSION_H
#define MIDICI_PE_SESSION_H

/*
 * Property Exchange session (C++, used by responder.cpp only): the Get, Set
 * and Subscription transactions, chunking, the one Set reassembly buffer,
 * subscriptions, and the notifies a settings change sends.
 *
 * One request at a time (PE Capabilities says 1). Every outgoing PE message
 * is built a chunk at a time by poll() straight into the midici reply buffer,
 * as CiTxQueue has room, so a reply longer than the queue still goes out
 * whole. A new request replaces a reply still being sent.
 */

#include <stddef.h>
#include <stdint.h>
#include "vendor/MidiCiMessage.h"

extern "C" {
#include "instrument/settings.h"
}

/* Longest PE message we send, F0 and F7 included (design spec section 8). */
#define CI_PE_MESSAGE_MAX 320

namespace lp_pe {

using SettingsGet = const struct settings *(*)(void);
using SettingsApply = void (*)(const struct settings *);

/* Where the instrument's settings come from and how a Set is applied. Without
 * them the settings resources answer 404. */
void bind(SettingsGet get, SettingsApply apply);

/* Any context (USB reset, alt change): drop every subscription, the reply in
 * progress and a partial Set. Takes effect at the next handle() or poll(). */
void request_reset();

/* Main loop. A Discovery from this initiator declared its receivable SysEx size. */
void note_initiator(uint32_t muid, uint32_t max_sysex);
/* Main loop. That initiator's MUID was invalidated: forget its subscriptions and requests. */
void forget_initiator(uint32_t muid);

/* Main loop. A PE message (0x34..0x3F) addressed to our MUID on the Function
 * Block, already parsed (message.HasPropertyExchangeFields). payload is the
 * whole message from the 0x7E. */
void handle(const WindowsMidiServicesCapabilityInquiry::ParsedMessage &message, const uint8_t *payload,
            size_t len, uint32_t my_muid, const char *piid);

/* Main loop: notices settings changes for subscribers and sends whatever
 * chunks CiTxQueue has room for. */
void poll(uint32_t my_muid, const char *piid);

/* Test hooks. */
bool busy();
uint8_t subscriptions();	/* CI_PE_BIT mask of resources with a subscriber */

} // namespace lp_pe

#endif
