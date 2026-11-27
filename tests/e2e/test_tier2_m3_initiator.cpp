#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump.h"

extern "C" {
#if E2E_HAS_M2 && !E2E_HAS_M1 && !E2E_HAS_M3
#include "midici/midici.h"
#endif
#include "instrument/settings.h"
#include "instrument/ui.h"
#include "midici/ci_identity.h"
#include "midici/ci_pe.h"
#include "platform/platform.h"
#include "ump/ump.h"
#include "usb/usb_midi2.h"
}

#ifndef BTN_MIDICI
#define BTN_MIDICI 89u
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ms = m2s::ci::ms;

#if E2E_HAS_M2 || E2E_HAS_M3
std::vector<std::vector<uint8_t>> reassemble_all_ci(const std::vector<m2s::UmpPacket>& pkts,
                                                    uint8_t group) {
	std::vector<std::vector<uint8_t>> msgs;
	m2s::ci::SysEx7Assembler asmblr;
	for (const auto& p : pkts) {
		if (m2s::umpType(p) != 0x3u || m2s::umpGroup(p) != (group & 0xFu)) continue;
		std::vector<uint8_t> msg;
		if (asmblr.feed(p, msg)) {
			msgs.push_back(std::move(msg));
		}
	}
	return msgs;
}
#endif

#if E2E_HAS_M3
void send_ci(e2e::FirmwareNode& fw, uint8_t group, const std::vector<uint8_t>& payload) {
	const std::vector<uint32_t> words = e2e::pack_sysex7_words(group, payload);
	fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
}

std::vector<std::vector<uint8_t>> drain_ci_group(e2e::FirmwareNode& fw, uint8_t group,
                                                 int passes = 16) {
	std::vector<m2s::UmpPacket> allPkts;
	for (int i = 0; i < passes; ++i) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		for (const auto& p : pkts) {
			if (m2s::umpType(p) == 0x3u) allPkts.push_back(p);
		}
	}
	return reassemble_all_ci(allPkts, group);
}

