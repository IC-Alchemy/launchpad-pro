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
                                                 std::vector<m2s::UmpPacket>* rawMt3 = nullptr,
                                                 int passes = 16) {
	std::vector<m2s::UmpPacket> allPkts;
	for (int i = 0; i < passes; ++i) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		for (const auto& p : pkts) {
			if (m2s::umpType(p) == 0x3u) {
				if (rawMt3) rawMt3->push_back(p);
				allPkts.push_back(p);
			}
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

std::vector<uint8_t> make_endpoint_reply(uint32_t srcMuid, uint32_t dstMuid,
                                         const std::string& piid) {
	std::vector<uint8_t> buf(ms::EndpointReplyFixedByteCount + piid.size(), 0u);
	const size_t n = ms::BuildEndpointReply(srcMuid, dstMuid, 0x00u,
	                                          reinterpret_cast<const uint8_t*>(piid.data()),
	                                          static_cast<uint16_t>(piid.size()), buf.data(),
	                                          buf.size());
	buf.resize(n);
	return buf;
}

std::vector<uint8_t> make_profile_reply(uint32_t srcMuid, uint32_t dstMuid) {
	const uint8_t enabled[5] = {0x7Eu, 0x00u, 0x01u, 0x00u, 0x01u};
	std::vector<uint8_t> buf(64u, 0u);
	const size_t n = ms::BuildProfileInquiryReply(ms::DeviceIdFunctionBlock, srcMuid, dstMuid,
	                                              enabled, 1u, nullptr, 0u, buf.data(), buf.size());
	buf.resize(n);
	return buf;
}

std::vector<uint8_t> make_pe_caps_reply(uint32_t srcMuid, uint32_t dstMuid,
                                        uint8_t simultaneous = 1u) {
	std::vector<uint8_t> buf(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	const size_t n = ms::BuildPropertyExchangeCapabilitiesReply(srcMuid, dstMuid, simultaneous,
	                                                            buf.data(), buf.size());
	buf.resize(n);
	return buf;
}

std::vector<uint8_t> make_pe_get_reply(uint32_t srcMuid, uint32_t dstMuid, uint8_t reqId,
                                       const std::string& hdr, const std::string& body) {
	ms::PropertyExchangeMessageFields f{};
	f.Type = ms::MessageType::PropertyGetDataReply;
	f.SourceMuid = srcMuid;
	f.DestinationMuid = dstMuid;
	f.RequestId = reqId;
	f.Header = reinterpret_cast<const uint8_t*>(hdr.data());
	f.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	f.ChunkCount = 1u;
	f.ChunkNumber = 1u;
	f.Data = body.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body.data());
	f.DataByteCount = static_cast<uint16_t>(body.size());
	std::vector<uint8_t> buf(ms::PropertyExchangeFixedByteCount + hdr.size() + body.size(), 0u);
	buf.resize(ms::BuildPropertyExchangeMessage(f, buf.data(), buf.size()));
	return buf;
}

struct LedSnapshot {
	std::array<std::array<uint8_t, 3>, 100> pads{};
};

LedSnapshot snapshot_leds() {
	LedSnapshot s;
	for (size_t i = 0; i < 100u; ++i) {
		s.pads[i][0] = e2e_leds[i][0];
		s.pads[i][1] = e2e_leds[i][1];
		s.pads[i][2] = e2e_leds[i][2];
	}
	return s;
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
// Tier 1 — F12: Initiator Boot/Negotiation Trigger (T1-F12-01..05)
// ============================================================================

#if E2E_HAS_M3

E2E_TEST(1, "T1-F12-01", "UMP Stream negotiation auto-emits Discovery broadcast with Launchpad identity (one-shot)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	if (msgs.size() == 1u) {
		const auto& m = msgs[0];
		E2E_CHECK_EQ(m.size(), ms::DiscoveryByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
		E2E_CHECK_EQ(p.DeviceId, ms::DeviceIdFunctionBlock);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		E2E_CHECK_EQ(p.DestinationMuid, ms::MuidBroadcast);
		if (m.size() >= 29u) {
			E2E_CHECK_EQ(m[13], static_cast<uint8_t>(CI_MANUFACTURER_0));
			E2E_CHECK_EQ(m[14], static_cast<uint8_t>(CI_MANUFACTURER_1));
			E2E_CHECK_EQ(m[15], static_cast<uint8_t>(CI_MANUFACTURER_2));
			E2E_CHECK_EQ(ms::ReadFourteenBitValue(&m[16]), static_cast<uint16_t>(CI_FAMILY));
			E2E_CHECK_EQ(ms::ReadFourteenBitValue(&m[18]), static_cast<uint16_t>(CI_MODEL));
			E2E_CHECK_EQ(ms::ReadMuid(&m[25]), 512u);
		}
	}

	fw.trigger_negotiated_discovery();
	E2E_CHECK(drain_ci_group(fw, 0).empty());
}

E2E_TEST(1, "T1-F12-02", "USB alt 0 suppresses automatic Discovery broadcast") {
	e2e::FirmwareNode fw;
	fw.boot(true, 0);
	fw.poll(10);
	fw.advance_ms(1000);
	E2E_CHECK_EQ(e2e_tx_count(), 0u);
}

E2E_TEST(1, "T1-F12-03", "Boot in alt 1 does not emit Discovery before full EP+FB UMP negotiation") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.poll(8);
	E2E_CHECK(drain_ci_group(fw, 0).empty());

	const auto epOnly = e2e::make_ep_discovery(0x1Fu);
	fw.send_ump(epOnly.data(), static_cast<uint16_t>(epOnly.size()));
	fw.poll(4);
	E2E_CHECK(drain_ci_group(fw, 0).empty());

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	if (msgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(msgs[0].data(), msgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}
}

E2E_TEST(1, "T1-F12-04", "Boot Discovery declares PE capability 0x08, sw_rev, OutputPathId=0, and midici_muid") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	if (msgs.size() == 1u && msgs[0].size() >= ms::DiscoveryByteCount) {
		const auto& m = msgs[0];
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK_EQ(p.SourceMuid, midici_muid());
		E2E_CHECK((m[24] & 0x08u) != 0u);
		E2E_CHECK_EQ(m[20], static_cast<uint8_t>(CI_SW_REV_0));
		E2E_CHECK_EQ(m[21], static_cast<uint8_t>(CI_SW_REV_1));
		E2E_CHECK_EQ(m[22], static_cast<uint8_t>(CI_SW_REV_2));
		E2E_CHECK_EQ(m[23], static_cast<uint8_t>(CI_SW_REV_3));
		E2E_CHECK_EQ(p.OutputPathId, 0x00u);
	}
}

E2E_TEST(1, "T1-F12-05", "Boot Discovery UMP SysEx7 packet framing is valid (MT3, group 0, Start->Continue->End, 7-bit)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	std::vector<m2s::UmpPacket> raw;
	const auto msgs = drain_ci_group(fw, 0, &raw);
	E2E_CHECK_EQ(msgs.size(), 1u);
	E2E_CHECK_EQ(raw.size(), 5u);
	for (size_t i = 0; i < raw.size(); ++i) {
		E2E_CHECK_EQ(m2s::umpType(raw[i]), 0x3u);
		E2E_CHECK_EQ(m2s::umpGroup(raw[i]), 0u);
		const uint8_t form = m2s::umpStatusNibble(raw[i]);
		if (i == 0u) E2E_CHECK_EQ(form, 1u);
		else if (i + 1u == raw.size()) E2E_CHECK_EQ(form, 3u);
		else E2E_CHECK_EQ(form, 2u);
	}
	if (msgs.size() == 1u) {
		for (uint8_t b : msgs[0]) {
			E2E_CHECK(b <= 0x7Fu);
			E2E_CHECK(b != 0xF0u && b != 0xF7u);
		}
	}

	const auto cfg = e2e::make_stream_config_req(UMP_PROTO_MIDI2, 0u);
	fw.send_ump(cfg.data(), static_cast<uint16_t>(cfg.size()));
	fw.poll(4);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
}

// ============================================================================
// Tier 1 — F13: Initiator Setup-Mode Pad Trigger (T1-F13-01..05)
// ============================================================================

E2E_TEST(1, "T1-F13-01", "Setup-mode BTN_MIDICI (89) and PAGE_MIDICI pad 11 trigger Discovery broadcast on group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)fw.drain_ump();

	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);

	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	const auto onEntry = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(onEntry.size(), 1u);
	if (onEntry.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(onEntry[0].data(), onEntry[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
		E2E_CHECK_EQ(p.DestinationMuid, ms::MuidBroadcast);
	}

	fw.tap_pad(11u, 20);
	const auto onPad11 = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(onPad11.size(), 1u);
	if (onPad11.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(onPad11[0].data(), onPad11[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
		E2E_CHECK_EQ(p.DestinationMuid, ms::MuidBroadcast);
	}
}

E2E_TEST(1, "T1-F13-02", "Existing occupied Setup controls (BTN_TUNING=97, PAGE_OPTIONS 31..34, 54) are undisturbed") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)fw.drain_ump();

	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);

	// Switch to Tuning (97), then Options (96) and toggle smoothing (34) + threshold (54)
	fw.tap_pad(97, 20);
	fw.tap_pad(96, 20);
	fw.tap_pad(34, 20);
	fw.tap_pad(54, 20);

	E2E_CHECK_EQ(ui_settings()->pressure_smoothing, static_cast<uint8_t>(SMOOTH_HEAVY));
	E2E_CHECK_EQ(ui_settings()->pressure_threshold, static_cast<uint8_t>(THRESH_INSTANT));
	E2E_CHECK(drain_ci_group(fw, 0).empty());
}

E2E_TEST(1, "T1-F13-03", "Pressing BTN_MIDICI on Play page (menu closed) does not emit Discovery or notes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)fw.drain_ump();

	E2E_CHECK_EQ(ui_menu_open(), 0u);
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	fw.poll(4);
	const auto pkts = fw.drain_tx_packets();
	E2E_CHECK(reassemble_all_ci(pkts, 0).empty());
	for (const auto& p : pkts) {
		E2E_CHECK(m2s::umpType(p) != 0x4u && m2s::umpType(p) != 0x2u);
	}
}

E2E_TEST(1, "T1-F13-04", "Manual Setup BTN_MIDICI and pad 11 re-trigger Discovery after boot 3s window closes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)fw.drain_ump();

	fw.trigger_negotiated_discovery();
	(void)drain_ci_group(fw, 0);
	fw.advance_ms(3200);
	(void)fw.drain_ump();

	fw.tap_setup(50);
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	E2E_CHECK_EQ(drain_ci_group(fw, 0).size(), 1u);

	fw.tap_pad(11u, 20);
	const auto msgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(msgs.size(), 1u);
	if (msgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(msgs[0].data(), msgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}
}

E2E_TEST(1, "T1-F13-05", "Opening Setup, triggering BTN_MIDICI, and closing Setup preserves settings and emits zero notes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)fw.drain_ump();

	const struct settings before = *ui_settings();
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 0u);

	const auto pkts = fw.drain_tx_packets();
	for (const auto& p : pkts) {
		E2E_CHECK(m2s::umpType(p) != 0x4u && m2s::umpType(p) != 0x2u);
	}
	E2E_CHECK_EQ(std::memcmp(&before, ui_settings(), sizeof(before)), 0);
}

// ============================================================================
// Tier 1 — F14: 3s Discovery Window & Peer Follow-Ups (T1-F14-01..05)
// ============================================================================

E2E_TEST(1, "T1-F14-01", "3-second Discovery window collects replies up to 3000 ms and ignores late replies") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	const auto disc = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(disc.size(), 1u);
	const uint32_t ourMuid = midici_muid();

	fw.advance_ms(1500);
	send_ci(fw, 0, make_discovery_reply(0x0101011u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.advance_ms(1600);  // total 3100 ms > 3000 ms
	send_ci(fw, 0, make_discovery_reply(0x0202022u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
}

E2E_TEST(1, "T1-F14-02", "Discovered peer receives EndpointInquiry (0x72) and stores ProductInstanceId") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111222u, ourMuid, 0x00u));
	const auto followUps = drain_ci_group(fw, 0);
	bool sawEndpoint = false;
	for (const auto& m : followUps) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::EndpointInquiry && p.DestinationMuid == 0x0111222u &&
		    p.HasEndpointFields && p.Endpoint.Status == 0x00u) {
			sawEndpoint = true;
		}
	}
	E2E_CHECK(sawEndpoint);

	send_ci(fw, 0, make_endpoint_reply(0x0111222u, ourMuid, "SYNTH-PIID-1"));
	(void)drain_ci_group(fw, 0);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK((info.flags & CI_PEER_FLAG_ENDPOINT) != 0u);
	E2E_CHECK(std::strcmp(info.product_instance_id, "SYNTH-PIID-1") == 0);
}

