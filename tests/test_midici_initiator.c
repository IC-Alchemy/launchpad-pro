#include "test.h"
#include "stubs/stubs.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
#include "midici/midici.h"
#include "midici/responder.h"
#include "midici/initiator.h"
#include "midici/ci_identity.h"
#include "ump/ump.h"
#include "midi2.h"
#include <stdint.h>
#include <string.h>

void EP1_OUT_Callback(void);
void __wrap_midi_parsers_init(void);
void __wrap_midi_parsers_poll(void);
void __wrap_usbmidi_poll(void);

static void write_muid(uint8_t *p, uint32_t m)
{
	p[0] = (uint8_t)(m & 0x7Fu);
	p[1] = (uint8_t)((m >> 7) & 0x7Fu);
	p[2] = (uint8_t)((m >> 14) & 0x7Fu);
	p[3] = (uint8_t)((m >> 21) & 0x7Fu);
}

static uint32_t read_muid(const uint8_t *p)
{
	return (uint32_t)(p[0] & 0x7Fu) |
	       ((uint32_t)(p[1] & 0x7Fu) << 7) |
	       ((uint32_t)(p[2] & 0x7Fu) << 14) |
	       ((uint32_t)(p[3] & 0x7Fu) << 21);
}

static void init_alt1(void)
{
	stub_irq_hook = 0;
	stubs_reset();
	__wrap_midi_parsers_init();
	usb_midi2_usb_reset();
	usb_midi2_set_alt(1);
	usb_midi2_service();
	bDeviceState = USB_STATE_CONFIGURED;
	midi2_debug_pause_rx(0);
	ump_stream_set_protocol_hook(midi2_protocol_changing);
	midici_initiator_init();
}

static void host_out_words(const uint32_t *w, uint16_t n)
{
	uint8_t p[64];
	uint16_t i;
	for (i = 0; i < n && i < 16; i++) {
		p[4 * i] = (uint8_t)w[i];
		p[4 * i + 1] = (uint8_t)(w[i] >> 8);
		p[4 * i + 2] = (uint8_t)(w[i] >> 16);
		p[4 * i + 3] = (uint8_t)(w[i] >> 24);
	}
	stub_rx_packet(p, (uint16_t)(4u * i));
	EP1_OUT_Callback();
}

static uint16_t host_in_words(uint32_t out[16])
{
	uint16_t i;
	int writes = stub.tx_writes;
	stub.ep_tx_status[1] = EP_TX_NAK;
	__wrap_usbmidi_poll();
	if (stub.tx_writes == writes)
		return 0;
	for (i = 0; i < stub.last_tx_len / 4; i++)
		out[i] = (uint32_t)stub.last_tx[4 * i] | ((uint32_t)stub.last_tx[4 * i + 1] << 8) |
		         ((uint32_t)stub.last_tx[4 * i + 2] << 16) |
		         ((uint32_t)stub.last_tx[4 * i + 3] << 24);
	return (uint16_t)(stub.last_tx_len / 4);
}

static void host_out_sysex(uint8_t group, const uint8_t *data, uint16_t len)
{
	uint32_t words[UMP_RX_WORDS];
	uint16_t count = ump_sysex7_pack(words, UMP_RX_WORDS, group, data, len);
	CHECK(count != 0);
	for (uint16_t i = 0; i < count; i += 16) {
		uint16_t rem = (uint16_t)(count - i);
		host_out_words(words + i, rem < 16 ? rem : 16);
		__wrap_midi_parsers_poll();
	}
}

#define MAX_RX_CI 16
static struct ump_sysex7_rx g_ci_msgs[MAX_RX_CI];
static uint8_t g_ci_count;

static void drain_ci_from_device(void)
{
	struct ump_sysex7_rx cur;
	uint32_t words[16];
	uint16_t count;

	g_ci_count = 0;
	ump_sysex7_rx_reset(&cur);
	for (;;) {
		if ((count = host_in_words(words)) == 0)
			break;
		for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
			if ((words[i] >> 28) != 3)
				continue;
			int r = ump_sysex7_rx_feed(&cur, words + i);
			CHECK(r >= 0);
			if (r == 1) {
				if (g_ci_count < MAX_RX_CI)
					g_ci_msgs[g_ci_count++] = cur;
				ump_sysex7_rx_reset(&cur);
			}
		}
	}
	CHECK_EQ(cur.active, 0);
}

static uint16_t build_discovery_reply(uint8_t *buf, uint32_t src, uint32_t dst,
                                      uint8_t ver, uint8_t cat, uint32_t max_sysex, uint8_t fb)
{
	uint16_t n = 0;
	buf[n++] = 0x7E;
	buf[n++] = 0x7F;
	buf[n++] = 0x0D;
	buf[n++] = 0x71;
	buf[n++] = ver;
	write_muid(buf + n, src); n += 4;
	write_muid(buf + n, dst); n += 4;
	buf[n++] = 0x00; buf[n++] = 0x20; buf[n++] = 0x33; /* mfg */
	buf[n++] = 0x12; buf[n++] = 0x01; /* family = 0x0092 */
	buf[n++] = 0x34; buf[n++] = 0x02; /* model = 0x0134 */
	buf[n++] = 1; buf[n++] = 2; buf[n++] = 3; buf[n++] = 4; /* sw_rev */
	buf[n++] = cat;
	write_muid(buf + n, max_sysex); n += 4;
	if (ver >= 2) {
		buf[n++] = 0; /* output path id */
		buf[n++] = fb;
	}
	return n;
}

static uint16_t build_pe_get_reply(uint8_t *buf, uint32_t src, uint32_t dst, uint8_t req_id,
                                   const char *hdr, uint16_t cnt, uint16_t num,
                                   const char *data, uint16_t dlen)
{
	uint16_t n = 0;
	uint16_t hl = hdr ? (uint16_t)strlen(hdr) : 0u;
	buf[n++] = 0x7E; buf[n++] = 0x7F; buf[n++] = 0x0D; buf[n++] = 0x35; buf[n++] = 0x02;
	write_muid(buf + n, src); n += 4;
	write_muid(buf + n, dst); n += 4;
	buf[n++] = req_id;
	buf[n++] = (uint8_t)(hl & 0x7Fu);
	buf[n++] = (uint8_t)((hl >> 7) & 0x7Fu);
	if (hl) {
		memcpy(buf + n, hdr, hl);
		n += hl;
	}
	buf[n++] = (uint8_t)(cnt & 0x7Fu); buf[n++] = (uint8_t)((cnt >> 7) & 0x7Fu);
	buf[n++] = (uint8_t)(num & 0x7Fu); buf[n++] = (uint8_t)((num >> 7) & 0x7Fu);
	buf[n++] = (uint8_t)(dlen & 0x7Fu); buf[n++] = (uint8_t)((dlen >> 7) & 0x7Fu);
	if (dlen) {
		memcpy(buf + n, data, dlen);
		n += dlen;
	}
	return n;
}