std::vector<uint8_t> make_discovery_reply(uint32_t srcMuid, uint32_t dstMuid, uint8_t categories,
                                          uint32_t maxSysEx = 512u, uint8_t fb = 0x00u,
                                          uint8_t ver = 0x02u) {
	ms::DiscoveryReplyFields f{};
	f.SourceMuid = srcMuid;
	f.DestinationMuid = dstMuid;
	f.ManufacturerSysExId[0] = 0x7Du;
	f.DeviceFamily = 0x0001u;
	f.DeviceFamilyModelNumber = 0x0002u;
	f.SoftwareRevisionLevel[1] = 0x01u;
	f.CapabilityCategories = categories;
	f.ReceivableMaximumSysExSize = maxSysEx;
	f.OutputPathId = 0x00u;
	f.FunctionBlockNumber = fb;
	f.MessageVersion = ver;
	std::vector<uint8_t> buf(ms::DiscoveryReplyByteCount, 0u);
	buf.resize(ms::BuildDiscoveryReply(f, buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_pe_caps_reply(uint32_t srcMuid, uint32_t dstMuid,
                                        uint8_t simultaneous = 1u) {
	std::vector<uint8_t> buf(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	buf.resize(ms::BuildPropertyExchangeCapabilitiesReply(srcMuid, dstMuid, simultaneous,
	                                                      buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_pe_get_chunk(uint32_t srcMuid, uint32_t dstMuid, uint8_t reqId,
                                       const std::string& hdr, uint16_t chunkCount,
                                       uint16_t chunkNum, const std::string& body) {
	ms::PropertyExchangeMessageFields f{};
	f.Type = ms::MessageType::PropertyGetDataReply;
	f.SourceMuid = srcMuid;
	f.DestinationMuid = dstMuid;
	f.RequestId = reqId;
	f.Header = hdr.empty() ? nullptr : reinterpret_cast<const uint8_t*>(hdr.data());
	f.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	f.ChunkCount = chunkCount;
	f.ChunkNumber = chunkNum;
	f.Data = body.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body.data());
	f.DataByteCount = static_cast<uint16_t>(body.size());
	std::vector<uint8_t> buf(ms::PropertyExchangeFixedByteCount + hdr.size() + body.size(), 0u);
	buf.resize(ms::BuildPropertyExchangeMessage(f, buf.data(), buf.size()));
	return buf;
}

std::vector<std::vector<uint8_t>> trigger_midici_setup_discovery(e2e::FirmwareNode& fw) {
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	auto msgs = drain_ci_group(fw, 0);
	if (msgs.empty()) {
		fw.tap_pad(11u, 20);
		msgs = drain_ci_group(fw, 0);
	}
	return msgs;
}

bool led_nonzero(uint8_t idx) {
	return e2e_leds[idx][0] != 0u || e2e_leds[idx][1] != 0u || e2e_leds[idx][2] != 0u;
}

bool led_off(uint8_t idx) {
	return !led_nonzero(idx);
}

bool led_is(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
	return e2e_leds[idx][0] == r && e2e_leds[idx][1] == g && e2e_leds[idx][2] == b;
}
#endif  // E2E_HAS_M3

}  // namespace

// ============================================================================
// Tier 2 — F12 Edge Cases (T2-F12-01..05)
// ============================================================================

#if E2E_HAS_M3

E2E_TEST(2, "T2-F12-01", "UMP Stream negotiation emits one Discovery; protocol switches emit 0; set_alt re-arms") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	const auto initMsgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(initMsgs.size(), 1u);
	if (initMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(initMsgs[0].data(), initMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}

	for (uint8_t proto : {static_cast<uint8_t>(UMP_PROTO_MIDI1),
	                      static_cast<uint8_t>(UMP_PROTO_MIDI2)}) {
		const auto cfg = e2e::make_stream_config_req(proto, 0u);
		fw.send_ump(cfg.data(), static_cast<uint16_t>(cfg.size()));
		fw.poll(4);
		fw.trigger_negotiated_discovery();
		E2E_CHECK_EQ(ump_stream_protocol(), proto);
		E2E_CHECK(drain_ci_group(fw, 0).empty());
	}

	fw.set_alt(0);
	fw.poll(2);
	fw.set_alt(1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	const auto reMsgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(reMsgs.size(), 1u);
	if (reMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(reMsgs[0].data(), reMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}
}

E2E_TEST(2, "T2-F12-02", "set_alt(0) mid-discovery clears peers and pending follow-ups; alt 1 recovers") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x08u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.set_alt(0);
	fw.poll(2);
	E2E_CHECK_EQ(fw.peer_count(), 0u);

	fw.set_alt(1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
}

E2E_TEST(2, "T2-F12-03", "usb_midi2_usb_reset clears initiator peer table and preserves open Setup menu") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	usb_midi2_usb_reset();
	fw.poll(2);
	E2E_CHECK_EQ(midi2_usb_alt(), 0u);
	E2E_CHECK_EQ(fw.peer_count(), 0u);
	E2E_CHECK_EQ(ui_menu_open(), 1u);

	fw.set_alt(1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK_EQ(drain_ci_group(fw, 0).size(), 1u);
}

E2E_TEST(2, "T2-F12-04", "Pre-filled CiTxQueue handles trigger_negotiated_discovery cleanly without faults") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const std::vector<uint8_t> filler(120u, 0x33u);  // 20 pkts each
	for (int i = 0; i < 6; ++i) {
		E2E_CHECK_EQ(midici_send_sysex(1, filler.data(), static_cast<uint16_t>(filler.size())), 0);
	}
	const std::vector<uint8_t> filler4(24u, 0x33u);  // 4 pkts -> 124 used, 4 free (< 5)
	E2E_CHECK_EQ(midici_send_sysex(1, filler4.data(), static_cast<uint16_t>(filler4.size())), 0);
	E2E_CHECK(midici_tx_free_packets() < 5u);

	fw.trigger_negotiated_discovery();
	fw.poll(1);
	fw.poll(40);
	(void)fw.drain_ump();
	E2E_CHECK_EQ(midici_tx_pending(), 0u);
}

E2E_TEST(2, "T2-F12-05", "Boundary entropy {0, 0x0FFFFFFF, 0x0FFFFF00, 0x12345678} yields valid boot Discovery MUID") {
	e2e::FirmwareNode fw;
	const uint32_t ent[4] = {0u, 0x0FFFFFFFu, 0x0FFFFF00u, 0x12345678u};
	fw.set_entropy(ent, 4);
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	if (msgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(msgs[0].data(), msgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(ms::MuidIsUsable(p.SourceMuid));
	}
}

// ============================================================================
// Tier 2 — F13 Edge Cases (T2-F13-01..05)
// ============================================================================

E2E_TEST(2, "T2-F13-01", "Rapid re-trigger via BTN_MIDICI + pad 11 200 ms apart avoids duplicate peer MUID entries") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.advance_ms(200);
	fw.tap_pad(11u, 20);
	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
}

E2E_TEST(2, "T2-F13-02", "Setup peek >= 400 ms closes menu on release while 3s Discovery window remains active") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	fw.press_setup(127);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	fw.advance_ms(400);
	fw.release_setup();
	E2E_CHECK_EQ(ui_menu_open(), 0u);

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
}

E2E_TEST(2, "T2-F13-03", "Setup + BTN_MIDICI and pad 11 in USB alt 0 leak zero MT 0x3 packets") {
	e2e::FirmwareNode fw;
	fw.boot(true, 0);
	fw.tap_setup(50);
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	fw.tap_pad(11u, 20);
	fw.poll(6);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
	E2E_CHECK_EQ(e2e_tx_count(), 0u);
}

E2E_TEST(2, "T2-F13-04", "Page cycling (95 -> 89 -> 96 -> 97 -> 89) preserves settings/controls and restarts Discovery on BTN_MIDICI") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const struct settings baseline = *ui_settings();

	fw.tap_setup(50);
	fw.tap_pad(95u, 20);
	(void)trigger_midici_setup_discovery(fw);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.tap_pad(96u, 20);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
	E2E_CHECK(led_is(96u, 63u, 63u, 63u));
	for (uint8_t pad = 31u; pad <= 34u; ++pad) {
		E2E_CHECK(led_nonzero(pad));
	}
	E2E_CHECK(led_nonzero(54u));

	fw.tap_pad(97u, 20);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
	E2E_CHECK(led_is(97u, 63u, 63u, 63u));

	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	const auto restartMsgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(restartMsgs.size(), 1u);
	if (restartMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(restartMsgs[0].data(), restartMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}
	E2E_CHECK_EQ(std::memcmp(&baseline, ui_settings(), sizeof(baseline)), 0);
	E2E_CHECK_EQ(fw.peer_count(), 0u);

	send_ci(fw, 0, make_discovery_reply(0x0222222u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(led_nonzero(21u));
}

E2E_TEST(2, "T2-F13-05", "Release-only on BTN_MIDICI / pad 11 emits zero Discovery; single press+release emits one") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)fw.drain_ump();

	// Release without prior press
	fw.release_pad(static_cast<uint8_t>(BTN_MIDICI));
	fw.release_pad(11u);
	E2E_CHECK(drain_ci_group(fw, 0).empty());

	// Open PAGE_MIDICI via BTN_MIDICI, then verify press+release on pad 11 emits exactly one Discovery
	(void)trigger_midici_setup_discovery(fw);
	fw.press_pad(11u, 127);
	E2E_CHECK_EQ(drain_ci_group(fw, 0).size(), 1u);
	fw.release_pad(11u);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
}

// ============================================================================
// Tier 2 — F14 Edge Cases (T2-F14-01..05)
// ============================================================================

E2E_TEST(2, "T2-F14-01", "3s window exact boundary (2999 ms vs 3001 ms) and uint32_t wraparound") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	fw.advance_ms(2999);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.advance_ms(2);  // total 3001 ms
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	// uint32_t millisecond wraparound: 0xFFFFFC00 -> 0x00000200 (+1536ms ok) vs 0x00000800 (+3072ms closed)
	fw.boot(true, 1);
	midici_initiator_start_discovery(0xFFFFFC00u);
	// Drain emitted Discovery via USB TX path without invoking main_loop()/midici_tick(g_ms)
	std::vector<m2s::UmpPacket> wrapDiscPkts;
	for (int i = 0; i < 4; ++i) {
		if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
		__wrap_usbmidi_poll();
		const auto pkts = fw.drain_tx_packets();
		wrapDiscPkts.insert(wrapDiscPkts.end(), pkts.begin(), pkts.end());
	}
	E2E_CHECK_EQ(reassemble_all_ci(wrapDiscPkts, 0).size(), 1u);
	E2E_CHECK(midici_initiator_is_discovering());

	const uint32_t wrapMuid = midici_muid();
	const auto rep1 = make_discovery_reply(0x0333333u, wrapMuid, 0x00u);
	E2E_CHECK(midici_initiator_on_message(0, rep1.data(), static_cast<uint16_t>(rep1.size()),
	                                      0x00000200u));
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	midici_initiator_tick(0x00000800u);
	E2E_CHECK(!midici_initiator_is_discovering());
	const auto rep2 = make_discovery_reply(0x0444444u, wrapMuid, 0x00u);
	(void)midici_initiator_on_message(0, rep2.data(), static_cast<uint16_t>(rep2.size()),
	                                  0x00000800u);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
}

E2E_TEST(2, "T2-F14-02", "Peer table overflow (12 distinct peers) bounds count <= 8 with zero OOB") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	for (uint32_t i = 1u; i <= 12u; ++i) {
		send_ci(fw, 0, make_discovery_reply(0x0100000u + i, ourMuid, 0x00u));
		(void)drain_ci_group(fw, 0);
	}
	const uint8_t cnt = fw.peer_count();
	E2E_CHECK(cnt >= 1u && cnt <= 8u);
	ci_peer_info_t out{};
	E2E_CHECK(!fw.get_peer(cnt, &out));
	E2E_CHECK(!fw.get_peer(255u, &out));
}

E2E_TEST(2, "T2-F14-03", "v1.1 DiscoveryReply (29B), NAK 0x01 / 404 tolerance, and multi-chunk DeviceInfo") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	// Peer 1: v1.1 29B DiscoveryReply (cat=0x08) -> Endpoint NAK 0x01 & DeviceInfo 404
	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x08u, 512u, 0x00u, 0x01u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	uint8_t nak[64] = {};
	ms::AcknowledgmentFields af{};
	af.OriginalMessageType = static_cast<uint8_t>(ms::MessageType::EndpointInquiry);
	af.StatusCode = ms::NakStatusMessageNotSupported;
	const size_t nakN = ms::BuildAcknowledgment(ms::MessageType::Nak, ms::DeviceIdFunctionBlock,
	                                            0x0111111u, ourMuid, af, nullptr, 0u, nak,
	                                            sizeof(nak));
	send_ci(fw, 0, std::vector<uint8_t>(nak, nak + nakN));
	send_ci(fw, 0, make_pe_caps_reply(0x0111111u, ourMuid, 1u));
	const auto f1 = drain_ci_group(fw, 0);
	uint8_t req1 = 1u;
	for (const auto& m : f1) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			req1 = p.PropertyExchange.RequestId;
		}
	}
	send_ci(fw, 0, make_pe_get_chunk(0x0111111u, ourMuid, req1, R"({"status":404})", 1u, 1u, ""));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	// Peer 2: multi-chunk DeviceInfo
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x08u, 512u));
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_pe_caps_reply(0x0222222u, ourMuid, 1u));
	const auto f2 = drain_ci_group(fw, 0);
	uint8_t req2 = 2u;
	for (const auto& m : f2) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			req2 = p.PropertyExchange.RequestId;
		}
	}
	send_ci(fw, 0, make_pe_get_chunk(0x0222222u, ourMuid, req2, R"({"status":200})", 2u, 1u,
	                                 R"({"manufacturer":)"));
	send_ci(fw, 0, make_pe_get_chunk(0x0222222u, ourMuid, req2, "", 2u, 2u, R"("Ok"})"));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 2u);
}