E2E_TEST(1, "T1-F14-03", "ProfileInquiry (0x20) is sent ONLY if peer declares 0x04 category") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	// Peer A (cat=0x00) -> EndpointInquiry only, no ProfileInquiry
	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	const auto fA = drain_ci_group(fw, 0);
	bool sawEpA = false;
	for (const auto& m : fA) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok) {
			if (p.Type == ms::MessageType::EndpointInquiry && p.DestinationMuid == 0x0111111u) {
				sawEpA = true;
			}
			E2E_CHECK(p.Type != ms::MessageType::ProfileInquiry);
		}
	}
	E2E_CHECK(sawEpA);

	// Peer B (cat=0x04) -> ProfileInquiry sent
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x04u));
	const auto fB = drain_ci_group(fw, 0);
	bool sawProf = false;
	for (const auto& m : fB) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::ProfileInquiry && p.DestinationMuid == 0x0222222u) {
			sawProf = true;
		}
	}
	E2E_CHECK(sawProf);

	send_ci(fw, 0, make_profile_reply(0x0222222u, ourMuid));
	(void)drain_ci_group(fw, 0);
	ci_peer_info_t pB{};
	E2E_CHECK(fw.get_peer(1, &pB));
	E2E_CHECK((pB.flags & CI_PEER_FLAG_PROFILES) != 0u);
}