static void test_ump_negotiation_boot_trigger_and_reset(void)
{
	static const uint32_t ep_disc[4] = {0xF0000101u, 0x00000001u, 0, 0};
	static const uint32_t fb_disc[4] = {0xF010FF03u, 0, 0, 0};

	init_alt1();
	CHECK_EQ(ump_stream_negotiated(), 0);
	CHECK_EQ(midi2_ump_negotiated(), 0);
	CHECK_EQ(midici_initiator_is_discovering(), 0);

	/* Not configured -> negotiation stays 0 */
	bDeviceState = USB_STATE_ADDRESSED;
	host_out_words(ep_disc, 4);
	__wrap_midi_parsers_poll();
	host_out_words(fb_disc, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(ump_stream_negotiated(), 1);
	CHECK_EQ(midi2_ump_negotiated(), 0);
	CHECK_EQ(midici_tx_pending(), 0);

	/* Configured -> triggers Discovery once */
	bDeviceState = USB_STATE_CONFIGURED;
	CHECK_EQ(midi2_ump_negotiated(), 1);
	__wrap_midi_parsers_poll();
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x70);

	/* Repeated EP/FB Discovery does not flood Discovery */
	host_out_words(ep_disc, 4);
	__wrap_midi_parsers_poll();
	host_out_words(fb_disc, 4);
	__wrap_midi_parsers_poll();
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);

	/* Alt 0 reset clears state & re-arms for next Alt 1 negotiation */
	usb_midi2_set_alt(0);
	usb_midi2_service();
	CHECK_EQ(ump_stream_negotiated(), 0);
	CHECK_EQ(midi2_ump_negotiated(), 0);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK_EQ(midici_initiator_peer_count(), 0);

	usb_midi2_set_alt(1);
	usb_midi2_service();
	host_out_words(ep_disc, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(midi2_ump_negotiated(), 0);
	host_out_words(fb_disc, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(midi2_ump_negotiated(), 1);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x70);
}