E2E_TEST(2, "T2-F14-04", "Wrong DestinationMuid, wrong group (group 1), and self-echo DiscoveryReply all ignored") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111111u, 0x0999999u, 0x00u));  // wrong dest
	send_ci(fw, 1, make_discovery_reply(0x0222222u, ourMuid, 0x00u));     // wrong group
	send_ci(fw, 0, make_discovery_reply(ourMuid, ourMuid, 0x00u));        // self-echo
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 0u);
}

E2E_TEST(2, "T2-F14-05", "InvalidateMuid targeting one of 2 discovered peers removes only that peer") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 2u);

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t n = ms::BuildInvalidateMuid(0x0333333u, 0x0111111u, inv, sizeof(inv));
	send_ci(fw, 0, std::vector<uint8_t>(inv, inv + n));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t remain{};
	E2E_CHECK(fw.get_peer(0, &remain));
	E2E_CHECK_EQ(remain.muid, 0x0222222u);
}

// ============================================================================
// Tier 2 — F15 Edge Cases (T2-F15-01..05)
// ============================================================================

E2E_TEST(2, "T2-F15-01", "10 peers on PAGE_MIDICI light row 2 pads 21..28 (8 max) with e2e_leds[99] untouched") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	for (uint32_t i = 1u; i <= 10u; ++i) {
		send_ci(fw, 0, make_discovery_reply(0x0100000u + i, ourMuid, 0x00u));
		(void)drain_ci_group(fw, 0);
	}

	E2E_CHECK(led_off(99u));
	E2E_CHECK_EQ(fw.peer_count(), 8u);
	int lit = 0;
	for (uint8_t pad = 21u; pad <= 28u; ++pad) {
		if (led_nonzero(pad)) ++lit;
	}
	E2E_CHECK_EQ(lit, 8);
}