E2E_TEST(1, "T1-F14-04", "PE peer (0x08) triggers PE Caps (0x30) then Get DeviceInfo (0x34)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0333333u, ourMuid, 0x08u, 512u));
	const auto f1 = drain_ci_group(fw, 0);
	bool sawEp = false, sawPeCaps = false, sawProf = false;
	for (const auto& m : f1) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok) continue;
		if (p.Type == ms::MessageType::EndpointInquiry) sawEp = true;
		if (p.Type == ms::MessageType::PropertyExchangeCapabilitiesInquiry) sawPeCaps = true;
		if (p.Type == ms::MessageType::ProfileInquiry) sawProf = true;
	}
	E2E_CHECK(sawEp);
	E2E_CHECK(sawPeCaps);
	E2E_CHECK(!sawProf);

	send_ci(fw, 0, make_pe_caps_reply(0x0333333u, ourMuid, 1u));
	const auto f2 = drain_ci_group(fw, 0);
	uint8_t devReqId = 0;
	bool sawGetDev = false;
	for (const auto& m : f2) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok) continue;
		if (p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			const std::string hdr(
			    reinterpret_cast<const char*>(m.data() + p.PropertyExchange.HeaderOffset),
			    p.PropertyExchange.HeaderByteCount);
			if (m2s::ci::ciJsonString(hdr, "resource") == "DeviceInfo") {
				sawGetDev = true;
				devReqId = p.PropertyExchange.RequestId;
			}
		}
	}
	E2E_CHECK(sawGetDev);

	send_ci(fw, 0, make_pe_get_reply(0x0333333u, ourMuid, devReqId, R"({"status":200})",
	                                 R"({"manufacturer":"Test"})"));
	(void)drain_ci_group(fw, 0);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK((info.flags & CI_PEER_FLAG_PE_CAPS) != 0u);
	E2E_CHECK((info.flags & CI_PEER_FLAG_DEVICE_INFO) != 0u);
}

