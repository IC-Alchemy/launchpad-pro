#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump.h"

extern "C" {
#if !E2E_HAS_M1 && !E2E_HAS_M3
#include "midici/midici.h"
#endif
#include "instrument/settings.h"
#include "instrument/ui.h"
#include "midici/ci_identity.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "midici/responder.h"
#include "pads/pad_velocity.h"
#include "platform/pads.h"
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

#if E2E_HAS_M2
namespace ms = m2s::ci::ms;

std::vector<std::vector<uint8_t>> reassemble_all_ci(const std::vector<m2s::UmpPacket>& pkts,
                                                    uint8_t group) {
	std::vector<std::vector<uint8_t>> msgs;
	m2s::ci::SysEx7Assembler asmblr;
	for (const auto& p : pkts) {
		if (m2s::umpType(p) != 0x3u || m2s::umpGroup(p) != (group & 0xFu)) continue;
		std::vector<uint8_t> msg;
		if (asmblr.feed(p, msg)) msgs.push_back(std::move(msg));
	}
	return msgs;
}
#endif

#if E2E_HAS_M2 && E2E_HAS_M3
std::vector<uint8_t> make_discovery_reply(uint32_t srcMuid, uint32_t dstMuid, uint8_t categories,
                                          uint32_t maxSysEx = 512u, uint8_t fb = 0x00u) {
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
	f.MessageVersion = ms::MessageVersionCurrent;
	std::vector<uint8_t> buf(ms::DiscoveryReplyByteCount, 0u);
	buf.resize(ms::BuildDiscoveryReply(f, buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_endpoint_reply(uint32_t srcMuid, uint32_t dstMuid,
                                         const std::string& piid) {
	std::vector<uint8_t> buf(ms::EndpointReplyFixedByteCount + piid.size(), 0u);
	buf.resize(ms::BuildEndpointReply(srcMuid, dstMuid, 0x00u,
	                                  reinterpret_cast<const uint8_t*>(piid.data()),
	                                  static_cast<uint16_t>(piid.size()), buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_profile_reply(uint32_t srcMuid, uint32_t dstMuid) {
	const uint8_t enabled[5] = {0x7Eu, 0x00u, 0x01u, 0x00u, 0x01u};
	std::vector<uint8_t> buf(64u, 0u);
	buf.resize(ms::BuildProfileInquiryReply(ms::DeviceIdFunctionBlock, srcMuid, dstMuid, enabled,
	                                        1u, nullptr, 0u, buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_pe_caps_reply(uint32_t srcMuid, uint32_t dstMuid,
                                        uint8_t simultaneous = 1u) {
	std::vector<uint8_t> buf(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	buf.resize(ms::BuildPropertyExchangeCapabilitiesReply(srcMuid, dstMuid, simultaneous,
	                                                      buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_pe_get_reply(uint32_t srcMuid, uint32_t dstMuid, uint8_t reqId,
                                       const std::string& hdr, const std::string& body) {
	ms::PropertyExchangeMessageFields f{};
	f.Type = ms::MessageType::PropertyGetDataReply;
	f.SourceMuid = srcMuid;
	f.DestinationMuid = dstMuid;
	f.RequestId = reqId;
	f.Header = hdr.empty() ? nullptr : reinterpret_cast<const uint8_t*>(hdr.data());
	f.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	f.ChunkCount = 1u;
	f.ChunkNumber = 1u;
	f.Data = body.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body.data());
	f.DataByteCount = static_cast<uint16_t>(body.size());
	std::vector<uint8_t> buf(ms::PropertyExchangeFixedByteCount + hdr.size() + body.size(), 0u);
	buf.resize(ms::BuildPropertyExchangeMessage(f, buf.data(), buf.size()));
	return buf;
}

bool led_is(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
	return e2e_leds[idx][0] == r && e2e_leds[idx][1] == g && e2e_leds[idx][2] == b;
}

bool setup_led_is(uint8_t r, uint8_t g, uint8_t b) {
	return e2e_setup_led[0] == r && e2e_setup_led[1] == g && e2e_setup_led[2] == b;
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2 && E2E_HAS_M3
void arm_pad_adc(uint8_t pad, uint16_t adcMax10, uint16_t raw12) {
	const uint8_t slot = pad_to_adcmax_slot(pad);
	const uint8_t rawIdx = pad_raw_index(pad);
	if (slot != PAD_NO_SLOT) ADC_Max[slot] = adcMax10;
	if (rawIdx != PAD_NO_SLOT) e2e_adc[rawIdx] = raw12;
}
#endif

}  // namespace

// ============================================================================
// Tier 4 — Real-World Application Scenarios (T4-01..T4-07)
// ============================================================================

#if E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(4, "T4-01", "Cold-boot USB enumeration -> UMP Stream negotiation -> mutual peer discovery and identity match") {
	e2e::FirmwareNode fw;
	fw.set_serial("00112233445566778899AABB");
	fw.boot(true, 1);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "M2S-T4-01";
	hostId.maxSysEx = 4096u;
	e2e::WirePair wire(fw, hostId, 0x0140101u);

	std::vector<m2s::UmpPacket> streamCaptured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		for (const auto& p : pkts) {
			if (m2s::umpType(p) == 0xFu) streamCaptured.push_back(p);
		}
	};

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK(wire.discover_from_agent());
	E2E_CHECK(wire.settle(256));
	wire.tx_fw_to_agent_filter = nullptr;

	bool sawFb0 = false, sawFb1 = false, sawIdentity = false;
	std::string stProductId;
	for (const auto& p : streamCaptured) {
		if (p.wordCount != 4u) continue;
		const uint16_t st = static_cast<uint16_t>((p.words[0] >> 16) & 0x3FFu);
		if (st == 0x011u) {
			const uint8_t fb = static_cast<uint8_t>((p.words[0] >> 8) & 0x7Fu);
			if (fb == 0u && p.words[1] == 0x00010200u) sawFb0 = true;
			if (fb == 1u && p.words[1] == 0x01010000u) sawFb1 = true;
		} else if (st == 0x002u) {
			sawIdentity = (p.words[1] == CI_UMP_IDENTITY_WORD1 &&
			               p.words[2] == CI_UMP_IDENTITY_WORD2 &&
			               p.words[3] == CI_UMP_IDENTITY_WORD3);
		} else if (st == 0x004u) {
			const uint8_t c0 = static_cast<uint8_t>((p.words[0] >> 8) & 0xFFu);
			const uint8_t c1 = static_cast<uint8_t>(p.words[0] & 0xFFu);
			if (c0) stProductId.push_back(static_cast<char>(c0));
			if (c1) stProductId.push_back(static_cast<char>(c1));
			for (size_t w = 1; w < 4; ++w) {
				for (int sh = 24; sh >= 0; sh -= 8) {
					const char c = static_cast<char>((p.words[w] >> sh) & 0xFFu);
					if (c) stProductId.push_back(c);
				}
			}
		}
	}
	E2E_CHECK(sawFb0);
	E2E_CHECK(sawFb1);
	E2E_CHECK(sawIdentity);
	E2E_CHECK(stProductId == "00112233445566778899AABB");

	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		const m2s::CiPeer& peer = snap.peers[0];
		E2E_CHECK(peer.answeredDiscovery);
		E2E_CHECK(peer.discoveredUs);
		E2E_CHECK(ms::MuidIsUsable(peer.muid));
		E2E_CHECK_EQ(peer.version, 2u);
		E2E_CHECK_EQ(peer.functionBlock, 0u);
		E2E_CHECK_EQ(peer.maxSysEx, 512u);
		E2E_CHECK_EQ(peer.manufacturer[0], static_cast<uint8_t>(CI_MANUFACTURER_0));
		E2E_CHECK_EQ(peer.manufacturer[1], static_cast<uint8_t>(CI_MANUFACTURER_1));
		E2E_CHECK_EQ(peer.manufacturer[2], static_cast<uint8_t>(CI_MANUFACTURER_2));
		E2E_CHECK_EQ(peer.family, static_cast<uint16_t>(CI_FAMILY));
		E2E_CHECK_EQ(peer.model, static_cast<uint16_t>(CI_MODEL));
		E2E_CHECK(peer.productInstanceId == "00112233445566778899AABB");
		E2E_CHECK_EQ(peer.resourceListStatus, 200);
		E2E_CHECK_EQ(peer.deviceInfoStatus, 200);
		E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "serialNumber") == peer.productInstanceId);
		E2E_CHECK(peer.notes.empty());
	}

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t lpPeer{};
	E2E_CHECK(fw.get_peer(0, &lpPeer));
	E2E_CHECK_EQ(lpPeer.muid, 0x0140101u);
	E2E_CHECK_EQ(lpPeer.max_sysex, 4096u);
	E2E_CHECK((lpPeer.flags & CI_PEER_FLAG_COMPLETE) != 0u);
	E2E_CHECK(std::strcmp(lpPeer.product_instance_id, "M2S-T4-01") == 0);
	E2E_CHECK_EQ(lpPeer.device_info_status, 200u);
}
#endif

#if E2E_HAS_M2
E2E_TEST(4, "T4-02", "Property Exchange chunked (128B, <=1 chunk/pass) vs unchunked (4096B) plus 404 and 405") {
	uint32_t fwMuidFinal = 0u;
	for (uint32_t maxSysEx : {128u, 4096u}) {
		e2e::FirmwareNode fw;
		fw.boot(true, 1);

		m2s::ci::CiIdentity id;
		id.maxSysEx = maxSysEx;
		id.productInstanceId = "M2S-T4-02";
		e2e::WirePair wire(fw, id, 0x0402020u);

		std::vector<m2s::UmpPacket> capturedFw, capturedAgent;
		m2s::ci::SysEx7Assembler passAsm;
		wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
			capturedFw.insert(capturedFw.end(), pkts.begin(), pkts.end());
			int completedThisPump = 0;
			for (const auto& p : pkts) {
				if (m2s::umpType(p) != 0x3u || m2s::umpGroup(p) != 0u) continue;
				std::vector<uint8_t> m;
				if (passAsm.feed(p, m)) {
					ms::ParsedMessage pm{};
					if (ms::Parse(m.data(), m.size(), pm) == ms::ParseStatus::Ok &&
					    pm.Type == ms::MessageType::PropertyGetDataReply) {
						++completedThisPump;
					}
				}
			}
			E2E_CHECK(completedThisPump <= 1);
		};
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

		E2E_CHECK(wire.agent().discover(nullptr));
		int idle = 0;
		for (int c = 0; c < 512; ++c) {
			const bool active = wire.pump();
			if (!active && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
				if (++idle >= 2) break;
			} else {
				idle = 0;
			}
		}
		wire.tx_fw_to_agent_filter = nullptr;
		wire.tx_agent_to_fw_filter = nullptr;

		const m2s::CiSnapshot snap = wire.agent().snapshot();
		E2E_CHECK_EQ(snap.peers.size(), 1u);
		if (snap.peers.size() != 1u) return;
		const auto& peer = snap.peers[0];
		fwMuidFinal = peer.muid;
		E2E_CHECK_EQ(peer.resourceListStatus, 200);
		E2E_CHECK_EQ(peer.deviceInfoStatus, 200);
		E2E_CHECK(peer.deviceInfo.find("\"manufacturerId\":[0,32,41]") != std::string::npos);
		E2E_CHECK(peer.deviceInfo.find("\"familyId\":[81,0]") != std::string::npos);
		E2E_CHECK(peer.deviceInfo.find("\"modelId\":[0,0]") != std::string::npos);
		E2E_CHECK(peer.deviceInfo.find("\"versionId\":[0,1,0,0]") != std::string::npos);
		E2E_CHECK(m2s::ci::ciJsonString(peer.deviceInfo, "serialNumber") == "TEST0001");

		uint8_t rlReqId = 0u, devReqId = 0u;
		for (const auto& m : reassemble_all_ci(capturedAgent, 0)) {
			ms::ParsedMessage p{};
			if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
			    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
				const std::string hdr(
				    reinterpret_cast<const char*>(m.data() + p.PropertyExchange.HeaderOffset),
				    p.PropertyExchange.HeaderByteCount);
				const std::string res = m2s::ci::ciJsonString(hdr, "resource");
				if (res == "ResourceList") rlReqId = p.PropertyExchange.RequestId;
				if (res == "DeviceInfo") devReqId = p.PropertyExchange.RequestId;
			}
		}

		int rlChunks = 0, devChunks = 0;
		const size_t limit = (maxSysEx == 128u) ? 128u : 512u;
		for (const auto& m : reassemble_all_ci(capturedFw, 0)) {
			ms::ParsedMessage p{};
			if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok ||
			    p.Type != ms::MessageType::PropertyGetDataReply || !p.HasPropertyExchangeFields) {
				continue;
			}
			if (p.PropertyExchange.RequestId == rlReqId) {
				++rlChunks;
				E2E_CHECK(m.size() + 2u <= limit);
			} else if (p.PropertyExchange.RequestId == devReqId) {
				++devChunks;
				E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, static_cast<uint16_t>(devChunks));
				E2E_CHECK(m.size() + 2u <= limit);
			}
		}
		if (maxSysEx == 128u) {
			E2E_CHECK(rlChunks > 1);
			E2E_CHECK_EQ(devChunks, 3);
		} else {
			E2E_CHECK_EQ(devChunks, 1);
		}

		if (maxSysEx == 4096u) {
			std::vector<m2s::UmpPacket> errPkts;
			wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
				errPkts.insert(errPkts.end(), pkts.begin(), pkts.end());
			};
			auto sendPe = [&](ms::MessageType type, uint8_t reqId, const std::string& hdr) {
				ms::PropertyExchangeMessageFields pf{};
				pf.Type = type;
				pf.SourceMuid = wire.agent().muid();
				pf.DestinationMuid = fwMuidFinal;
				pf.RequestId = reqId;
				pf.Header = reinterpret_cast<const uint8_t*>(hdr.data());
				pf.HeaderByteCount = static_cast<uint16_t>(hdr.size());
				pf.ChunkCount = 1u;
				pf.ChunkNumber = 1u;
				uint8_t buf[128] = {};
				const size_t n = ms::BuildPropertyExchangeMessage(pf, buf, sizeof(buf));
				wire.inject_sysex7_to_fw(0, buf, static_cast<uint16_t>(n));
			};
			sendPe(ms::MessageType::PropertyGetDataInquiry, 44u, R"({"resource":"DoesNotExist"})");
			sendPe(ms::MessageType::PropertySubscriptionInquiry, 45u,
			       R"({"command":"start","resource":"DeviceInfo"})");
			E2E_CHECK(wire.settle(128));
			wire.tx_fw_to_agent_filter = nullptr;

			bool saw404 = false, saw405 = false;
			for (const auto& m : reassemble_all_ci(errPkts, 0)) {
				ms::ParsedMessage p{};
				if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok ||
				    !p.HasPropertyExchangeFields) {
					continue;
				}
				const std::string hdr(
				    reinterpret_cast<const char*>(m.data() + p.PropertyExchange.HeaderOffset),
				    p.PropertyExchange.HeaderByteCount);
				const int status = m2s::ci::ciJsonInt(hdr, "status", 0);
				if (p.Type == ms::MessageType::PropertyGetDataReply &&
				    p.PropertyExchange.RequestId == 44u) {
					saw404 = (status == 404 && p.PropertyExchange.DataByteCount == 0u);
				}
				if (p.Type == ms::MessageType::PropertySubscriptionReply &&
				    p.PropertyExchange.RequestId == 45u) {
					saw405 = (status == 405);
				}
			}
			E2E_CHECK(saw404);
			E2E_CHECK(saw405);
		}
	}
}
#endif

#if E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(4, "T4-03", "Self-echo loopback, MUID collision, mid-transfer InvalidateMuid, and InitiatorMuidInvalidated") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x04031111u, 0x04032222u, 0x04033333u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);

	e2e::WireConfig cfg;
	cfg.agent_identity.maxSysEx = 128u;
	cfg.agent_identity.productInstanceId = "M2S-T4-03";
	cfg.agent_muid = 0x0403030u;
	cfg.self_echo_to_fw = true;
	cfg.self_echo_to_agent = true;
	e2e::WirePair wire(fw, cfg);

	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot s0 = wire.agent().snapshot();
	E2E_CHECK_EQ(s0.peers.size(), 1u);
	if (s0.peers.empty()) return;
	const uint32_t oldFwMuid = s0.peers[0].muid;
	E2E_CHECK_EQ(s0.peers[0].deviceInfoStatus, 200);

	// 1. Foreign colliding Discovery -> InvalidateMuid(newFwMuid, oldFwMuid) + ST_FB_INFO
	std::vector<m2s::UmpPacket> colPkts;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		colPkts.insert(colPkts.end(), pkts.begin(), pkts.end());
	};
	std::vector<uint8_t> colDisc(ms::DiscoveryByteCount, 0u);
	ms::DiscoveryReplyFields df{};
	df.SourceMuid = oldFwMuid;
	df.DestinationMuid = ms::MuidBroadcast;
	df.CapabilityCategories = 0x08u;
	df.ReceivableMaximumSysExSize = 128u;
	ms::BuildDiscovery(df, colDisc.data(), colDisc.size());
	wire.inject_sysex7_to_fw(0, colDisc.data(), static_cast<uint16_t>(colDisc.size()));
	E2E_CHECK(wire.settle(128));
	wire.tx_fw_to_agent_filter = nullptr;

	uint32_t newFwMuid = 0u;
	bool sawColInv = false, sawFbNotify = false;
	for (const auto& p : colPkts) {
		if (m2s::umpType(p) == 0xFu && p.words[0] == 0xF0118033u) sawFbNotify = true;
	}
	for (const auto& m : reassemble_all_ci(colPkts, 0)) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::InvalidateMuid && p.TargetMuid == oldFwMuid) {
			sawColInv = true;
			newFwMuid = p.SourceMuid;
		}
	}
	E2E_CHECK(sawColInv);
	E2E_CHECK(sawFbNotify);
	E2E_CHECK(newFwMuid != oldFwMuid && ms::MuidIsUsable(newFwMuid));

	// 2. Re-discover newFwMuid, start 128B GET DeviceInfo, InvalidateMuid after chunk 1/3
	E2E_CHECK(wire.discover_from_agent());
	const std::string hdr = R"({"resource":"DeviceInfo"})";
	ms::PropertyExchangeMessageFields pf{};
	pf.Type = ms::MessageType::PropertyGetDataInquiry;
	pf.SourceMuid = 0x0403030u;
	pf.DestinationMuid = newFwMuid;
	pf.RequestId = 71u;
	pf.Header = reinterpret_cast<const uint8_t*>(hdr.data());
	pf.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	pf.ChunkCount = 1u;
	pf.ChunkNumber = 1u;
	uint8_t peBuf[128] = {};
	const size_t peN = ms::BuildPropertyExchangeMessage(pf, peBuf, sizeof(peBuf));

	std::vector<m2s::UmpPacket> midPkts;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		midPkts.insert(midPkts.end(), pkts.begin(), pkts.end());
	};
	wire.inject_sysex7_to_fw(0, peBuf, static_cast<uint16_t>(peN));
	// Pump 1 cycle: sends all 16 words of GET DeviceInfo to EP1 OUT and queues chunk 1/3;
	// inject InvalidateMuid immediately before cycle 2 queues chunks 2..3.
	wire.pump();
	E2E_CHECK(midici_tx_pending() > 0u);

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(0x0403030u, newFwMuid, inv, sizeof(inv));
	wire.inject_sysex7_to_fw(0, inv, static_cast<uint16_t>(invN));
	E2E_CHECK(wire.settle(128));
	wire.tx_fw_to_agent_filter = nullptr;

	int totalChunks71 = 0;
	for (const auto& m : reassemble_all_ci(midPkts, 0)) {
		ms::ParsedMessage pm{};
		if (ms::Parse(m.data(), m.size(), pm) == ms::ParseStatus::Ok &&
		    pm.Type == ms::MessageType::PropertyGetDataReply &&
		    pm.SourceMuid == newFwMuid && pm.PropertyExchange.RequestId == 71u) {
			++totalChunks71;
			E2E_CHECK_EQ(pm.PropertyExchange.ChunkNumber, 1u);
			E2E_CHECK_EQ(pm.PropertyExchange.ChunkCount, 3u);
		}
	}
	E2E_CHECK_EQ(totalChunks71, 1);
	E2E_CHECK_EQ(midici_tx_pending(), 0u);
	const uint32_t thirdFwMuid = midici_muid();
	E2E_CHECK(thirdFwMuid != newFwMuid);

	E2E_CHECK(wire.discover_from_agent());
	const auto pThird = wire.find_agent_peer(thirdFwMuid);
	E2E_CHECK(pThird.has_value());
	if (pThird.has_value()) {
		E2E_CHECK_EQ(pThird->deviceInfoStatus, 200);
	}

	// 3. InitiatorMuidInvalidated (TargetMuid == CiAgent 0x0403030u):
	//    purges Launchpad's initiator table AND purges pe_session maxSysEx=128 cache
	fw.start_discovery();
	E2E_CHECK(wire.settle(256));
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	uint8_t invAgent[ms::InvalidateMuidByteCount] = {};
	const size_t invAgentN =
	    ms::BuildInvalidateMuid(0x0999999u, 0x0403030u, invAgent, sizeof(invAgent));
	wire.inject_sysex7_to_fw(0, invAgent, static_cast<uint16_t>(invAgentN));
	E2E_CHECK(wire.settle(128));
	E2E_CHECK_EQ(fw.peer_count(), 0u);

	// Uncached peer falls back to 512 -> single-chunk DeviceInfo
	std::vector<m2s::UmpPacket> uncachedPkts;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		uncachedPkts.insert(uncachedPkts.end(), pkts.begin(), pkts.end());
	};
	pf.DestinationMuid = thirdFwMuid;
	pf.RequestId = 72u;
	const size_t peN2 = ms::BuildPropertyExchangeMessage(pf, peBuf, sizeof(peBuf));
	wire.inject_sysex7_to_fw(0, peBuf, static_cast<uint16_t>(peN2));
	E2E_CHECK(wire.settle(128));
	wire.tx_fw_to_agent_filter = nullptr;

	int chunks72 = 0;
	for (const auto& m : reassemble_all_ci(uncachedPkts, 0)) {
		ms::ParsedMessage pm{};
		if (ms::Parse(m.data(), m.size(), pm) == ms::ParseStatus::Ok &&
		    pm.Type == ms::MessageType::PropertyGetDataReply &&
		    pm.PropertyExchange.RequestId == 72u) {
			++chunks72;
			E2E_CHECK_EQ(pm.PropertyExchange.ChunkCount, 1u);
		}
	}
	E2E_CHECK_EQ(chunks72, 1);
}