E2E_TEST(2, "T2-F15-02", "InvalidateMuid on PAGE_MIDICI clears row 2 pad 21 back to off on refresh") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t n = ms::BuildInvalidateMuid(0x0222222u, 0x0111111u, inv, sizeof(inv));
	send_ci(fw, 0, std::vector<uint8_t>(inv, inv + n));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 0u);

	for (uint8_t pad = 21u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

E2E_TEST(2, "T2-F15-03", "set_alt(0) while PAGE_MIDICI open turns Setup LED amber {63,20,0}, clears peers, keeps menu open") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.set_alt(0);
	fw.poll(2);
	E2E_CHECK_EQ(fw.peer_count(), 0u);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	E2E_CHECK_EQ(e2e_setup_led[0], 63u);
	E2E_CHECK_EQ(e2e_setup_led[1], 20u);
	E2E_CHECK_EQ(e2e_setup_led[2], 0u);
}

E2E_TEST(2, "T2-F15-04", "Play-mode pad 11 lights white when sounding and reverts to its root colour with peers active") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK_EQ(ui_menu_open(), 0u);

	const uint8_t ch = ui_settings()->channel;
	ui_on_sounding(60, ch, 1);
	E2E_CHECK(led_is(11u, 63u, 63u, 63u));

	ui_on_sounding(60, ch, 0);
	E2E_CHECK(led_is(11u, 0u, 16u, 63u));
}