E2E_TEST(1, "T1-F14-05", "Live WirePair against CiAgent populates peer table via follow-ups") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "M2S-F14-PEER";
	hostId.maxSysEx = 4096u;
	e2e::WirePair wire(fw, hostId, 0x0444444u);

	fw.trigger_negotiated_discovery();
	E2E_CHECK(wire.settle(256));

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK_EQ(info.muid, 0x0444444u);
	E2E_CHECK_EQ(info.manufacturer[0], 0x7Du);
	E2E_CHECK_EQ(info.max_sysex, 4096u);
	E2E_CHECK(std::strcmp(info.product_instance_id, "M2S-F14-PEER") == 0);
}

// ============================================================================
// Tier 1 — F15: Discovered Peers on Pad LEDs (T1-F15-01..05)
// ============================================================================

E2E_TEST(1, "T1-F15-01", "PAGE_MIDICI lights BTN_MIDICI and renders peer 0 on pad 21 while 22..28 stay off") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	E2E_CHECK(led_is(static_cast<uint8_t>(BTN_MIDICI), 63u, 63u, 63u));
	for (uint8_t pad = 21u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_endpoint_reply(0x0111111u, ourMuid, "PEER-1"));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(led_is(21u, 0u, 48u, 63u));
	for (uint8_t pad = 22u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

E2E_TEST(1, "T1-F15-02", "PAGE_MIDICI renders 3 distinct peers on row 2 pads 21..23 while 24..28 stay off") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	for (uint32_t i = 1u; i <= 3u; ++i) {
		send_ci(fw, 0, make_discovery_reply(0x0100000u + i, ourMuid, 0x00u));
	}
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 3u);

	E2E_CHECK(led_is(21u, 63u, 24u, 0u));
	E2E_CHECK(led_is(22u, 63u, 24u, 0u));
	E2E_CHECK(led_is(23u, 63u, 24u, 0u));
	for (uint8_t pad = 24u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

E2E_TEST(1, "T1-F15-03", "Row 2 peer LED color reflects completion/capability status across ui_poll and draw_all()") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	// Peer 0: PE-capable peer (0x08), partial -> amber {63,24,0}
	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x08u));
	(void)drain_ci_group(fw, 0);
	const std::array<uint8_t, 3> partialRgb = {e2e_leds[21][0], e2e_leds[21][1], e2e_leds[21][2]};
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));

	// Complete Endpoint + PE Caps + DeviceInfo -> bright green {0,63,0}
	send_ci(fw, 0, make_endpoint_reply(0x0111111u, ourMuid, "PE-PEER"));
	send_ci(fw, 0, make_pe_caps_reply(0x0111111u, ourMuid, 1u));
	const auto f2 = drain_ci_group(fw, 0);
	uint8_t reqId = 1u;
	for (const auto& m : f2) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			reqId = p.PropertyExchange.RequestId;
		}
	}
	send_ci(fw, 0, make_pe_get_reply(0x0111111u, ourMuid, reqId, R"({"status":200})",
	                                 R"({"manufacturer":"Test"})"));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK(led_is(21u, 0u, 63u, 0u));
	const std::array<uint8_t, 3> completeRgb = {e2e_leds[21][0], e2e_leds[21][1], e2e_leds[21][2]};
	E2E_CHECK(completeRgb != partialRgb);

	// Peer 1: non-PE peer (0x00), partial amber -> complete cyan {0,48,63}
	send_ci(fw, 0, make_discovery_reply(0x0222222u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK(led_is(22u, 63u, 24u, 0u));
	send_ci(fw, 0, make_endpoint_reply(0x0222222u, ourMuid, "NONPE-PEER"));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK(led_is(22u, 0u, 48u, 63u));

	// Close & reopen Setup -> draw_all() preserves peer capability colors
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 0u);
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	E2E_CHECK(led_is(21u, 0u, 63u, 0u));
	E2E_CHECK(led_is(22u, 0u, 48u, 63u));
	for (uint8_t pad = 23u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

E2E_TEST(1, "T1-F15-04", "Background peers do not overwrite Play-page LEDs; BTN_MIDICI restarts scan and renders 21..28") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const LedSnapshot playBase = snapshot_leds();

	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_discovery_reply(0x0111111u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK_EQ(ui_menu_open(), 0u);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(snapshot_leds().pads == playBase.pads);

	fw.tap_setup(50);
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	const auto freshDisc = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(freshDisc.size(), 1u);
	E2E_CHECK_EQ(fw.peer_count(), 0u);
	for (uint8_t pad = 21u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}

	send_ci(fw, 0, make_discovery_reply(0x0222222u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));
	for (uint8_t pad = 22u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

E2E_TEST(1, "T1-F15-05", "Closing Setup restores Play-page ROLE_ROOT LEDs and reopening Setup restores PAGE_MIDICI pad 21") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0111111u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);

	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 0u);
	E2E_CHECK(led_is(11u, 0u, 16u, 63u));

	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	E2E_CHECK(drain_ci_group(fw, 0).empty());
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(led_is(static_cast<uint8_t>(BTN_MIDICI), 63u, 63u, 63u));
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));
	for (uint8_t pad = 22u; pad <= 28u; ++pad) {
		E2E_CHECK(led_off(pad));
	}
}