static void test_discovery_wire_format_and_followups(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	const uint32_t peer_muid = 0x01234567u;
	ci_peer_info_t peer;

	init_alt1();
	midici_initiator_start_discovery(100u);
	my_muid = midici_muid();
	CHECK_EQ(midici_initiator_is_discovering(), 1);

	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	/* F0 + 30B payload + F7 = 32B */
	CHECK_EQ(g_ci_msgs[0].len, 32u);
	CHECK_EQ(g_ci_msgs[0].buf[1], 0x7Eu);
	CHECK_EQ(g_ci_msgs[0].buf[2], 0x7Fu);
	CHECK_EQ(g_ci_msgs[0].buf[3], 0x0Du);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x70u);
	CHECK_EQ(g_ci_msgs[0].buf[5], 0x02u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 6), my_muid);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), 0x0FFFFFFFu);
	CHECK_EQ(g_ci_msgs[0].buf[14], CI_MANUFACTURER_0);
	CHECK_EQ(g_ci_msgs[0].buf[15], CI_MANUFACTURER_1);
	CHECK_EQ(g_ci_msgs[0].buf[16], CI_MANUFACTURER_2);
	CHECK_EQ(g_ci_msgs[0].buf[17], (uint8_t)(CI_FAMILY & 0x7Fu));
	CHECK_EQ(g_ci_msgs[0].buf[18], (uint8_t)((CI_FAMILY >> 7) & 0x7Fu));
	CHECK_EQ(g_ci_msgs[0].buf[25], 0x08u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 26), CI_REASSEMBLY_BYTES);
	CHECK_EQ(g_ci_msgs[0].buf[30], 0u);

	/* Truncated DiscoveryReply (30B v2 < 31B, 28B v1.1 < 29B) must be rejected */
	n = build_discovery_reply(msg, peer_muid, my_muid, 2, 0x0C, 512, 2);
	CHECK(!midici_initiator_on_message(0, msg, (uint16_t)(n - 1), 105u) ||
	      midici_initiator_peer_count() == 0);
	CHECK_EQ(midici_initiator_peer_count(), 0);
	n = build_discovery_reply(msg, peer_muid, my_muid, 1, 0x0C, 512, 0);
	midici_initiator_on_message(0, msg, (uint16_t)(n - 1), 106u);
	CHECK_EQ(midici_initiator_peer_count(), 0);

	/* Valid v2 DiscoveryReply with Profiles + PE */
	n = build_discovery_reply(msg, peer_muid, my_muid, 2, 0x0C, 1024, 3);
	CHECK(midici_initiator_on_message(0, msg, n, 110u));
	CHECK_EQ(midici_initiator_peer_count(), 1);
	CHECK(midici_initiator_get_peer(0, &peer));
	CHECK_EQ(peer.muid, peer_muid);
	CHECK_EQ(peer.family, 0x0092u);
	CHECK_EQ(peer.model, 0x0134u);
	CHECK_EQ(peer.max_sysex, 1024u);
	CHECK_EQ(peer.categories, 0x0Cu);
	CHECK_EQ(peer.function_block, 3u);
	CHECK_EQ(peer.flags, CI_PEER_FLAG_DISCOVERED);

	/* Tick 1 -> Endpoint Inquiry (0x72, 14B + F0/F7 = 16B) */
	midici_initiator_tick(111u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].len, 16u);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x72u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peer_muid);
	CHECK_EQ(g_ci_msgs[0].buf[14], 0x00u);

	/* Tick 2 -> Profile Inquiry (0x20, 13B + F0/F7 = 15B) */
	midici_initiator_tick(112u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].len, 15u);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x20u);

	/* Tick 3 -> PE Capabilities Inquiry (0x30, 16B + F0/F7 = 18B, {1,0,0}) */
	midici_initiator_tick(113u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].len, 18u);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x30u);
	CHECK_EQ(g_ci_msgs[0].buf[14], 1u);
	CHECK_EQ(g_ci_msgs[0].buf[15], 0u);
	CHECK_EQ(g_ci_msgs[0].buf[16], 0u);

	/* Next tick without PE Caps reply sends nothing */
	midici_initiator_tick(114u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);

	/* Feed Endpoint Reply (PIID = "MIDI2Sound") */
	{
		const char *piid = "MIDI2Sound";
		uint16_t plen = (uint16_t)strlen(piid);
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0x00;
		msg[k++] = (uint8_t)(plen & 0x7Fu); msg[k++] = 0;
		memcpy(msg + k, piid, plen); k += plen;
		CHECK(midici_initiator_on_message(0, msg, k, 115u));
	}

	/* Feed Profile Inquiry Reply (2 enabled, 1 disabled) */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x21; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0;
		memset(msg + k, 0x21, 10); k += 10;
		msg[k++] = 1; msg[k++] = 0;
		memset(msg + k, 0x22, 5); k += 5;
		CHECK(midici_initiator_on_message(0, msg, k, 116u));
	}

	/* Truncated PE Caps Reply (13B < 14B) is ignored */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		midici_initiator_on_message(0, msg, k, 117u);
		CHECK(midici_initiator_get_peer(0, &peer));
		CHECK_EQ(peer.flags & CI_PEER_FLAG_PE_CAPS, 0u);
		msg[k++] = 2u; /* simultaneous = 2 */
		msg[k++] = 0u;
		msg[k++] = 0u;
		CHECK(midici_initiator_on_message(0, msg, k, 118u));
	}

	/* Next tick -> Get DeviceInfo (0x34, 47B + F0/F7 = 49B) */
	midici_initiator_tick(119u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].len, 49u);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	uint8_t req_id = g_ci_msgs[0].buf[14];
	CHECK(req_id >= 1u && req_id <= 127u);

	/* Feed 3-chunk DeviceInfo reply */
	const char *d1 = "{\"manufacturerId\":[0,32,41],";
	const char *d2 = "\"familyId\":[81,0],\"modelId\":[0,0],";
	const char *d3 = "\"versionId\":[0,1,0,0],\"serialNumber\":\"MIDI2Sound\"}";
	n = build_pe_get_reply(msg, peer_muid, my_muid, req_id, "{\"status\":200}", 3, 1,
	                       d1, (uint16_t)strlen(d1));
	CHECK(midici_initiator_on_message(0, msg, n, 120u));
	CHECK(midici_initiator_get_peer(0, &peer));
	CHECK_EQ(peer.flags & CI_PEER_FLAG_COMPLETE, 0u);

	n = build_pe_get_reply(msg, peer_muid, my_muid, req_id, "", 3, 2,
	                       d2, (uint16_t)strlen(d2));
	CHECK(midici_initiator_on_message(0, msg, n, 121u));

	n = build_pe_get_reply(msg, peer_muid, my_muid, req_id, "", 3, 3,
	                       d3, (uint16_t)strlen(d3));
	CHECK(midici_initiator_on_message(0, msg, n, 122u));

	CHECK(midici_initiator_get_peer(0, &peer));
	CHECK_EQ(peer.device_info_status, 200u);
	CHECK_EQ(peer.pe_simultaneous, 2u);
	CHECK_EQ(peer.profiles_enabled, 2u);
	CHECK_EQ(peer.profiles_disabled, 1u);
	CHECK(strcmp(peer.product_instance_id, "MIDI2Sound") == 0);
	CHECK_EQ(peer.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_ENDPOINT | CI_PEER_FLAG_PROFILES |
	         CI_PEER_FLAG_PE_CAPS | CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_COMPLETE);
}

static void test_window_wraparound_and_backpressure(void)
{
	uint8_t msg[64];
	uint16_t n;
	uint32_t my_muid;

	init_alt1();
	midici_initiator_start_discovery(0xFFFFF800u);
	my_muid = midici_muid();
	drain_ci_from_device();
	CHECK_EQ(midici_initiator_is_discovering(), 1);

	/* 2999 ms elapsed across uint32_t wraparound */
	midici_initiator_tick(0x000003B7u);
	CHECK_EQ(midici_initiator_is_discovering(), 1);

	/* 3000 ms elapsed -> closes */
	midici_initiator_tick(0x000003B8u);
	CHECK_EQ(midici_initiator_is_discovering(), 0);

	/* 8-peer max + CiTxQueue backpressure -> 0 overflows */
	init_alt1();
	midici_initiator_start_discovery(10u);
	my_muid = midici_muid();
	drain_ci_from_device();

	for (uint32_t i = 0; i < 9; i++) {
		n = build_discovery_reply(msg, 0x00100001u + i, my_muid, 2, 0x0C, 512, 0);
		midici_initiator_on_message(0, msg, n, 11u);
	}
	CHECK_EQ(midici_initiator_peer_count(), 8u);

	/* Fill CiTxQueue so < 3 packets remain free */
	{
		uint8_t fill60[60];
		uint8_t fill6[6];
		memset(fill60, 0x01, sizeof fill60);
		memset(fill6, 0x02, sizeof fill6);
		while (midici_tx_free_packets() >= 10)
			CHECK_EQ(midici_send_sysex(1, fill60, sizeof fill60), 0);
		while (midici_tx_free_packets() > 1)
			CHECK_EQ(midici_send_sysex(1, fill6, sizeof fill6), 0);
	}
	{
		uint32_t ovf0 = midici_tx_overflows();
		midici_initiator_tick(12u);
		midici_initiator_tick(13u);
		CHECK_EQ(midici_tx_overflows(), ovf0);
	}

	/* Drain queue and pace all 24 follow-ups (3 per peer) with 1 per tick */
	{
		uint32_t w[16];
		while (host_in_words(w) > 0) {}
	}
	int sent = 0;
	for (int t = 0; t < 40; t++) {
		midici_initiator_tick((uint32_t)(20 + t));
		drain_ci_from_device();
		CHECK(g_ci_count <= 1);
		sent += g_ci_count;
	}
	CHECK_EQ(sent, 24);
	CHECK_EQ(midici_tx_overflows(), 0u);
}