E2E_TEST(2, "T2-F15-05", "get_peer NULL / out-of-range guards consistent with row 2 pads 21..28 LED count") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	E2E_CHECK(!fw.get_peer(0, nullptr));
	ci_peer_info_t p{};
	E2E_CHECK(!fw.get_peer(0, &p));
	E2E_CHECK(!fw.get_peer(255u, &p));

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK(!fw.get_peer(0, nullptr));
	E2E_CHECK(!fw.get_peer(255u, &p));
	E2E_CHECK_EQ(fw.peer_count(), 2u);
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));
	E2E_CHECK(led_is(22u, 63u, 24u, 0u));
	for (uint8_t pad = 23u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

#endif  // E2E_HAS_M3

// ============================================================================
// Tier 2 — F17 Edge Cases (T2-F17-01..05)
// ============================================================================

#if E2E_HAS_M2

E2E_TEST(2, "T2-F17-01", "Crafted 0x3FFF lengths & truncated Discovery: NAK 0x41, CiAgent malformed++, zero ASan faults") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0611111u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap0 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap0.peers.size(), 1u);
	if (snap0.peers.empty()) return;
	const uint32_t fwMuid = snap0.peers[0].muid;

	std::vector<m2s::UmpPacket> captured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		captured.insert(captured.end(), pkts.begin(), pkts.end());
	};

	// Inject crafted 0x3FFF lengths + truncated Discovery into firmware
	std::vector<uint8_t> badPeHdr(ms::PropertyExchangeFixedByteCount + 4u, 0u);
	ms::WriteCommonHeader(badPeHdr.data(), badPeHdr.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataInquiry, wire.agent().muid(), fwMuid);
	badPeHdr[13] = 1u;
	ms::WriteFourteenBitValue(&badPeHdr[14], 0x3FFFu);
	wire.inject_sysex7_to_fw(0, badPeHdr.data(), static_cast<uint16_t>(badPeHdr.size()));

	std::vector<uint8_t> truncDisc(18u, 0u);
	ms::WriteCommonHeader(truncDisc.data(), truncDisc.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::Discovery, wire.agent().muid(), ms::MuidBroadcast);
	wire.inject_sysex7_to_fw(0, truncDisc.data(), static_cast<uint16_t>(truncDisc.size()));

	// Inject crafted 0x3FFF lengths into CiAgent -> malformed increments without fault
	const uint64_t malBefore = wire.agent().snapshot().malformed;
	std::vector<uint8_t> badEp(ms::EndpointReplyFixedByteCount + 2u, 0u);
	ms::WriteCommonHeader(badEp.data(), badEp.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::EndpointReply, fwMuid, wire.agent().muid());
	ms::WriteFourteenBitValue(&badEp[14], 0x3FFFu);
	wire.inject_sysex7_to_agent(0, badEp.data(), static_cast<uint16_t>(badEp.size()));

	std::vector<uint8_t> badProf(ms::CommonHeaderByteCount + 4u, 0u);
	ms::WriteCommonHeader(badProf.data(), badProf.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::ProfileInquiryReply, fwMuid, wire.agent().muid());
	ms::WriteFourteenBitValue(&badProf[13], 0x3FFFu);
	wire.inject_sysex7_to_agent(0, badProf.data(), static_cast<uint16_t>(badProf.size()));

	E2E_CHECK(wire.settle(128));
	wire.tx_fw_to_agent_filter = nullptr;
	int nakCount = 0;
	bool sawPeNak = false, sawDiscNak = false;
	for (const auto& m : reassemble_all_ci(captured, 0)) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::Nak && p.HasAcknowledgmentFields &&
		    p.Acknowledgment.StatusCode == ms::NakStatusMessageMalformed) {
			++nakCount;
			if (p.Acknowledgment.OriginalMessageType ==
			    static_cast<uint8_t>(ms::MessageType::PropertyGetDataInquiry)) {
				sawPeNak = true;
			}
			if (p.Acknowledgment.OriginalMessageType ==
			    static_cast<uint8_t>(ms::MessageType::Discovery)) {
				sawDiscNak = true;
			}
		}
	}
	E2E_CHECK_EQ(nakCount, 2);
	E2E_CHECK(sawPeNak);
	E2E_CHECK(sawDiscNak);
	const m2s::CiSnapshot snap1 = wire.agent().snapshot();
	E2E_CHECK(snap1.malformed >= malBefore + 2u);
	E2E_CHECK(wire.discover_from_agent());
}