E2E_TEST(4, "T4-04", "Setup-mode pad trigger -> 3000ms window -> CiAgent (0x08) + 0x04|0x08 Peer 2 + late Peer 3 + LEDs") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	m2s::ci::CiIdentity id;
	id.productInstanceId = "M2S-PEER1";
	e2e::WirePair wire(fw, id, 0x0404011u);

	std::vector<m2s::UmpPacket> txLog;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		txLog.insert(txLog.end(), pkts.begin(), pkts.end());
	};

	fw.tap_setup(50);
	fw.press_pad(static_cast<uint8_t>(BTN_MIDICI), 127);
	const uint32_t t0 = midici_initiator_now_ms();
	fw.advance_ms(20);
	fw.release_pad(static_cast<uint8_t>(BTN_MIDICI));
	const uint32_t ourMuid = midici_muid();

	E2E_CHECK(led_is(11u, 0u, 48u, 63u));
	for (uint8_t pad = 21u; pad <= 28u; ++pad) {
		E2E_CHECK(led_is(pad, 0u, 0u, 0u));
	}

	// Settle Peer 1 (CiAgent, 0x08) at t0 + 100 ms -> pad 21 turns bright green
	wire.advance_ms_and_settle(100u - (midici_initiator_now_ms() - t0));
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t p1{};
	E2E_CHECK(fw.get_peer(0, &p1));
	E2E_CHECK_EQ(p1.muid, 0x0404011u);
	E2E_CHECK((p1.flags & CI_PEER_FLAG_COMPLETE) != 0u);
	E2E_CHECK_EQ(p1.device_info_status, 200u);
	E2E_CHECK(led_is(21u, 0u, 63u, 0u));

	// Advance to t0 + 2999 ms -> inject Peer 2 (0x04 | 0x08) -> amber pad 22, ProfileInquiry sent to Peer 2 only
	fw.advance_ms(2999u - (midici_initiator_now_ms() - t0));
	E2E_CHECK(midici_initiator_is_discovering());
	wire.inject_sysex7_to_fw(0, make_discovery_reply(0x0404022u, ourMuid, 0x04u | 0x08u).data(),
	                         static_cast<uint16_t>(ms::DiscoveryReplyByteCount));
	// Pump enough to process DiscoveryReply + emit initial follow-ups (but before Peer 2 replies)
	for (int i = 0; i < 12; ++i) wire.pump();
	E2E_CHECK_EQ(fw.peer_count(), 2u);
	E2E_CHECK(led_is(22u, 63u, 24u, 0u));

	// Complete Peer 2 follow-ups (Endpoint, Profile, PE Caps, DeviceInfo)
	wire.inject_sysex7_to_fw(0, make_endpoint_reply(0x0404022u, ourMuid, "SIM-PEER2").data(),
	                         static_cast<uint16_t>(ms::EndpointReplyFixedByteCount + 9u));
	const auto profRep = make_profile_reply(0x0404022u, ourMuid);
	wire.inject_sysex7_to_fw(0, profRep.data(), static_cast<uint16_t>(profRep.size()));
	const auto peCapsRep = make_pe_caps_reply(0x0404022u, ourMuid, 1u);
	wire.inject_sysex7_to_fw(0, peCapsRep.data(), static_cast<uint16_t>(peCapsRep.size()));
	E2E_CHECK(wire.settle(128));

	uint8_t p2DevReq = 0;
	bool profToP1 = false, profToP2 = false;
	for (const auto& m : reassemble_all_ci(txLog, 0)) {
		ms::ParsedMessage pm{};
		if (ms::Parse(m.data(), m.size(), pm) != ms::ParseStatus::Ok) continue;
		if (pm.Type == ms::MessageType::ProfileInquiry) {
			if (pm.DestinationMuid == 0x0404011u) profToP1 = true;
			if (pm.DestinationMuid == 0x0404022u) profToP2 = true;
		}
		if (pm.Type == ms::MessageType::PropertyGetDataInquiry &&
		    pm.DestinationMuid == 0x0404022u && pm.HasPropertyExchangeFields) {
			p2DevReq = pm.PropertyExchange.RequestId;
		}
	}
	E2E_CHECK(!profToP1);
	E2E_CHECK(profToP2);
	E2E_CHECK(p2DevReq != 0u);

	const auto devRep = make_pe_get_reply(0x0404022u, ourMuid, p2DevReq, R"({"status":200})",
	                                      R"({"manufacturer":"SimPeer2"})");
	wire.inject_sysex7_to_fw(0, devRep.data(), static_cast<uint16_t>(devRep.size()));
	E2E_CHECK(wire.settle(128));
	E2E_CHECK(led_is(22u, 0u, 63u, 0u));

	// Advance past 3000 ms (t0 + 3005 ms) -> window closes, pad 11 dims, late Peer 3 ignored
	fw.advance_ms(3005u - (midici_initiator_now_ms() - t0));
	E2E_CHECK(!midici_initiator_is_discovering());
	E2E_CHECK(led_is(11u, 0u, 6u, 8u));
	wire.inject_sysex7_to_fw(0, make_discovery_reply(0x0404033u, ourMuid, 0x00u).data(),
	                         static_cast<uint16_t>(ms::DiscoveryReplyByteCount));
	E2E_CHECK(wire.settle(64));
	E2E_CHECK_EQ(fw.peer_count(), 2u);
	E2E_CHECK(led_is(23u, 0u, 0u, 0u));

	// Close Setup -> Play LEDs restored (pad 11 ROLE_ROOT); reopen -> both peers green
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 0u);
	E2E_CHECK(led_is(11u, 0u, 16u, 63u));
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	E2E_CHECK(led_is(21u, 0u, 63u, 0u));
	E2E_CHECK(led_is(22u, 0u, 63u, 0u));
	for (uint8_t pad = 23u; pad <= 28u; ++pad) {
		E2E_CHECK(led_is(pad, 0u, 0u, 0u));
	}
	wire.tx_fw_to_agent_filter = nullptr;
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2
E2E_TEST(4, "T4-05", "Adversarial wire resilience: malformed MT3, 513B drop, 0x3FFF/truncated PE -> NAK 0x41 + recovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0405050u);
	E2E_CHECK(wire.discover_from_agent());

	const m2s::CiSnapshot snap0 = wire.agent().snapshot();
	E2E_CHECK_EQ(snap0.peers.size(), 1u);
	if (snap0.peers.empty()) return;
	const uint32_t fwMuid = snap0.peers[0].muid;

	const uint32_t drops0 = fw.reassembly_drops();
	const uint64_t mal0 = wire.agent().snapshot().malformed;

	std::vector<m2s::UmpPacket> captured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		captured.insert(captured.end(), pkts.begin(), pkts.end());
	};

	// (a) orphan kContinue & kEnd + (b) invalid cnt=7 & st=4
	const uint32_t badUmp[8] = {
	    0x30231122u, 0x33445566u,  // orphan kContinue
	    0x30321122u, 0x00000000u,  // orphan kEnd
	    0x30070000u, 0x00000000u,  // cnt=7
	    0x30420000u, 0x00000000u,  // st=4
	};
	wire.inject_ump_to_fw(badUmp, 8u);

	// (c) 513B overlong SysEx7 stream
	std::vector<uint8_t> over513(513u, 0x33u);
	over513[0] = 0x7Eu;
	over513[1] = 0x7Fu;
	over513[2] = 0x0Du;
	over513[3] = 0x70u;
	over513[4] = 0x02u;
	wire.inject_sysex7_to_fw(0, over513.data(), static_cast<uint16_t>(over513.size()));

	// (d) truncated 13B PE Get + (e) PE Get HeaderByteCount=0x3FFF
	uint8_t truncPe[13] = {};
	ms::WriteCommonHeader(truncPe, sizeof(truncPe), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataInquiry, wire.agent().muid(), fwMuid);
	wire.inject_sysex7_to_fw(0, truncPe, sizeof(truncPe));

	std::vector<uint8_t> badPe(ms::PropertyExchangeFixedByteCount + 4u, 0u);
	ms::WriteCommonHeader(badPe.data(), badPe.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataInquiry, wire.agent().muid(), fwMuid);
	badPe[13] = 3u;
	ms::WriteFourteenBitValue(&badPe[14], 0x3FFFu);
	wire.inject_sysex7_to_fw(0, badPe.data(), static_cast<uint16_t>(badPe.size()));

	// (f) malformed EndpointReply & ProfileInquiryReply into CiAgent
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

	E2E_CHECK(fw.reassembly_drops() - drops0 >= 5u);
	E2E_CHECK(wire.agent().snapshot().malformed >= mal0 + 2u);

	int nakPeCount = 0;
	for (const auto& m : reassemble_all_ci(captured, 0)) {
		ms::ParsedMessage pm{};
		if (ms::Parse(m.data(), m.size(), pm) == ms::ParseStatus::Ok &&
		    pm.Type == ms::MessageType::Nak && pm.HasAcknowledgmentFields &&
		    pm.Acknowledgment.OriginalMessageType ==
		        static_cast<uint8_t>(ms::MessageType::PropertyGetDataInquiry) &&
		    pm.Acknowledgment.StatusCode == ms::NakStatusMessageMalformed) {
			++nakPeCount;
		}
	}
	E2E_CHECK_EQ(nakPeCount, 2);

	// Clean post-attack recovery
	E2E_CHECK(wire.discover_from_agent());
	const auto recovered = wire.find_agent_peer(fwMuid);
	E2E_CHECK(recovered.has_value());
	if (recovered.has_value()) {
		E2E_CHECK_EQ(recovered->resourceListStatus, 200);
		E2E_CHECK_EQ(recovered->deviceInfoStatus, 200);
	}
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(4, "T4-06", "Concurrent Play-mode notes/pressure + 16x 0xF8 clocks + ST_EP_DISCOVERY + 128B chunked PE") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	m2s::ci::CiIdentity id;
	id.maxSysEx = 128u;
	id.productInstanceId = "M2S-T4-06";
	e2e::WirePair wire(fw, id, 0x0406060u);

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK(wire.settle(256));

	std::vector<m2s::UmpPacket> captured;
	wire.tx_fw_to_agent_filter = [&](std::vector<m2s::UmpPacket>& pkts) {
		captured.insert(captured.end(), pkts.begin(), pkts.end());
	};

	E2E_CHECK(wire.agent().discover(nullptr));

	for (int i = 0; i < 16; ++i) {
		hal_send_midi(USBSTANDALONE, 0xF8u, 0u, 0u);
	}
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	wire.inject_ump_to_fw(epDisc.data(), epDisc.size());

	const uint8_t pads[3] = {11u, 13u, 15u};
	for (uint8_t p : pads) {
		arm_pad_adc(p, 800u, 2800u);
		fw.press_pad(p, 127u);
	}
	wire.advance_ms_and_settle(10);

	for (uint8_t p : pads) {
		fw.release_pad(p);
	}
	wire.advance_ms_and_settle(4);
	wire.tx_fw_to_agent_filter = nullptr;

	int epInfo = 0, clocks = 0, noteOn = 0, noteOff = 0, polyPress = 0;
	for (const auto& p : captured) {
		const uint8_t mt = m2s::umpType(p);
		if (mt == 0xFu && p.words[0] == 0xF0010101u) ++epInfo;
		else if (mt == 0x1u && p.words[0] == 0x10F80000u) ++clocks;
		else if (mt == 0x4u) {
			const uint8_t st = static_cast<uint8_t>((p.words[0] >> 20) & 0xFu);
			if (st == 0x9u) ++noteOn;
			else if (st == 0x8u) ++noteOff;
			else if (st == 0xAu) ++polyPress;
		}
	}

	E2E_CHECK(epInfo >= 1);
	E2E_CHECK_EQ(clocks, 16);
	E2E_CHECK_EQ(noteOn, 3);
	E2E_CHECK_EQ(noteOff, 3);
	E2E_CHECK(polyPress >= 1);
	E2E_CHECK_EQ(ump_tx_reserve_words(), 0u);

	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK_EQ(snap.peers[0].resourceListStatus, 200);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
}