#endif  // E2E_HAS_M3

// ============================================================================
// Tier 1 — F17: Host Back-to-Back CiAgent Verification (T1-F17-01..05)
// ============================================================================

#if E2E_HAS_M2

E2E_TEST(1, "T1-F17-01", "CiAgent discovers Launchpad with MUID, identity, v2, FB 0, maxSysEx 512, and PIID") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0123456u);

	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		const m2s::CiPeer& peer = snap.peers[0];
		E2E_CHECK(peer.answeredDiscovery);
		E2E_CHECK(ms::MuidIsUsable(peer.muid));
		E2E_CHECK_EQ(peer.version, 2u);
		E2E_CHECK_EQ(peer.functionBlock, 0u);
		E2E_CHECK_EQ(peer.maxSysEx, 512u);
		E2E_CHECK_EQ(peer.manufacturer[0], static_cast<uint8_t>(CI_MANUFACTURER_0));
		E2E_CHECK_EQ(peer.manufacturer[1], static_cast<uint8_t>(CI_MANUFACTURER_1));
		E2E_CHECK_EQ(peer.manufacturer[2], static_cast<uint8_t>(CI_MANUFACTURER_2));
		E2E_CHECK_EQ(peer.family, static_cast<uint16_t>(CI_FAMILY));
		E2E_CHECK_EQ(peer.model, static_cast<uint16_t>(CI_MODEL));
		E2E_CHECK(peer.hasEndpointInfo);
		E2E_CHECK(peer.productInstanceId == "TEST0001");
		E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "serialNumber") == peer.productInstanceId);
	}
}