static void test_pe_errors_nak_timeout_and_invalidate(void)
{
	uint8_t msg[360];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t p1 = 0x00200001u;
	const uint32_t p2 = 0x00200002u;
	const uint32_t p3 = 0x00200003u;

	init_alt1();
	midici_initiator_start_discovery(0xFFFFFC00u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, p1, my_muid, 2, 0x08, 512, 0);
	midici_initiator_on_message(0, msg, n, 0xFFFFFC01u);
	n = build_discovery_reply(msg, p2, my_muid, 2, 0x08, 512, 0);
	midici_initiator_on_message(0, msg, n, 0xFFFFFC01u);
	n = build_discovery_reply(msg, p3, my_muid, 2, 0x08, 512, 0);
	midici_initiator_on_message(0, msg, n, 0xFFFFFC01u);

	/* Pace Endpoint + PE Caps for 3 peers (6 ticks) */
	for (int i = 0; i < 6; i++) {
		midici_initiator_tick(0xFFFFFC02u + (uint32_t)i);
		drain_ci_from_device();
	}
	/* Answer Endpoint + PE Caps for p1, p2, p3 */
	for (uint32_t i = 0; i < 3; i++) {
		uint32_t pm = 0x00200001u + i;
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, pm); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 2; msg[k++] = 0; msg[k++] = 'O'; msg[k++] = 'K';
		midici_initiator_on_message(0, msg, k, 0xFFFFFC10u);

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, pm); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		midici_initiator_on_message(0, msg, k, 0xFFFFFC10u);
	}

	/* Tick -> p1 Get DeviceInfo; let it time out after 3000 ms (wraparound) */
	midici_initiator_tick(0xFFFFFC20u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), p1);

	/* 2999 ms later -> still waiting, no p2 Get sent */
	midici_initiator_tick(0xFFFFFC20u + 2999u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);

	/* 3000 ms later -> p1 times out (NAK + COMPLETE), same tick sends p2 Get! */
	midici_initiator_tick(0xFFFFFC20u + 3000u);
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK(p.flags & CI_PEER_FLAG_NAK);
	CHECK(p.flags & CI_PEER_FLAG_COMPLETE);
	CHECK(!(p.flags & CI_PEER_FLAG_DEVICE_INFO));
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), p2);
	uint8_t p2_rid = g_ci_msgs[0].buf[14];

	/* p2 returns 404 -> device_info_status == 404, NAK + COMPLETE */
	n = build_pe_get_reply(msg, p2, my_muid, p2_rid, "{\"status\":404}", 1, 1, "", 0);
	midici_initiator_on_message(0, msg, n, 0x00000800u);
	CHECK(midici_initiator_get_peer(1, &p));
	CHECK_EQ(p.device_info_status, 404u);
	CHECK(p.flags & CI_PEER_FLAG_NAK);
	CHECK(p.flags & CI_PEER_FLAG_COMPLETE);

	/* Next tick -> p3 Get DeviceInfo; send 0x7F NAK to p3 */
	midici_initiator_tick(0x00000801u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), p3);
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x7F; msg[k++] = 0x02;
		write_muid(msg + k, p3); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0x34; msg[k++] = 0x01; msg[k++] = 0x00;
		memset(msg + k, 0, 7); k += 7;
		midici_initiator_on_message(0, msg, k, 0x00000802u);
	}
	CHECK(midici_initiator_get_peer(2, &p));
	CHECK(p.flags & CI_PEER_FLAG_NAK);
	CHECK(p.flags & CI_PEER_FLAG_COMPLETE);

	/* InvalidateMuid (0x7E) targeting p2 compacts peer list */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x7E; msg[k++] = 0x02;
		write_muid(msg + k, p3); k += 4;
		write_muid(msg + k, 0x0FFFFFFFu); k += 4;
		write_muid(msg + k, p2); k += 4;
		midici_initiator_on_message(0, msg, k, 0x00000803u);
	}
	CHECK_EQ(midici_initiator_peer_count(), 2u);
	CHECK(midici_initiator_get_peer(1, &p));
	CHECK_EQ(p.muid, p3);
}

static void test_self_echo_and_loopback_discovery_vs_collision(void)
{
	uint8_t msg[64];
	uint32_t my_muid;

	init_alt1();
	midici_initiator_start_discovery(500u);
	my_muid = midici_muid();
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);

	/* Self-echo on 0x71 (SourceMuid == my_muid) is dropped */
	build_discovery_reply(msg, my_muid, my_muid, 2, 0x08, 512, 0);
	CHECK(!midici_initiator_on_message(0, msg, 31, 501u));
	CHECK_EQ(midici_initiator_peer_count(), 0u);

	/* Loopback of Launchpad's own Discovery (F0..F7) through responder does NOT trigger MuidCollision */
	host_out_sysex(0, g_ci_msgs[0].buf, g_ci_msgs[0].len);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);
	CHECK_EQ(midici_muid(), my_muid);

	/* Foreign Discovery with same MUID still triggers MuidCollision (0x7E emitted + new MUID) */
	{
		uint8_t foreign[32];
		memcpy(foreign, g_ci_msgs[0].buf, 32);
		foreign[14] ^= 0x01u; /* different manufacturer */
		host_out_sysex(0, foreign, 32);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
		CHECK_EQ(g_ci_msgs[0].buf[4], 0x7Eu);
		CHECK(midici_muid() != my_muid);
	}
}

static void test_alt_switch_preserves_clock_for_negotiation_window(void)
{
	static const uint32_t ep_disc[4] = {0xF0000101u, 0x00000001u, 0, 0};
	static const uint32_t fb_disc[4] = {0xF010FF03u, 0, 0, 0};

	init_alt1();
	midici_initiator_tick(5000u);
	usb_midi2_set_alt(0);
	usb_midi2_service();
	usb_midi2_set_alt(1);
	usb_midi2_service();

	/* ST_EP_DISCOVERY + ST_FB_DISCOVERY before another tick() runs */
	host_out_words(ep_disc, 4);
	__wrap_midi_parsers_poll();
	host_out_words(fb_disc, 4);
	__wrap_midi_parsers_poll();
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x70);
	CHECK_EQ(midici_initiator_is_discovering(), 1);

	/* Next tick at 5000 ms must NOT expire the 3000 ms window prematurely */
	midici_initiator_tick(5000u);
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	midici_initiator_tick(7999u);
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	midici_initiator_tick(8000u);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
}