E2E_TEST(4, "T4-07", "USB alt 1<->0 and MIDI 2.0<->MIDI 1.0 protocol switch mid-session recovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	m2s::ci::CiIdentity id;
	id.maxSysEx = 128u;
	id.productInstanceId = "M2S-T4-07";
	e2e::WirePair wire(fw, id, 0x0407070u);

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK(wire.discover_from_agent());
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK(setup_led_is(0u, 63u, 0u));

	// Start partial SysEx7 + protocol switches MIDI2->MIDI1->MIDI2, then drop USB alt 1 -> 0
	const uint32_t partialStart[2] = {0x30167E7Fu, 0x0D700201u};
	fw.send_ump(partialStart, 2u);
	fw.poll(1);

	const auto toMidi1 = e2e::make_stream_config_req(UMP_PROTO_MIDI1, 0u);
	fw.send_ump(toMidi1.data(), static_cast<uint16_t>(toMidi1.size()));
	fw.poll(4);
	E2E_CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI1);
	E2E_CHECK(setup_led_is(63u, 20u, 0u));

	const auto toMidi2 = e2e::make_stream_config_req(UMP_PROTO_MIDI2, 0u);
	fw.send_ump(toMidi2.data(), static_cast<uint16_t>(toMidi2.size()));
	fw.poll(4);
	E2E_CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI2);

	fw.set_alt(0);
	wire.set_connected(false);
	fw.poll(4);
	(void)fw.drain_ump();

	E2E_CHECK_EQ(fw.peer_count(), 0u);
	E2E_CHECK_EQ(midici_tx_pending(), 0u);
	E2E_CHECK(setup_led_is(63u, 20u, 0u));

	// Reconnect alt 1, verify pre-reset partial is discarded (post-reset kEnd increments drops),
	// and re-establish mutual MIDI-CI session cleanly
	const uint32_t drops0 = fw.reassembly_drops();
	fw.set_alt(1);
	wire.set_connected(true);
	wire.agent().resetTransport();
	fw.poll(2);

	const uint32_t postEnd[2] = {0x30320102u, 0u};
	fw.send_ump(postEnd, 2u);
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK(wire.discover_from_agent());
	E2E_CHECK(wire.settle(256));
	E2E_CHECK(setup_led_is(0u, 63u, 0u));
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK(snap.peers[0].discoveredUs);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
}
#endif