E2E_TEST(1, "T1-F17-02", "CiAgent PE ResourceList 200 and DeviceInfo 200 at maxSysEx=128 (chunked) and 4096 (unchunked)") {
	for (uint32_t maxSysEx : {128u, 4096u}) {
		e2e::FirmwareNode fw;
		fw.boot(true, 1);
		m2s::ci::CiIdentity id;
		id.maxSysEx = maxSysEx;
		e2e::WirePair wire(fw, id, 0x0234567u);

		std::vector<m2s::UmpPacket> capturedFw;
		wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
			capturedFw.insert(capturedFw.end(), pkts.begin(), pkts.end());
		};
		std::vector<m2s::UmpPacket> capturedAgent;
		wire.tx_agent_to_fw_filter = [&](std::vector<uint32_t>& words) {
			for (size_t pos = 0; pos < words.size();) {
				const uint8_t wc =
				    m2s::umpWordCountForType(static_cast<uint8_t>(words[pos] >> 28));
				if (wc == 0 || pos + wc > words.size()) break;
				m2s::UmpPacket p{};
				for (uint8_t i = 0; i < wc; ++i) p.words[i] = words[pos + i];
				p.wordCount = wc;
				p.source = 10u;
				capturedAgent.push_back(p);
				pos += wc;
			}
		};

		E2E_CHECK(wire.discover_from_agent());
		const m2s::CiSnapshot snap = wire.agent().snapshot();
		E2E_CHECK_EQ(snap.peers.size(), 1u);
		if (snap.peers.size() == 1u) {
			const auto& peer = snap.peers[0];
			E2E_CHECK_EQ(peer.resourceListStatus, 200);
			E2E_CHECK_EQ(peer.deviceInfoStatus, 200);
			E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "manufacturer") == CI_MANUFACTURER_NAME);
			E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "family") == CI_FAMILY_NAME);
			E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "model") == CI_ENDPOINT_NAME);
			E2E_CHECK(peer.notes.empty());
		}

		uint8_t rlReqId = 0u, devInfoReqId = 0u;
		for (const auto& m : reassemble_all_ci(capturedAgent, 0)) {
			ms::ParsedMessage p{};
			if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
			    p.Type == ms::MessageType::PropertyGetDataInquiry &&
			    p.HasPropertyExchangeFields) {
				const std::string hdr(
				    reinterpret_cast<const char*>(m.data() + p.PropertyExchange.HeaderOffset),
				    p.PropertyExchange.HeaderByteCount);
				const std::string res = m2s::ci::ciJsonString(hdr, "resource");
				if (res == "ResourceList") rlReqId = p.PropertyExchange.RequestId;
				if (res == "DeviceInfo") devInfoReqId = p.PropertyExchange.RequestId;
			}
		}
		E2E_CHECK(rlReqId != 0u);
		E2E_CHECK(devInfoReqId != 0u);

		int rlChunks = 0, devInfoChunks = 0;
		uint16_t rlExpectedCount = 0u, devInfoExpectedCount = 0u;
		const size_t wireLimit = (maxSysEx == 128u) ? 128u : 320u;
		for (const auto& m : reassemble_all_ci(capturedFw, 0)) {
			ms::ParsedMessage p{};
			if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok ||
			    p.Type != ms::MessageType::PropertyGetDataReply ||
			    !p.HasPropertyExchangeFields) {
				continue;
			}
			if (p.PropertyExchange.RequestId == rlReqId) {
				++rlChunks;
				rlExpectedCount = p.PropertyExchange.ChunkCount;
				E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, static_cast<uint16_t>(rlChunks));
				E2E_CHECK(m.size() + 2u <= wireLimit);
			} else if (p.PropertyExchange.RequestId == devInfoReqId) {
				++devInfoChunks;
				devInfoExpectedCount = p.PropertyExchange.ChunkCount;
				E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, static_cast<uint16_t>(devInfoChunks));
				E2E_CHECK(m.size() + 2u <= wireLimit);
			}
		}
		E2E_CHECK(rlChunks > 1);
		E2E_CHECK_EQ(rlChunks, static_cast<int>(rlExpectedCount));
		if (maxSysEx == 128u) {
			E2E_CHECK_EQ(devInfoChunks, 3);
			E2E_CHECK_EQ(devInfoExpectedCount, 3u);
		} else {
			E2E_CHECK_EQ(devInfoChunks, 1);
			E2E_CHECK_EQ(devInfoExpectedCount, 1u);
		}
	}
}