E2E_TEST(2, "T2-F17-02", "MUID collision emits InvalidateMuid(new, old) and InvalidateMuid mid-128B ResourceList aborts") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x02221111u, 0x03332222u, 0x04443333u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);

	m2s::ci::CiIdentity id;
	id.maxSysEx = 128u;
	e2e::WirePair wire(fw, id, 0x0622222u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap0 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap0.peers.size(), 1u);
	if (snap0.peers.empty()) return;
	const uint32_t oldMuid = snap0.peers[0].muid;

	// Colliding Discovery (SourceMuid == oldMuid) -> InvalidateMuid(new, old)
	std::vector<uint8_t> col(ms::DiscoveryByteCount, 0u);
	ms::DiscoveryReplyFields df{};
	df.SourceMuid = oldMuid;
	df.DestinationMuid = ms::MuidBroadcast;
	df.CapabilityCategories = 0x08u;
	df.ReceivableMaximumSysExSize = 128u;
	ms::BuildDiscovery(df, col.data(), col.size());
	wire.inject_sysex7_to_fw(0, col.data(), static_cast<uint16_t>(col.size()));
	E2E_CHECK(wire.settle(128));

	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap1 = wire.agent().snapshot();
	uint32_t liveMuid = 0;
	for (const auto& p : snap1.peers) {
		if (p.muid != oldMuid && p.answeredDiscovery && p.deviceInfoStatus == 200) {
			liveMuid = p.muid;
		}
	}
	E2E_CHECK(liveMuid != 0u);

	// Start 128B chunked GET ResourceList, let chunk 1 emit, then InvalidateMuid mid-stream
	const std::string hdr = R"({"resource":"ResourceList"})";
	ms::PropertyExchangeMessageFields pf{};
	pf.Type = ms::MessageType::PropertyGetDataInquiry;
	pf.SourceMuid = wire.agent().muid();
	pf.DestinationMuid = liveMuid;
	pf.RequestId = 99u;
	pf.Header = reinterpret_cast<const uint8_t*>(hdr.data());
	pf.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	pf.ChunkCount = 1u;
	pf.ChunkNumber = 1u;
	uint8_t peBuf[128] = {};
	const size_t peN = ms::BuildPropertyExchangeMessage(pf, peBuf, sizeof(peBuf));
	wire.inject_sysex7_to_fw(0, peBuf, static_cast<uint16_t>(peN));
	wire.pump();
	wire.pump();

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(wire.agent().muid(), liveMuid, inv, sizeof(inv));
	wire.inject_sysex7_to_fw(0, inv, static_cast<uint16_t>(invN));
	E2E_CHECK(wire.settle(128));
	E2E_CHECK_EQ(midici_tx_pending(), 0u);
}