static void test_discovery_window_3000ms_enforcement_and_post_window_followups(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t peer;
	const uint32_t p_in_window = 0x00300001u;
	const uint32_t p_at_expiry = 0x00300002u;

	/* 1. 0x71 at +2999 ms (wraparound) accepted; 0x71 at +3000 ms without prior tick rejected */
	init_alt1();
	midici_initiator_start_discovery(0xFFFFF800u);
	my_muid = midici_muid();
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);

	n = build_discovery_reply(msg, p_in_window, my_muid, 2, 0x0C, 512, 1);
	CHECK(midici_initiator_on_message(0, msg, n, 0x000003B7u)); /* +2999 ms */
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	CHECK_EQ(midici_initiator_peer_count(), 1u);

	/* Directly inject 0x71 at +3000 ms without calling midici_initiator_tick() first */
	n = build_discovery_reply(msg, p_at_expiry, my_muid, 2, 0x0C, 512, 2);
	midici_initiator_on_message(0, msg, n, 0x000003B8u); /* +3000 ms */
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	CHECK_EQ(midici_initiator_peer_count(), 1u);

	/* Follow-ups (0x72, 0x20, 0x30) run only for p_in_window after 3000 ms */
	static const uint8_t expected_types[3] = {0x72u, 0x20u, 0x30u};
	for (int i = 0; i < 3; i++) {
		midici_initiator_tick(0x000003B9u + (uint32_t)i);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
		CHECK_EQ(g_ci_msgs[0].buf[4], expected_types[i]);
		CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), p_in_window);
	}
	midici_initiator_tick(0x000003BCu);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);

	/* Follow-up replies (0x73, 0x21, 0x31, 0x35) arriving after 3000 ms still complete */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, p_in_window); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 2; msg[k++] = 0; msg[k++] = 'P'; msg[k++] = '1';
		CHECK(midici_initiator_on_message(0, msg, k, 0x000003C0u));

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x21; msg[k++] = 0x02;
		write_muid(msg + k, p_in_window); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0;
		memset(msg + k, 0x21, 5); k += 5;
		msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 0x000003C1u));

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, p_in_window); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 0x000003C2u));
	}
	midici_initiator_tick(0x000003C3u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), p_in_window);
	{
		uint8_t req_id = g_ci_msgs[0].buf[14];
		const char *body = "{\"manufacturerId\":[0,32,41]}";
		n = build_pe_get_reply(msg, p_in_window, my_muid, req_id, "{\"status\":200}", 1, 1,
		                       body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 0x000003C4u));
	}
	CHECK(midici_initiator_get_peer(0, &peer));
	CHECK_EQ(peer.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_ENDPOINT | CI_PEER_FLAG_PROFILES |
	         CI_PEER_FLAG_PE_CAPS | CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_COMPLETE);

	/* 2. Late 0x71 after midici_initiator_tick() closed the window is ignored with no follow-ups */
	init_alt1();
	midici_initiator_start_discovery(0xFFFFF800u);
	my_muid = midici_muid();
	drain_ci_from_device();
	midici_initiator_tick(0x000003B8u);
	CHECK_EQ(midici_initiator_is_discovering(), 0);

	n = build_discovery_reply(msg, 0x00300003u, my_muid, 2, 0x0C, 512, 0);
	midici_initiator_on_message(0, msg, n, 0x000003B8u);
	n = build_discovery_reply(msg, 0x00300004u, my_muid, 2, 0x0C, 512, 0);
	midici_initiator_on_message(0, msg, n, 0x00001000u);
	CHECK_EQ(midici_initiator_peer_count(), 0u);
	midici_initiator_tick(0x00001001u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);

	/* 3. Deferred 0x70 under full CiTxQueue is cancelled when 3000 ms window closes */
	init_alt1();
	{
		uint8_t fill60[60];
		uint8_t fill6[6];
		memset(fill60, 0x01, sizeof fill60);
		memset(fill6, 0x02, sizeof fill6);
		while (midici_tx_free_packets() >= 10)
			CHECK_EQ(midici_send_sysex(1, fill60, sizeof fill60), 0);
		while (midici_tx_free_packets() > 2)
			CHECK_EQ(midici_send_sysex(1, fill6, sizeof fill6), 0);
	}
	midici_initiator_start_discovery(1000u);
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	midici_initiator_tick(3999u);
	CHECK_EQ(midici_initiator_is_discovering(), 1);
	midici_initiator_tick(4000u);
	CHECK_EQ(midici_initiator_is_discovering(), 0);
	{
		uint32_t w[16];
		while (host_in_words(w) > 0) {}
	}
	midici_initiator_tick(4001u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);
}

static void test_duplicate_pe_caps_after_nak_404_timeout(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t peers_muid[3] = {0x00400001u, 0x00400002u, 0x00400003u};

	init_alt1();
	midici_initiator_start_discovery(100u);
	my_muid = midici_muid();
	drain_ci_from_device();

	for (uint8_t i = 0; i < 3; i++) {
		n = build_discovery_reply(msg, peers_muid[i], my_muid, 2, 0x08, 512, 0);
		CHECK(midici_initiator_on_message(0, msg, n, 101u));
	}
	for (int t = 0; t < 6; t++) {
		midici_initiator_tick(102u + (uint32_t)t);
		drain_ci_from_device();
	}
	for (uint8_t i = 0; i < 3; i++) {
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, peers_muid[i]); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 2; msg[k++] = 0; msg[k++] = 'O'; msg[k++] = 'K';
		CHECK(midici_initiator_on_message(0, msg, k, 110u));

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peers_muid[i]); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 110u));
	}

	/* p0: timeout after 3000 ms -> CI_PEER_FLAG_NAK; tick immediately sends p1 0x34 */
	midici_initiator_tick(120u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peers_muid[0]);

	midici_initiator_tick(3120u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peers_muid[1]);

	/* p1: 404 -> CI_PEER_FLAG_NAK */
	n = build_pe_get_reply(msg, peers_muid[1], my_muid, g_ci_msgs[0].buf[14],
	                       "{\"status\":404}", 1, 1, "", 0);
	CHECK(midici_initiator_on_message(0, msg, n, 3121u));

	/* p2: 0x7F NAK to 0x34 -> CI_PEER_FLAG_NAK */
	midici_initiator_tick(3122u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peers_muid[2]);
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x7F; msg[k++] = 0x02;
		write_muid(msg + k, peers_muid[2]); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0x34; msg[k++] = 0x01; msg[k++] = 0x00;
		memset(msg + k, 0, 7); k += 7;
		CHECK(midici_initiator_on_message(0, msg, k, 3123u));
	}

	/* Duplicate 0x31 for all 3 NAK'd peers must NOT re-queue TX_PE_GET */
	for (uint8_t i = 0; i < 3; i++) {
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peers_muid[i]); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 3124u + i));
	}
	for (uint8_t t = 0; t < 4; t++) {
		midici_initiator_tick(3130u + t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 0);
	}
	for (uint8_t i = 0; i < 3; i++) {
		CHECK(midici_initiator_get_peer(i, &p));
		CHECK(p.flags & CI_PEER_FLAG_NAK);
		CHECK(p.flags & CI_PEER_FLAG_COMPLETE);
		CHECK(!(p.flags & CI_PEER_FLAG_DEVICE_INFO));
	}
}