E2E_TEST(1, "T1-F17-03", "CiAgent back-to-back PE 404 (unknown resource) and 405 (read-only Subscription)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0345678u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap0 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap0.peers.size(), 1u);
	if (snap0.peers.empty()) return;
	const uint32_t fwMuid = snap0.peers[0].muid;

	std::vector<m2s::UmpPacket> captured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		captured.insert(captured.end(), pkts.begin(), pkts.end());
	};

	auto sendPe = [&](ms::MessageType type, uint8_t reqId, const std::string& hdr) {
		ms::PropertyExchangeMessageFields f{};
		f.Type = type;
		f.SourceMuid = wire.agent().muid();
		f.DestinationMuid = fwMuid;
		f.RequestId = reqId;
		f.Header = reinterpret_cast<const uint8_t*>(hdr.data());
		f.HeaderByteCount = static_cast<uint16_t>(hdr.size());
		f.ChunkCount = 1u;
		f.ChunkNumber = 1u;
		uint8_t buf[128] = {};
		const size_t n = ms::BuildPropertyExchangeMessage(f, buf, sizeof(buf));
		wire.inject_sysex7_to_fw(0, buf, static_cast<uint16_t>(n));
	};

	sendPe(ms::MessageType::PropertyGetDataInquiry, 55u, R"({"resource":"X-DoesNotExist"})");
	sendPe(ms::MessageType::PropertySubscriptionInquiry, 56u,
	       R"({"command":"start","resource":"DeviceInfo"})");
	sendPe(ms::MessageType::PropertySubscriptionInquiry, 57u,
	       R"({"command":"start","resource":"ResourceList"})");
	E2E_CHECK(wire.settle(128));

	const auto replies = reassemble_all_ci(captured, 0);
	int got404 = 0, got405 = 0;
	for (const auto& m : replies) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok ||
		    !p.HasPropertyExchangeFields) {
			continue;
		}
		const std::string hdr(
		    reinterpret_cast<const char*>(m.data() + p.PropertyExchange.HeaderOffset),
		    p.PropertyExchange.HeaderByteCount);
		const int st = m2s::ci::ciJsonInt(hdr, "status", 0);
		if (p.Type == ms::MessageType::PropertyGetDataReply &&
		    p.PropertyExchange.RequestId == 55u && st == 404) {
			++got404;
		}
		if (p.Type == ms::MessageType::PropertySubscriptionReply &&
		    (p.PropertyExchange.RequestId == 56u || p.PropertyExchange.RequestId == 57u) &&
		    st == 405) {
			++got405;
		}
	}
	E2E_CHECK_EQ(got404, 1);
	E2E_CHECK_EQ(got405, 2);
}