E2E_TEST(2, "T2-F17-03", "Live self-echoes plus unsupported 0x40/0x42 inquiries -> NAK 0x01 and DeviceInfo 200") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	e2e::WireConfig cfg;
	cfg.agent_muid = 0x0633333u;
	cfg.self_echo_to_fw = true;
	cfg.self_echo_to_agent = true;
	e2e::WirePair wire(fw, cfg);

	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap0 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap0.peers.size(), 1u);
	if (snap0.peers.empty()) return;
	const uint32_t fwMuid = snap0.peers[0].muid;

	std::vector<m2s::UmpPacket> captured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		captured.insert(captured.end(), pkts.begin(), pkts.end());
	};

	uint8_t proc[ms::ProcessInquiryCapabilitiesByteCount] = {};
	ms::BuildProcessInquiryCapabilities(wire.agent().muid(), fwMuid, proc, sizeof(proc));
	wire.inject_sysex7_to_fw(0, proc, sizeof(proc));

	uint8_t mmr[ms::MidiMessageReportByteCount] = {};
	ms::MidiMessageReportFields mf{};
	ms::BuildMidiMessageReport(ms::DeviceIdFunctionBlock, wire.agent().muid(), fwMuid, mf, mmr,
	                           sizeof(mmr));
	wire.inject_sysex7_to_fw(0, mmr, sizeof(mmr));

	E2E_CHECK(wire.settle(128));
	const auto replies = reassemble_all_ci(captured, 0);
	int nakCount = 0;
	for (const auto& m : replies) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::Nak && p.HasAcknowledgmentFields &&
		    p.Acknowledgment.StatusCode == ms::NakStatusMessageNotSupported) {
			++nakCount;
		}
	}
	E2E_CHECK_EQ(nakCount, 2);
	const m2s::CiSnapshot snap1 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap1.peers.size(), 1u);
	if (snap1.peers.size() == 1u) {
		E2E_CHECK_EQ(snap1.peers[0].deviceInfoStatus, 200);
	}
}