static void send_nak_msg(uint8_t *msg, uint32_t src, uint32_t dst,
                         uint8_t ver, uint8_t orig_type, uint32_t now_ms)
{
	uint16_t k = 0;
	msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x7F; msg[k++] = ver;
	write_muid(msg + k, src); k += 4;
	write_muid(msg + k, dst); k += 4;
	msg[k++] = orig_type;
	msg[k++] = 0x01; /* Not Supported */
	msg[k++] = 0x00;
	memset(msg + k, 0, 7); k += 7;
	CHECK(midici_initiator_on_message(0, msg, k, now_ms));
}

static void test_pe_after_endpoint_and_profile_nak(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t v11_peer = 0x00500001u;
	const uint32_t v12_peer = 0x00500002u;

	init_alt1();
	midici_initiator_start_discovery(100u);
	my_muid = midici_muid();
	drain_ci_from_device();

	/* v1.1 PE+Profile peer + v1.2 PE+Profile peer */
	n = build_discovery_reply(msg, v11_peer, my_muid, 1, 0x0C, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 101u));
	n = build_discovery_reply(msg, v12_peer, my_muid, 2, 0x0C, 512, 1);
	CHECK(midici_initiator_on_message(0, msg, n, 102u));

	/* Pace 0x72, 0x20, 0x30 for both peers (6 ticks) */
	for (int t = 0; t < 6; t++) {
		midici_initiator_tick(103u + (uint32_t)t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
	}

	/* v1.1 peer NAKs 0x72 (EndpointInquiry), replies 0x21 (Profile), then replies 0x31 */
	send_nak_msg(msg, v11_peer, my_muid, 0x01, 0x72, 110u);
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x21; msg[k++] = 0x01;
		write_muid(msg + k, v11_peer); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0;
		memset(msg + k, 0x21, 5); k += 5;
		msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 111u));

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x01;
		write_muid(msg + k, v11_peer); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 112u));
	}

	/* v1.2 peer NAKs BOTH 0x72 and 0x20 prior to 0x31 */
	send_nak_msg(msg, v12_peer, my_muid, 0x02, 0x72, 113u);
	send_nak_msg(msg, v12_peer, my_muid, 0x02, 0x20, 114u);
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, v12_peer); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 115u));
	}

	/* Neither peer is COMPLETE yet; next tick MUST send 0x34 Get DeviceInfo to v1.1 peer */
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);
	midici_initiator_tick(116u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), v11_peer);
	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, v11_peer, my_muid, g_ci_msgs[0].buf[14],
		                       "{\"status\":200}", 1, 1, body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 117u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_PROFILES | CI_PEER_FLAG_PE_CAPS |
	         CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Next tick MUST send 0x34 Get DeviceInfo to v1.2 peer */
	midici_initiator_tick(118u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), v12_peer);
	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, v12_peer, my_muid, g_ci_msgs[0].buf[14],
		                       "{\"status\":200}", 1, 1, body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 119u));
	}
	CHECK(midici_initiator_get_peer(1, &p));
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_PE_CAPS |
	         CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Subsequent duplicate 0x31 for either peer must NOT re-queue 0x34 */
	for (uint8_t i = 0; i < 2; i++) {
		uint32_t pm = (i == 0) ? v11_peer : v12_peer;
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, pm); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 120u + i));
	}
	for (uint8_t t = 0; t < 3; t++) {
		midici_initiator_tick(125u + t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 0);
	}
}

static void test_pe_caps_zero_simultaneous_cancels_queued_pe_get(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t peer_muid = 0x00600001u;

	init_alt1();
	midici_initiator_start_discovery(200u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, peer_muid, my_muid, 2, 0x08, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 201u));

	/* Pace 0x72 and 0x30 */
	midici_initiator_tick(202u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x72u);

	midici_initiator_tick(203u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x30u);

	/* Answer 0x73 */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 2; msg[k++] = 0; msg[k++] = 'P'; msg[k++] = '0';
		CHECK(midici_initiator_on_message(0, msg, k, 204u));
	}

	/* First 0x31 has pe_simultaneous = 2 (queues TX_PE_GET) */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 205u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);

	/* Before tick transmits 0x34, second 0x31 arrives with pe_simultaneous = 0 */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 206u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.pe_simultaneous, 0u);
	CHECK(p.flags & CI_PEER_FLAG_COMPLETE);
	CHECK(!(p.flags & CI_PEER_FLAG_DEVICE_INFO));

	for (uint8_t t = 0; t < 3; t++) {
		midici_initiator_tick(210u + t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 0);
	}
}

static void send_bare_v11_nak_msg(uint8_t *msg, uint32_t src, uint32_t dst, uint32_t now_ms)
{
	uint16_t k = 0;
	msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x7F; msg[k++] = 0x01;
	write_muid(msg + k, src); k += 4;
	write_muid(msg + k, dst); k += 4;
	CHECK_EQ(k, 13u);
	CHECK(midici_initiator_on_message(0, msg, k, now_ms));
}