E2E_TEST(1, "T1-F17-04", "InvalidateMuid MUID rotation and self-echo suppression against CiAgent") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x01112221u, 0x03334442u, 0x05556663u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);

	e2e::WireConfig cfg;
	cfg.agent_muid = 0x0456789u;
	cfg.self_echo_to_fw = true;
	cfg.self_echo_to_agent = true;
	e2e::WirePair wire(fw, cfg);

	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap1 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap1.peers.size(), 1u);
	if (snap1.peers.empty()) return;
	const uint32_t oldMuid = snap1.peers[0].muid;
	E2E_CHECK_EQ(snap1.peers[0].deviceInfoStatus, 200);

	// Invalidate Launchpad's MUID, settle, then re-discover
	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t n = ms::BuildInvalidateMuid(wire.agent().muid(), oldMuid, inv, sizeof(inv));
	wire.inject_sysex7_to_fw(0, inv, static_cast<uint16_t>(n));
	E2E_CHECK(wire.settle(128));
	E2E_CHECK(wire.discover_from_agent());

	const m2s::CiSnapshot snap2 = wire.agent().snapshot();
	bool foundNew = false;
	uint32_t liveFwMuid = 0u;
	for (const auto& p : snap2.peers) {
		if (p.muid != oldMuid && p.answeredDiscovery && p.deviceInfoStatus == 200) {
			foundNew = true;
			liveFwMuid = p.muid;
		}
	}
	E2E_CHECK(foundNew);
	E2E_CHECK(liveFwMuid != 0u);
	if (liveFwMuid != 0u) {
		std::vector<m2s::UmpPacket> echoCaptured;
		wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
			echoCaptured.insert(echoCaptured.end(), pkts.begin(), pkts.end());
		};
		uint8_t epSelf[64] = {};
		const size_t epN = ms::BuildEndpointInquiry(liveFwMuid, liveFwMuid,
		                                            ms::EndpointStatusProductInstanceId,
		                                            epSelf, sizeof(epSelf));
		wire.echo_to_fw(0, epSelf, static_cast<uint16_t>(epN));
		uint8_t invSelf[ms::InvalidateMuidByteCount] = {};
		const size_t invN = ms::BuildInvalidateMuid(liveFwMuid, liveFwMuid, invSelf, sizeof(invSelf));
		wire.echo_to_fw(0, invSelf, static_cast<uint16_t>(invN));
		E2E_CHECK(wire.settle(128));
		wire.tx_fw_to_agent_filter = nullptr;
		E2E_CHECK(reassemble_all_ci(echoCaptured, 0).empty());
		E2E_CHECK_EQ(midici_muid(), liveFwMuid);
	}
}

#endif  // E2E_HAS_M2

#if E2E_HAS_M2 && E2E_HAS_M3

E2E_TEST(1, "T1-F17-05", "Mutual peer listing: CiAgent lists Launchpad and Launchpad lists CiAgent") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "M2S-MUTUAL-01";
	e2e::WirePair wire(fw, hostId, 0x056789Au);

	fw.trigger_negotiated_discovery();
	E2E_CHECK(wire.discover_from_agent());

	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK(snap.peers[0].answeredDiscovery);
		E2E_CHECK(snap.peers[0].discoveredUs);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t peer{};
	E2E_CHECK(fw.get_peer(0, &peer));
	E2E_CHECK_EQ(peer.muid, 0x056789Au);
	E2E_CHECK(std::strcmp(peer.product_instance_id, "M2S-MUTUAL-01") == 0);
}

#endif  // E2E_HAS_M2 && E2E_HAS_M3