#endif  // E2E_HAS_M2

#if E2E_HAS_M2 && E2E_HAS_M3

E2E_TEST(2, "T2-F17-04", "Mutual discovery with CiAgent maxSysEx=128 + resetTransport() + re-discovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	m2s::ci::CiIdentity hostId;
	hostId.maxSysEx = 128u;
	hostId.productInstanceId = "M2S-128-RE";
	e2e::WirePair wire(fw, hostId, 0x0644444u);

	fw.trigger_negotiated_discovery();
	E2E_CHECK(wire.discover_from_agent());
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK_EQ(wire.agent().snapshot().peers.size(), 1u);

	wire.agent().resetTransport();
	fw.start_discovery();
	E2E_CHECK(wire.discover_from_agent());
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK_EQ(wire.agent().snapshot().peers.size(), 1u);
	if (wire.agent().snapshot().peers.size() == 1u) {
		E2E_CHECK_EQ(wire.agent().snapshot().peers[0].deviceInfoStatus, 200);
	}
}

E2E_TEST(2, "T2-F17-05", "CiAgent maxSysEx=64 (< 128 fallback) yields DeviceInfo 200 and Initiator tolerates 404/0x3FFF") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	m2s::ci::CiIdentity hostId;
	hostId.maxSysEx = 64u;
	hostId.productInstanceId = "M2S-CLAMP64";
	e2e::WirePair wire(fw, hostId, 0x0655555u);

	fw.trigger_negotiated_discovery();
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	// Also verify Initiator tolerates malformed DataByteCount=0x3FFF in a peer PropertyGetDataReply
	std::vector<uint8_t> badReply(ms::PropertyExchangeFixedByteCount + 16u, 0u);
	ms::WriteCommonHeader(badReply.data(), badReply.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataReply, 0x0655555u, midici_muid());
	badReply[13] = 1u;
	ms::WriteFourteenBitValue(&badReply[16], 1u);
	ms::WriteFourteenBitValue(&badReply[18], 1u);
	ms::WriteFourteenBitValue(&badReply[20], 0x3FFFu);
	wire.inject_sysex7_to_fw(0, badReply.data(), static_cast<uint16_t>(badReply.size()));
	E2E_CHECK(wire.settle(64));
	E2E_CHECK_EQ(fw.peer_count(), 1u);
}

#endif  // E2E_HAS_M2 && E2E_HAS_M3