static void test_v11_bare_13byte_nak_to_endpoint_and_pe_caps(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t peer_pe_prof = 0x00700001u;
	const uint32_t peer_nak_all = 0x00700002u;

	init_alt1();
	midici_initiator_start_discovery(300u);
	my_muid = midici_muid();
	drain_ci_from_device();

	/* Peer 0: v1.1 PE+Profile; Peer 1: v1.1 PE-only */
	n = build_discovery_reply(msg, peer_pe_prof, my_muid, 1, 0x0C, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 301u));
	n = build_discovery_reply(msg, peer_nak_all, my_muid, 1, 0x08, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 302u));

	/* Pace Peer 0 (0x72, 0x20, 0x30) + Peer 1 (0x72, 0x30) = 5 ticks */
	for (int t = 0; t < 5; t++) {
		midici_initiator_tick(303u + (uint32_t)t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
	}

	/* Peer 0 sends real 13-byte bare v1.1 NAK to 0x72, then 0x21, then 0x31 (sim = 1) */
	send_bare_v11_nak_msg(msg, peer_pe_prof, my_muid, 310u);
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x21; msg[k++] = 0x01;
		write_muid(msg + k, peer_pe_prof); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0;
		memset(msg + k, 0x21, 5); k += 5;
		msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 311u));

		k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x01;
		write_muid(msg + k, peer_pe_prof); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 312u));
	}

	/* Peer 1 sends 13-byte bare v1.1 NAKs to BOTH 0x72 and 0x30 -> must settle COMPLETE */
	send_bare_v11_nak_msg(msg, peer_nak_all, my_muid, 313u);
	CHECK(midici_initiator_get_peer(1, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);
	send_bare_v11_nak_msg(msg, peer_nak_all, my_muid, 314u);
	CHECK(midici_initiator_get_peer(1, &p));
	CHECK_EQ(p.flags, CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Peer 0 must NOT be complete yet; next tick MUST emit 0x34 Get DeviceInfo to Peer 0 */
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);
	midici_initiator_tick(315u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peer_pe_prof);

	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, peer_pe_prof, my_muid, g_ci_msgs[0].buf[14],
		                       "{\"status\":200}", 1, 1, body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 316u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_PROFILES | CI_PEER_FLAG_PE_CAPS |
	         CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Duplicate 0x31 for Peer 0 must NOT re-queue 0x34 */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x01;
		write_muid(msg + k, peer_pe_prof); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 317u));
	}
	for (uint8_t t = 0; t < 3; t++) {
		midici_initiator_tick(320u + t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 0);
	}
}

static void test_pe_caps_zero_simultaneous_cancels_active_pe_get(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p1, p2;
	const uint32_t peer1 = 0x00800001u;
	const uint32_t peer2 = 0x00800002u;

	init_alt1();
	midici_initiator_start_discovery(400u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, peer1, my_muid, 2, 0x08, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 401u));
	n = build_discovery_reply(msg, peer2, my_muid, 2, 0x08, 512, 1);
	CHECK(midici_initiator_on_message(0, msg, n, 402u));

	/* Pace 0x72 and 0x30 for both peers (4 ticks) */
	for (int t = 0; t < 4; t++) {
		midici_initiator_tick(403u + (uint32_t)t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
	}

	/* Answer 0x73 for both peers */
	for (int i = 0; i < 2; i++) {
		uint32_t pm = (i == 0) ? peer1 : peer2;
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x02;
		write_muid(msg + k, pm); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 2; msg[k++] = 0; msg[k++] = 'P'; msg[k++] = (uint8_t)('1' + i);
		CHECK(midici_initiator_on_message(0, msg, k, 410u + (uint32_t)i));
	}

	/* Both peers reply 0x31 with pe_simultaneous > 0 -> both queue TX_PE_GET */
	for (int i = 0; i < 2; i++) {
		uint32_t pm = (i == 0) ? peer1 : peer2;
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, pm); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 2; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 412u + (uint32_t)i));
	}

	/* Tick transmits 0x34 for Peer 1 (pe_get.active == true, peer_muid == peer1) */
	midici_initiator_tick(415u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peer1);

	/* While pe_get is active for Peer 1, Peer 1 sends 0x31 with pe_simultaneous = 0 */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x02;
		write_muid(msg + k, peer1); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 416u));
	}
	CHECK(midici_initiator_get_peer(0, &p1));
	CHECK_EQ(p1.pe_simultaneous, 0u);
	CHECK(p1.flags & CI_PEER_FLAG_COMPLETE);
	CHECK(!(p1.flags & CI_PEER_FLAG_NAK));
	CHECK(!(p1.flags & CI_PEER_FLAG_DEVICE_INFO));

	/* Very next tick MUST immediately transmit Peer 2's queued 0x34 */
	midici_initiator_tick(417u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	CHECK_EQ(read_muid(g_ci_msgs[0].buf + 10), peer2);

	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, peer2, my_muid, g_ci_msgs[0].buf[14],
		                       "{\"status\":200}", 1, 1, body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 418u));
	}
	CHECK(midici_initiator_get_peer(1, &p2));
	CHECK(p2.flags & CI_PEER_FLAG_DEVICE_INFO);
	CHECK(p2.flags & CI_PEER_FLAG_COMPLETE);

	/* Advance past 3000 ms timeout; Peer 1 must NEVER receive CI_PEER_FLAG_NAK */
	midici_initiator_tick(3420u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 0);
	CHECK(midici_initiator_get_peer(0, &p1));
	CHECK_EQ(p1.flags & CI_PEER_FLAG_NAK, 0u);
}

