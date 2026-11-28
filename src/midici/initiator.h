#ifndef MIDICI_INITIATOR_H
#define MIDICI_INITIATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CI_INITIATOR_MAX_PEERS   8u
#define CI_DISCOVERY_WINDOW_MS   3000u
#define CI_PE_GET_TIMEOUT_MS     3000u
#define CI_PEER_PIID_BUF_BYTES   16u

/* Peer progress / status flags (ci_peer_info_t::flags) */
#define CI_PEER_FLAG_DISCOVERED  0x01u  /* DiscoveryReply (0x71) received */
#define CI_PEER_FLAG_ENDPOINT    0x02u  /* EndpointReply (0x73) PIID received */
#define CI_PEER_FLAG_PROFILES    0x04u  /* Final Function Block Profile Inquiry Reply (0x21) received */
#define CI_PEER_FLAG_PE_CAPS     0x08u  /* PE CapabilitiesReply (0x31) received */
#define CI_PEER_FLAG_DEVICE_INFO 0x10u  /* PE Get DeviceInfo (0x35) status 200 */
#define CI_PEER_FLAG_NAK         0x20u  /* Follow-up NAK'd, 4xx, malformed, or timed out */
#define CI_PEER_FLAG_COMPLETE    0x40u  /* All applicable follow-ups settled */

typedef struct ci_peer_info {
	uint32_t muid;
	uint32_t max_sysex;
	uint16_t family;
	uint16_t model;
	uint16_t device_info_status;
	uint16_t profiles_enabled;     /* Total across scopes when CI_PEER_FLAG_PROFILES is set */
	uint16_t profiles_disabled;
	uint8_t manufacturer[3];
	uint8_t sw_rev[4];
	uint8_t version;
	uint8_t categories;
	uint8_t function_block;
	uint8_t pe_simultaneous;
	uint8_t flags;
	char product_instance_id[CI_PEER_PIID_BUF_BYTES];
} ci_peer_info_t;

void midici_initiator_init(void);
void midici_initiator_reset(void); /* ISR-safe: sets reset_requested */
void midici_initiator_tick(uint32_t now_ms);
void midici_initiator_start_discovery(uint32_t now_ms);
void midici_initiator_on_ump_negotiated(uint32_t now_ms);
bool midici_initiator_on_message(uint8_t group, const uint8_t *payload, uint16_t len,
                                 uint32_t now_ms);
void midici_initiator_on_muid_changed(void);
uint8_t midici_initiator_peer_count(void);
bool midici_initiator_get_peer(uint8_t idx, ci_peer_info_t *out);
bool midici_initiator_is_discovering(void);
uint32_t midici_initiator_now_ms(void);

/* True once when payload matches Launchpad's own active 30-byte Discovery broadcast. */
bool midici_initiator_is_own_discovery_echo(const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