static void test_v11_out_of_order_bare_nak_case_a(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t peer_muid = 0x00900001u;

	init_alt1();
	midici_initiator_start_discovery(500u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, peer_muid, my_muid, 1, 0x0C, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 501u));

	/* Pace all 3 follow-ups: 0x72, 0x20, 0x30 */
	for (int t = 0; t < 3; t++) {
		midici_initiator_tick(502u + (uint32_t)t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
	}

	/* Bare 13-byte v1.1 NAK to 0x30 arrives BEFORE 0x73 and 0x21 */
	send_bare_v11_nak_msg(msg, peer_muid, my_muid, 510u);
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK(p.flags & CI_PEER_FLAG_NAK);
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);

	/* Now 0x73 arrives */
	{
		const char *piid = "CaseA";
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x73; msg[k++] = 0x01;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 0; msg[k++] = 5; msg[k++] = 0;
		memcpy(msg + k, piid, 5); k += 5;
		CHECK(midici_initiator_on_message(0, msg, k, 511u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);

	/* Now 0x21 arrives -> bare NAK resolves RX_PE_CAPS | RX_PE_GET and settles COMPLETE */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x21; msg[k++] = 0x01;
		write_muid(msg + k, peer_muid); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0;
		memset(msg + k, 0x21, 5); k += 5;
		msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 512u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK(strcmp(p.product_instance_id, "CaseA") == 0);
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_ENDPOINT | CI_PEER_FLAG_PROFILES |
	         CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Extra bare NAK after completion and subsequent ticks must NOT emit 0x34 */
	send_bare_v11_nak_msg(msg, peer_muid, my_muid, 513u);
	for (uint8_t t = 0; t < 3; t++) {
		midici_initiator_tick(514u + t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 0);
	}
}

static void test_v11_out_of_order_bare_nak_case_b(void)
{
	uint8_t msg[128];
	uint16_t n;
	uint32_t my_muid;
	ci_peer_info_t p;
	const uint32_t peer_pe = 0x00A00001u;
	const uint32_t peer_both = 0x00A00002u;

	/* Subcase 1: 0x72 + 0x30 in flight -> early 0x31(sim=1) -> 0x34 active -> bare NAK to 0x72 -> 0x35 200 */
	init_alt1();
	midici_initiator_start_discovery(600u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, peer_pe, my_muid, 1, 0x08, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 601u));

	/* Pace 0x72 and 0x30 */
	midici_initiator_tick(602u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x72u);

	midici_initiator_tick(603u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x30u);

	/* Early 0x31 (sim = 1) arrives before 0x72 reply/NAK */
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x01;
		write_muid(msg + k, peer_pe); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 604u));
	}

	/* Tick transmits 0x34 (pe_get.active == true) */
	midici_initiator_tick(605u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	uint8_t req_id = g_ci_msgs[0].buf[14];

	/* Bare 13-byte v1.1 NAK to 0x72 arrives while 0x34 is active -> MUST NOT abort pe_get */
	send_bare_v11_nak_msg(msg, peer_pe, my_muid, 606u);
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK(p.flags & CI_PEER_FLAG_NAK);
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);

	/* 0x35 (200) arrives -> BOTH DEVICE_INFO and COMPLETE must be set */
	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, peer_pe, my_muid, req_id, "{\"status\":200}", 1, 1,
		                       body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 607u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.device_info_status, 200u);
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_PE_CAPS |
	         CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);

	/* Subcase 2: 0x72+0x20+0x30 -> early 0x31 -> 0x34 active -> 2 bare NAKs (0x72,0x20) -> 0x35 200 */
	init_alt1();
	midici_initiator_start_discovery(700u);
	my_muid = midici_muid();
	drain_ci_from_device();

	n = build_discovery_reply(msg, peer_both, my_muid, 1, 0x0C, 512, 0);
	CHECK(midici_initiator_on_message(0, msg, n, 701u));
	for (int t = 0; t < 3; t++) {
		midici_initiator_tick(702u + (uint32_t)t);
		drain_ci_from_device();
		CHECK_EQ(g_ci_count, 1);
	}
	{
		uint16_t k = 0;
		msg[k++] = 0x7E; msg[k++] = 0x7F; msg[k++] = 0x0D; msg[k++] = 0x31; msg[k++] = 0x01;
		write_muid(msg + k, peer_both); k += 4;
		write_muid(msg + k, my_muid); k += 4;
		msg[k++] = 1; msg[k++] = 0; msg[k++] = 0;
		CHECK(midici_initiator_on_message(0, msg, k, 705u));
	}
	midici_initiator_tick(706u);
	drain_ci_from_device();
	CHECK_EQ(g_ci_count, 1);
	CHECK_EQ(g_ci_msgs[0].buf[4], 0x34u);
	req_id = g_ci_msgs[0].buf[14];

	send_bare_v11_nak_msg(msg, peer_both, my_muid, 707u);
	send_bare_v11_nak_msg(msg, peer_both, my_muid, 708u);
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.flags & CI_PEER_FLAG_COMPLETE, 0u);

	{
		const char *body = "{\"manufacturerId\":[0,32,51]}";
		n = build_pe_get_reply(msg, peer_both, my_muid, req_id, "{\"status\":200}", 1, 1,
		                       body, (uint16_t)strlen(body));
		CHECK(midici_initiator_on_message(0, msg, n, 709u));
	}
	CHECK(midici_initiator_get_peer(0, &p));
	CHECK_EQ(p.device_info_status, 200u);
	CHECK_EQ(p.flags,
	         CI_PEER_FLAG_DISCOVERED | CI_PEER_FLAG_PE_CAPS |
	         CI_PEER_FLAG_DEVICE_INFO | CI_PEER_FLAG_NAK | CI_PEER_FLAG_COMPLETE);
}

void test_midici_initiator_all(void)
{
	RUN(test_ump_negotiation_boot_trigger_and_reset);
	RUN(test_discovery_wire_format_and_followups);
	RUN(test_window_wraparound_and_backpressure);
	RUN(test_pe_errors_nak_timeout_and_invalidate);
	RUN(test_self_echo_and_loopback_discovery_vs_collision);
	RUN(test_alt_switch_preserves_clock_for_negotiation_window);
	RUN(test_discovery_window_3000ms_enforcement_and_post_window_followups);
	RUN(test_duplicate_pe_caps_after_nak_404_timeout);
	RUN(test_pe_after_endpoint_and_profile_nak);
	RUN(test_pe_caps_zero_simultaneous_cancels_queued_pe_get);
	RUN(test_v11_bare_13byte_nak_to_endpoint_and_pe_caps);
	RUN(test_pe_caps_zero_simultaneous_cancels_active_pe_get);
	RUN(test_v11_out_of_order_bare_nak_case_a);
	RUN(test_v11_out_of_order_bare_nak_case_b);
	usb_midi2_usb_reset();
	ump_stream_set_protocol_hook(0);
}
