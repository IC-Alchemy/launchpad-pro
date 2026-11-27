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

#if E2E_HAS_M2 || E2E_HAS_M3
namespace ms = m2s::ci::ms;

void send_ci(e2e::FirmwareNode& fw, uint8_t group, const std::vector<uint8_t>& payload) {
	const std::vector<uint32_t> words = e2e::pack_sysex7_words(group, payload);
	fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
}
#endif

#if E2E_HAS_M1
struct SpyMsg {
	uint8_t group = 0;
	std::vector<uint8_t> payload;
};

std::vector<SpyMsg>& spy_log() {
	static std::vector<SpyMsg> log;
	return log;
}

void spy_handler(uint8_t group, const uint8_t* payload, uint16_t len) {
	SpyMsg m;
	m.group = group;
	if (payload && len > 0) m.payload.assign(payload, payload + len);
	spy_log().push_back(std::move(m));
}

void install_spy() {
	spy_log().clear();
	midici_set_handler(spy_handler);
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2
struct SplitMix64 {
	uint64_t state;
	explicit SplitMix64(uint64_t seed) : state(seed) {}
	uint64_t next_u64() {
		uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	uint32_t next_u32() { return static_cast<uint32_t>(next_u64() >> 32); }
	uint8_t next_u8() { return static_cast<uint8_t>(next_u64() >> 56); }
};
#endif

#if E2E_HAS_M2
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

#if E2E_HAS_M2 || E2E_HAS_M3
struct DrainedOutputs {
	std::vector<std::vector<uint8_t>> ciMsgs;
	std::vector<std::vector<uint8_t>> otherGroupCiMsgs;
	std::vector<std::array<uint32_t, 4>> streamMsgs;
	std::vector<m2s::UmpPacket> rawMt3Pkts;
};

// The expanded ResourceList spans more than 96 polls at four SysEx7 packets per drain.
DrainedOutputs drain_outputs(e2e::FirmwareNode& fw, uint8_t group = 0, int maxPolls = 256) {
	DrainedOutputs out;
	m2s::ci::SysEx7Assembler mainAsm;
	m2s::ci::SysEx7Assembler otherAsm;
	int idle = 0;
	for (int i = 0; i < maxPolls; ++i) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		const bool hadPkts = !pkts.empty();
		for (const auto& p : pkts) {
			const uint8_t mt = m2s::umpType(p);
			if (mt == 0xFu && p.wordCount == 4) {
				out.streamMsgs.push_back({p.words[0], p.words[1], p.words[2], p.words[3]});
			} else if (mt == 0x3u && p.wordCount == 2) {
				out.rawMt3Pkts.push_back(p);
				std::vector<uint8_t> msg;
				if (m2s::umpGroup(p) == (group & 0xFu)) {
					if (mainAsm.feed(p, msg)) out.ciMsgs.push_back(std::move(msg));
				} else {
					if (otherAsm.feed(p, msg)) out.otherGroupCiMsgs.push_back(std::move(msg));
				}
			}
		}
		if (!hadPkts && midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			if (++idle >= 3) break;
		} else {
			idle = 0;
		}
	}
	return out;
}
#endif

#if E2E_HAS_M2
std::vector<uint8_t> make_discovery_bytes(uint32_t srcMuid, uint32_t maxSysEx,
                                          uint8_t ver = 0x02u, uint8_t outputPathId = 0x00u) {
	const size_t len = (ver == ms::MessageVersion11) ? ms::DiscoveryByteCountVersion11
	                                                 : ms::DiscoveryByteCount;
	std::vector<uint8_t> buf(len, 0u);
	buf[0] = ms::UniversalSystemExclusiveId;
	buf[1] = ms::DeviceIdFunctionBlock;
	buf[2] = ms::SubId1CapabilityInquiry;
	buf[3] = static_cast<uint8_t>(ms::MessageType::Discovery);
	buf[4] = ver & 0x7Fu;
	ms::WriteMuid(&buf[5], srcMuid);
	ms::WriteMuid(&buf[9], ms::MuidBroadcast);
	buf[13] = 0x7Du;
	ms::WriteFourteenBitValue(&buf[16], 0x0001u);
	ms::WriteFourteenBitValue(&buf[18], 0x0001u);
	buf[21] = 0x01u;
	buf[24] = 0x08u;
	ms::WriteMuid(&buf[25], maxSysEx);
	if (len >= ms::DiscoveryByteCount) {
		buf[29] = outputPathId & 0x7Fu;
	}
	return buf;
}

uint32_t discover_fw_muid(e2e::FirmwareNode& fw, uint32_t initiatorMuid, uint32_t maxSysEx) {
	send_ci(fw, 0, make_discovery_bytes(initiatorMuid, maxSysEx, 0x02u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() != 1u) return 0u;
	ms::ParsedMessage parsed{};
	E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), parsed) == ms::ParseStatus::Ok);
	E2E_CHECK(parsed.Type == ms::MessageType::DiscoveryReply);
	return parsed.SourceMuid;
}

std::vector<uint8_t> make_pe_bytes(ms::MessageType type, uint32_t src, uint32_t dst, uint8_t reqId,
                                   const std::string& hdr, uint16_t chunkCount = 1u,
                                   uint16_t chunkNum = 1u, const std::string& data = "") {
	ms::PropertyExchangeMessageFields f{};
	f.Type = type;
	f.SourceMuid = src;
	f.DestinationMuid = dst;
	f.RequestId = reqId;
	f.Header = hdr.empty() ? nullptr : reinterpret_cast<const uint8_t*>(hdr.data());
	f.HeaderByteCount = static_cast<uint16_t>(hdr.size());
	f.ChunkCount = chunkCount;
	f.ChunkNumber = chunkNum;
	f.Data = data.empty() ? nullptr : reinterpret_cast<const uint8_t*>(data.data());
	f.DataByteCount = static_cast<uint16_t>(data.size());
	std::vector<uint8_t> buf(ms::PropertyExchangeFixedByteCount + hdr.size() + data.size(), 0u);
	buf.resize(ms::BuildPropertyExchangeMessage(f, buf.data(), buf.size()));
	return buf;
}

bool reassemble_pe_reply(const std::vector<std::vector<uint8_t>>& msgs,
                         ms::MessageType expectedType, uint8_t expectedReqId,
                         uint32_t expectedSrc, uint32_t expectedDst,
                         uint16_t maxMsgBytesWithFraming, std::string& outHdr,
                         std::string& outBody) {
	outHdr.clear();
	outBody.clear();
	if (msgs.empty()) return false;
	for (size_t idx = 0; idx < msgs.size(); ++idx) {
		const auto& m = msgs[idx];
		if (m.size() + 2u > maxMsgBytesWithFraming) return false;
		ms::ParsedMessage parsed{};
		if (ms::Parse(m.data(), m.size(), parsed) != ms::ParseStatus::Ok) return false;
		if (parsed.Type != expectedType) return false;
		if (parsed.VersionFormat != MIDI_CI_VERSION) return false;
		if (parsed.SourceMuid != expectedSrc || parsed.DestinationMuid != expectedDst) return false;
		if (!parsed.HasPropertyExchangeFields) return false;
		const auto& pe = parsed.PropertyExchange;
		if (pe.RequestId != expectedReqId) return false;
		if (pe.ChunkCount != msgs.size()) return false;
		if (pe.ChunkNumber != idx + 1u) return false;
		if (idx == 0u) {
			outHdr.assign(reinterpret_cast<const char*>(m.data() + pe.HeaderOffset),
			              pe.HeaderByteCount);
		} else if (pe.HeaderByteCount != 0u) {
			return false;
		}
		outBody.append(reinterpret_cast<const char*>(m.data() + pe.DataOffset), pe.DataByteCount);
	}
	return true;
}
#endif

#if E2E_HAS_M3
std::vector<std::vector<uint8_t>> drain_ci_group(e2e::FirmwareNode& fw, uint8_t group,
                                                 int passes = 16) {
	return drain_outputs(fw, group, passes).ciMsgs;
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
	const size_t cap =
	    (ver == ms::MessageVersion11) ? ms::DiscoveryReplyByteCountVersion11
	                                  : ms::DiscoveryReplyByteCount;
	std::vector<uint8_t> buf(cap, 0u);
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

#if E2E_HAS_M2 && E2E_HAS_M3
std::vector<uint8_t> make_profile_reply(uint32_t srcMuid, uint32_t dstMuid) {
	const uint8_t enabled[5] = {0x7Eu, 0x00u, 0x01u, 0x00u, 0x01u};
	std::vector<uint8_t> buf(64u, 0u);
	buf.resize(ms::BuildProfileInquiryReply(ms::DeviceIdFunctionBlock, srcMuid, dstMuid, enabled,
	                                        1u, nullptr, 0u, buf.data(), buf.size()));
	return buf;
}
#endif

std::vector<uint8_t> make_pe_caps_reply(uint32_t srcMuid, uint32_t dstMuid,
                                        uint8_t simultaneous = 1u,
                                        uint8_t ver = ms::MessageVersionCurrent) {
	std::vector<uint8_t> buf(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	buf.resize(ms::BuildPropertyExchangeCapabilitiesReply(srcMuid, dstMuid, simultaneous,
	                                                      buf.data(), buf.size(), ver));
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

std::vector<std::vector<uint8_t>> trigger_midici_setup_discovery(e2e::FirmwareNode& fw) {
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	auto msgs = drain_ci_group(fw, 0);
	if (msgs.empty()) {
		fw.tap_pad(11u, 20);
		msgs = drain_ci_group(fw, 0);
	}
	return msgs;
}

bool led_is(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
	return e2e_leds[idx][0] == r && e2e_leds[idx][1] == g && e2e_leds[idx][2] == b;
}

bool led_off(uint8_t idx) {
	return led_is(idx, 0u, 0u, 0u);
}
#endif

#if (E2E_HAS_M1 && E2E_HAS_M2) || E2E_HAS_M3
void arm_pad_adc(uint8_t pad, uint16_t adcMax10, uint16_t raw12) {
	const uint8_t slot = pad_to_adcmax_slot(pad);
	const uint8_t rawIdx = pad_raw_index(pad);
	if (slot != PAD_NO_SLOT) ADC_Max[slot] = adcMax10;
	if (rawIdx != PAD_NO_SLOT) e2e_adc[rawIdx] = raw12;
}
#endif

}  // namespace

// ============================================================================
// Tier 3 — Cross-Feature Pairwise Tests (T3-01..T3-18)
// ============================================================================

#if E2E_HAS_M1
E2E_TEST(3, "T3-01", "F1+F2+F3+F4: 512B group-0 reassembly interleaved with group-1 malformed drops + 512B TX round-trip") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	std::vector<uint8_t> payload512(512u, 0u);
	payload512[0] = 0x7Eu;
	payload512[1] = 0x7Fu;
	payload512[2] = 0x0Du;
	payload512[3] = 0x70u;
	payload512[4] = 0x02u;
	for (size_t i = 5; i < payload512.size(); ++i) {
		payload512[i] = static_cast<uint8_t>((i * 19u + 7u) & 0x7Fu);
	}

	const auto g0pkts = fw.packetize_sysex7(0u, payload512.data(), static_cast<uint16_t>(payload512.size()));
	E2E_CHECK_EQ(g0pkts.size(), 86u);

	const std::array<std::array<uint32_t, 2>, 4> g1Bad = {{
	    {0x31070000u, 0u},  // cnt = 7
	    {0x31410000u, 0u},  // st = 4
	    {0x31231122u, 0u},  // orphan kContinue
	    {0x31323344u, 0u},  // orphan kEnd
	}};

	size_t badIdx = 0;
	for (size_t i = 0; i < g0pkts.size(); ++i) {
		fw.send_ump(g0pkts[i].data(), 2u);
		if ((i == 10u || i == 30u || i == 50u || i == 70u) && badIdx < g1Bad.size()) {
			fw.send_ump(g1Bad[badIdx++].data(), 2u);
		}
		if ((i & 7u) == 7u) fw.poll(1);
	}
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 4u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(spy_log().size(), 1u);
	if (spy_log().size() == 1u) {
		E2E_CHECK_EQ(spy_log()[0].group, 0u);
		E2E_CHECK(spy_log()[0].payload == payload512);
	}

	// Send 512B back via midici_send_sysex -> verify 86 MT 0x3 packets reassemble bit-for-bit
	E2E_CHECK_EQ(fw.send_sysex(0u, payload512.data(), static_cast<uint16_t>(payload512.size())), 0);
	std::vector<m2s::UmpPacket> txMt3;
	for (int pass = 0; pass < 40; ++pass) {
		fw.poll_once();
		for (const auto& p : fw.drain_tx_packets()) {
			if (m2s::umpType(p) == 0x3u) txMt3.push_back(p);
		}
		if (midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) break;
	}
	E2E_CHECK_EQ(txMt3.size(), 86u);
	m2s::ci::SysEx7Assembler asmblr;
	std::vector<uint8_t> out;
	bool done = false;
	for (size_t i = 0; i < txMt3.size(); ++i) {
		const uint8_t st = static_cast<uint8_t>((txMt3[i].words[0] >> 20) & 0xFu);
		if (i == 0u) E2E_CHECK_EQ(st, 1u);
		else if (i + 1u == txMt3.size()) E2E_CHECK_EQ(st, 3u);
		else E2E_CHECK_EQ(st, 2u);
		if (asmblr.feed(txMt3[i], out)) done = true;
	}
	E2E_CHECK(done);
	E2E_CHECK_EQ(asmblr.dropped(), 0u);
	E2E_CHECK(out == payload512);

	midici_responder_init();
}
#endif

#if E2E_HAS_M2
E2E_TEST(3, "T3-02", "F5+F6+F8: FB version 0x02, v1->v2 ReplyVersionFor upgrade, group-0 vs group-1 routing") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const auto fbDisc = e2e::make_fb_discovery(0xFFu, 0x03u);
	const auto epDisc = e2e::make_ep_discovery(0x08u);
	fw.send_ump(fbDisc.data(), static_cast<uint16_t>(fbDisc.size()));
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	const DrainedOutputs streamOut = drain_outputs(fw, 0);

	bool sawFb0 = false, sawFb1 = false;
	std::string stProductId;
	for (const auto& s : streamOut.streamMsgs) {
		const uint16_t st = static_cast<uint16_t>((s[0] >> 16) & 0x3FFu);
		if (st == 0x011u) {
			const uint8_t fb = static_cast<uint8_t>((s[0] >> 8) & 0x7Fu);
			if (fb == 0u) {
				sawFb0 = (s[1] == 0x00010200u);
				E2E_CHECK_EQ((s[1] >> 8) & 0xFFu, 0x02u);
			} else if (fb == 1u) {
				sawFb1 = (s[1] == 0x01010000u);
			}
		} else if (st == 0x004u) {
			const uint8_t c0 = static_cast<uint8_t>((s[0] >> 8) & 0xFFu);
			const uint8_t c1 = static_cast<uint8_t>(s[0] & 0xFFu);
			if (c0) stProductId.push_back(static_cast<char>(c0));
			if (c1) stProductId.push_back(static_cast<char>(c1));
			for (size_t w = 1; w < 4; ++w) {
				for (int sh = 24; sh >= 0; sh -= 8) {
					const char c = static_cast<char>((s[w] >> sh) & 0xFFu);
					if (c) stProductId.push_back(c);
				}
			}
		}
	}
	E2E_CHECK(sawFb0);
	E2E_CHECK(sawFb1);
	E2E_CHECK(stProductId == "TEST0001");

	// Send v1 (0x01) 29B Discovery on both group 0 and group 1
	const auto v1Disc = make_discovery_bytes(0x0302020u, 512u, ms::MessageVersion11);
	send_ci(fw, 0u, v1Disc);
	send_ci(fw, 1u, v1Disc);
	const DrainedOutputs ciOut = drain_outputs(fw, 0);
	E2E_CHECK(ciOut.otherGroupCiMsgs.empty());
	E2E_CHECK_EQ(ciOut.ciMsgs.size(), 1u);
	if (ciOut.ciMsgs.size() == 1u) {
		E2E_CHECK_EQ(ciOut.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(ciOut.ciMsgs[0].data(), ciOut.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(p.VersionFormat, MIDI_CI_VERSION);
		E2E_CHECK_EQ(ciOut.ciMsgs[0][13], static_cast<uint8_t>(CI_MANUFACTURER_0));
		E2E_CHECK_EQ(ciOut.ciMsgs[0][14], static_cast<uint8_t>(CI_MANUFACTURER_1));
		E2E_CHECK_EQ(ciOut.ciMsgs[0][15], static_cast<uint8_t>(CI_MANUFACTURER_2));
		E2E_CHECK_EQ(ciOut.ciMsgs[0][30], 0u);  // Function Block 0
	}

	// v1 EndpointInquiry on group 0 -> v2 EndpointReply matching ST_PRODUCT_ID
	std::vector<uint8_t> v1Ep(ms::EndpointInquiryByteCount, 0u);
	v1Ep.resize(ms::BuildEndpointInquiry(0x0302020u, midici_muid(),
	                                     ms::EndpointStatusProductInstanceId, v1Ep.data(),
	                                     v1Ep.size()));
	v1Ep[ms::OffsetVersionFormat] = ms::MessageVersion11;
	send_ci(fw, 0u, v1Ep);
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	if (epOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(epOut.ciMsgs[0].data(), epOut.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::EndpointReply);
		E2E_CHECK_EQ(p.VersionFormat, MIDI_CI_VERSION);
		const std::string piid(
		    reinterpret_cast<const char*>(epOut.ciMsgs[0].data() + p.Endpoint.InformationOffset),
		    p.Endpoint.InformationByteCount);
		E2E_CHECK(piid == stProductId);
	}
}

E2E_TEST(3, "T3-03", "F5+F9+F10+F11: Whitespace/multi-key JSON header + maxSysEx=128 1-chunk/pass DeviceInfo") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t peerMuid = 0x0303030u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerMuid, 128u);
	E2E_CHECK(fwMuid != 0u);

	const std::string wsHdr =
	    R"( { "offset" : 0 , "resource" : "DeviceInfo" , "mutualEncoding" : "ASCII" } )";
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerMuid, fwMuid, 17u, wsHdr));

	std::vector<std::vector<uint8_t>> chunks;
	m2s::ci::SysEx7Assembler asmblr;
	int idle = 0;
	for (int pass = 0; pass < 96; ++pass) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		int completedThisPass = 0;
		for (const auto& p : pkts) {
			if (m2s::umpType(p) != 0x3u || m2s::umpGroup(p) != 0u) continue;
			std::vector<uint8_t> msg;
			if (asmblr.feed(p, msg)) {
				++completedThisPass;
				chunks.push_back(std::move(msg));
			}
		}
		E2E_CHECK(completedThisPass <= 1);
		if (pkts.empty() && midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			if (++idle >= 3) break;
		} else {
			idle = 0;
		}
	}

	E2E_CHECK_EQ(chunks.size(), 3u);
	std::string outHdr, outBody;
	E2E_CHECK(reassemble_pe_reply(chunks, ms::MessageType::PropertyGetDataReply, 17u, fwMuid,
	                              peerMuid, 128u, outHdr, outBody));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(outHdr, "status", 0), 200);
	E2E_CHECK(outBody.find("\"manufacturerId\":[0,32,41]") != std::string::npos);
	E2E_CHECK(outBody.find("\"familyId\":[81,0]") != std::string::npos);
	E2E_CHECK(outBody.find("\"modelId\":[0,0]") != std::string::npos);
	E2E_CHECK(outBody.find("\"versionId\":[0,1,0,0]") != std::string::npos);
	E2E_CHECK(m2s::ci::ciJsonString(outBody, "serialNumber") == "TEST0001");
}

E2E_TEST(3, "T3-04", "F7+F11: InvalidateMuid mid-128B chunking aborts chunks 2..3, emits ST_FB_INFO, and rotates MUID") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x03040111u, 0x03040222u, 0x03040333u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);

	const uint32_t peerMuid = 0x0304040u;
	const uint32_t oldMuid = discover_fw_muid(fw, peerMuid, 128u);
	E2E_CHECK(oldMuid != 0u);

	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerMuid, oldMuid, 11u,
	                      R"({"resource":"DeviceInfo"})"));

	// Pass 1 queues chunk 1/3 into CiTxQueue and drains its first packets;
	// inject InvalidateMuid immediately before Pass 2 queues chunks 2..3.
	std::vector<m2s::UmpPacket> allTxPkts;
	fw.poll_once();
	for (const auto& p : fw.drain_tx_packets()) allTxPkts.push_back(p);
	E2E_CHECK(midici_tx_pending() > 0u);

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(peerMuid, oldMuid, inv, sizeof(inv));
	send_ci(fw, 0u, std::vector<uint8_t>(inv, inv + invN));

	const DrainedOutputs after = drain_outputs(fw, 0);
	allTxPkts.insert(allTxPkts.end(), after.rawMt3Pkts.begin(), after.rawMt3Pkts.end());

	int totalOldChunks = 0;
	for (const auto& m : reassemble_all_ci(allTxPkts, 0u)) {
		ms::ParsedMessage parsed{};
		if (ms::Parse(m.data(), m.size(), parsed) == ms::ParseStatus::Ok &&
		    parsed.Type == ms::MessageType::PropertyGetDataReply &&
		    parsed.SourceMuid == oldMuid) {
			++totalOldChunks;
			E2E_CHECK_EQ(parsed.PropertyExchange.ChunkNumber, 1u);
			E2E_CHECK_EQ(parsed.PropertyExchange.ChunkCount, 3u);
		}
	}
	E2E_CHECK_EQ(totalOldChunks, 1);
	E2E_CHECK(midici_muid() != oldMuid);
	E2E_CHECK_EQ(midici_tx_pending(), 0u);

	bool sawFbNotify = false;
	for (const auto& s : after.streamMsgs) {
		if (s[0] == 0xF0118033u && s[1] == 0x00010200u) sawFbNotify = true;
	}
	E2E_CHECK(sawFbNotify);

	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0304099u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK(snap.peers[0].muid != oldMuid);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
}

E2E_TEST(3, "T3-05", "F7+F10+F11: InitiatorMuidInvalidated purges maxSysEx=128 cache -> 512 fallback, then 4096 re-discovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t peerA = 0x0311111u;
	const uint32_t peerB = 0x0322222u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerA, 128u);
	E2E_CHECK(fwMuid != 0u);

	// Cached maxSysEx=128 -> 3 chunks
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 1u,
	                      R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs d1 = drain_outputs(fw, 0);
	E2E_CHECK_EQ(d1.ciMsgs.size(), 3u);

	// Invalidate peerA -> forget_initiator(peerA)
	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(peerB, peerA, inv, sizeof(inv));
	send_ci(fw, 0u, std::vector<uint8_t>(inv, inv + invN));
	(void)drain_outputs(fw, 0);

	// Direct GET from peerA now falls back to 512 -> 1 chunk
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 2u,
	                      R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs d2 = drain_outputs(fw, 0);
	E2E_CHECK_EQ(d2.ciMsgs.size(), 1u);
	std::string h2, b2;
	E2E_CHECK(reassemble_pe_reply(d2.ciMsgs, ms::MessageType::PropertyGetDataReply, 2u, fwMuid,
	                              peerA, 512u, h2, b2));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(h2, "status", 0), 200);

	// Re-discover with 4096 -> capped at CI_PE_MESSAGE_MAX (512) -> X-ScaleList fits within 512/chunk
	E2E_CHECK_EQ(discover_fw_muid(fw, peerA, 4096u), fwMuid);
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 3u,
	                      R"({"resource":"X-ScaleList"})"));
	const DrainedOutputs d3 = drain_outputs(fw, 0);
	E2E_CHECK(d3.ciMsgs.size() >= 1u);
	for (const auto& m : d3.ciMsgs) {
		E2E_CHECK(m.size() + 2u <= 512u);
	}
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2
E2E_TEST(3, "T3-06", "F1+F7+F10: Malformed PE 0x3FFF + orphan kContinue -> NAK 0x41 & drop, then 200 ResourceList & 405/200 Sub") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t peerMuid = 0x0306060u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerMuid, 512u);
	E2E_CHECK(fwMuid != 0u);

	const uint32_t drops0 = fw.reassembly_drops();

	// Orphan kContinue on group 0 + malformed PE 0x34 HeaderByteCount=0x3FFF
	const uint32_t orphanCont[2] = {0x30231122u, 0x33445566u};
	fw.send_ump(orphanCont, 2u);

	std::vector<uint8_t> badPe(ms::PropertyExchangeFixedByteCount + 4u, 0u);
	ms::WriteCommonHeader(badPe.data(), badPe.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataInquiry, peerMuid, fwMuid);
	badPe[13] = 5u;
	ms::WriteFourteenBitValue(&badPe[14], 0x3FFFu);
	send_ci(fw, 0u, badPe);

	const DrainedOutputs badOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(badOut.ciMsgs.size(), 1u);
	if (badOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(badOut.ciMsgs[0].data(), badOut.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.OriginalMessageType,
		             static_cast<uint8_t>(ms::MessageType::PropertyGetDataInquiry));
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageMalformed);
	}

	// Clean GET ResourceList -> 200 with DeviceInfo & ChannelList
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerMuid, fwMuid, 12u,
	                      R"({"resource":"ResourceList"})"));
	const DrainedOutputs rlOut = drain_outputs(fw, 0);
	std::string rlHdr, rlBody;
	E2E_CHECK(reassemble_pe_reply(rlOut.ciMsgs, ms::MessageType::PropertyGetDataReply, 12u, fwMuid,
	                              peerMuid, 512u, rlHdr, rlBody));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(rlHdr, "status", 0), 200);
	E2E_CHECK(rlBody.find("\"DeviceInfo\"") != std::string::npos);
	E2E_CHECK(rlBody.find("\"ChannelList\"") != std::string::npos);

	// Subscription: DeviceInfo -> 405, X-ScaleSettings -> 200 with subscribeId "lp..."
	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, peerMuid, fwMuid, 13u,
	                      R"({"command":"start","resource":"DeviceInfo"})"));
	const DrainedOutputs sub405 = drain_outputs(fw, 0);
	std::string h405, b405;
	E2E_CHECK(reassemble_pe_reply(sub405.ciMsgs, ms::MessageType::PropertySubscriptionReply, 13u,
	                              fwMuid, peerMuid, 512u, h405, b405));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(h405, "status", 0), 405);

	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, peerMuid, fwMuid, 14u,
	                      R"({"command":"start","resource":"X-ScaleSettings"})"));
	const DrainedOutputs sub200 = drain_outputs(fw, 0);
	std::string h200, b200;
	E2E_CHECK(reassemble_pe_reply(sub200.ciMsgs, ms::MessageType::PropertySubscriptionReply, 14u,
	                              fwMuid, peerMuid, 512u, h200, b200));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(h200, "status", 0), 200);
	const std::string subId = m2s::ci::ciJsonString(h200, "subscribeId");
	E2E_CHECK(subId.rfind("lp", 0) == 0);
}
#endif

#if E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(3, "T3-07", "F12+F14: Automatic boot Discovery via negotiate_ump_stream populates Launchpad and CiAgent") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "M2S-T3-07";
	hostId.maxSysEx = 4096u;
	e2e::WirePair wire(fw, hostId, 0x0307070u);

	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	E2E_CHECK(wire.settle(256));

	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK(snap.peers[0].discoveredUs);
	}

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK_EQ(info.muid, 0x0307070u);
	const uint8_t wantFlags = static_cast<uint8_t>(
	    CI_PEER_FLAG_ENDPOINT | CI_PEER_FLAG_PE_CAPS | CI_PEER_FLAG_DEVICE_INFO |
	    CI_PEER_FLAG_COMPLETE);
	E2E_CHECK_EQ(static_cast<uint8_t>(info.flags & wantFlags), wantFlags);
	E2E_CHECK_EQ(info.device_info_status, 200u);
	E2E_CHECK(std::strcmp(info.product_instance_id, "M2S-T3-07") == 0);
}
#endif

#if E2E_HAS_M3
E2E_TEST(3, "T3-08", "F13+F14+F15: Setup trigger, 2999ms vs 3005ms Discovery window, and LED transitions") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.tap_setup(50);

	fw.press_pad(static_cast<uint8_t>(BTN_MIDICI), 127);
	const uint32_t t0 = midici_initiator_now_ms();
	fw.advance_ms(20);
	fw.release_pad(static_cast<uint8_t>(BTN_MIDICI));
	const auto disc = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(disc.size(), 1u);
	const uint32_t ourMuid = midici_muid();

	// Advance to t0 + 100 ms: Peer 1 (non-PE 0x00)
	fw.advance_ms(100u - (midici_initiator_now_ms() - t0));
	send_ci(fw, 0, make_discovery_reply(0x0308011u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);

	// Advance to t0 + 2999 ms: Peer 2 (PE 0x08) still inside 3000 ms window
	fw.advance_ms(2999u - (midici_initiator_now_ms() - t0));
	E2E_CHECK(midici_initiator_is_discovering());
	send_ci(fw, 0, make_discovery_reply(0x0308022u, ourMuid, 0x08u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 2u);
	E2E_CHECK(led_is(21u, 63u, 24u, 0u));
	E2E_CHECK(led_is(22u, 63u, 24u, 0u));
	E2E_CHECK(led_off(23u));

	// Advance to t0 + 3005 ms: window expired -> Peer 3 rejected
	fw.advance_ms(3005u - (midici_initiator_now_ms() - t0));
	E2E_CHECK(!midici_initiator_is_discovering());
	send_ci(fw, 0, make_discovery_reply(0x0308033u, ourMuid, 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 2u);
	E2E_CHECK(led_off(23u));

	// Complete Peer 1 -> cyan; complete Peer 2 -> bright green
	send_ci(fw, 0, make_endpoint_reply(0x0308011u, ourMuid, "P1"));
	send_ci(fw, 0, make_endpoint_reply(0x0308022u, ourMuid, "P2"));
	send_ci(fw, 0, make_pe_caps_reply(0x0308022u, ourMuid, 1u));
	const auto reqs = drain_ci_group(fw, 0);
	uint8_t devReq = 0;
	for (const auto& m : reqs) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			devReq = p.PropertyExchange.RequestId;
		}
	}
	E2E_CHECK(devReq != 0u);
	send_ci(fw, 0,
	        make_pe_get_reply(0x0308022u, ourMuid, devReq, R"({"status":200})",
	                          R"({"manufacturer":"Peer2"})"));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK(led_is(21u, 0u, 48u, 63u));
	E2E_CHECK(led_is(22u, 0u, 63u, 0u));
	E2E_CHECK(led_off(23u));
}
#endif

#if E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(3, "T3-09", "F14+F15+F11: Simultaneous Initiator Discovery and Responder 128B chunked DeviceInfo with CiAgent") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	m2s::ci::CiIdentity hostId;
	hostId.maxSysEx = 128u;
	hostId.productInstanceId = "M2S-CONC-09";
	e2e::WirePair wire(fw, hostId, 0x0309090u);

	fw.tap_setup(50);
	fw.tap_pad(static_cast<uint8_t>(BTN_MIDICI), 20);
	E2E_CHECK(wire.agent().discover(nullptr));
	E2E_CHECK(wire.settle(512));

	E2E_CHECK_EQ(midici_tx_overflows(), 0u);
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK_EQ(snap.peers[0].resourceListStatus, 200);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK((info.flags & CI_PEER_FLAG_COMPLETE) != 0u);
	E2E_CHECK_EQ(info.device_info_status, 200u);
	E2E_CHECK(led_is(21u, 0u, 63u, 0u));
}
#endif

#if E2E_HAS_M3
E2E_TEST(3, "T3-10", "F12+F13+F15: Boot -> Setup -> Play MT4 Note On/Off -> Setup reopen preserves peer LEDs") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_discovery_reply(0x0310011u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	fw.tap_setup(50);
	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();

	send_ci(fw, 0, make_discovery_reply(0x0310011u, ourMuid, 0x00u));
	send_ci(fw, 0, make_discovery_reply(0x0310022u, ourMuid, 0x08u));
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_endpoint_reply(0x0310011u, ourMuid, "P1"));
	send_ci(fw, 0, make_endpoint_reply(0x0310022u, ourMuid, "P2"));
	send_ci(fw, 0, make_pe_caps_reply(0x0310022u, ourMuid, 1u));
	const auto reqs = drain_ci_group(fw, 0);
	uint8_t devReq = 0;
	for (const auto& m : reqs) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			devReq = p.PropertyExchange.RequestId;
		}
	}
	E2E_CHECK(devReq != 0u);
	send_ci(fw, 0,
	        make_pe_get_reply(0x0310022u, ourMuid, devReq, R"({"status":200})",
	                          R"({"manufacturer":"P2"})"));
	(void)drain_ci_group(fw, 0);

	// Exit Setup -> Play mode -> play pad 11 (C4 = pitch 60 at default octave 4)
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 0u);
	arm_pad_adc(11u, 800u, 2800u);
	fw.press_pad(11u, 127u);
	fw.advance_ms(10);
	fw.release_pad(11u);
	fw.advance_ms(2);

	const auto pkts = fw.drain_tx_packets();
	int noteOn = 0, noteOff = 0;
	for (const auto& p : pkts) {
		if (m2s::umpType(p) == 0x4u) {
			const uint8_t st = static_cast<uint8_t>((p.words[0] >> 20) & 0xFu);
			const uint8_t note = static_cast<uint8_t>((p.words[0] >> 8) & 0x7Fu);
			if (st == 0x9u && note == 60u) ++noteOn;
			if (st == 0x8u && note == 60u) ++noteOff;
		}
	}
	E2E_CHECK_EQ(noteOn, 1);
	E2E_CHECK_EQ(noteOff, 1);

	// Reopen Setup -> PAGE_MIDICI peer LEDs restore
	fw.tap_setup(50);
	E2E_CHECK_EQ(ui_menu_open(), 1u);
	E2E_CHECK(led_is(21u, 0u, 48u, 63u));
	E2E_CHECK(led_is(22u, 0u, 63u, 0u));
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2
E2E_TEST(3, "T3-11", "F3+F11+USB TX: Concurrent 128B PE chunks, 8x 0xF8 clocks, 4x MT4 notes, and ST_EP_DISCOVERY headroom") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t peerMuid = 0x0311000u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerMuid, 128u);
	E2E_CHECK(fwMuid != 0u);

	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerMuid, fwMuid, 21u,
	                      R"({"resource":"DeviceInfo"})"));

	// Concurrently inject 8x 0xF8 clocks, 4 pad attacks/releases, and ST_EP_DISCOVERY(0x01)
	for (int i = 0; i < 8; ++i) {
		hal_send_midi(USBSTANDALONE, 0xF8u, 0u, 0u);
	}
	const uint8_t pads[4] = {11u, 12u, 13u, 14u};
	for (uint8_t p : pads) {
		arm_pad_adc(p, 800u, 2800u);
		fw.press_pad(p, 127u);
	}
	fw.advance_ms(10);
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	for (uint8_t p : pads) {
		fw.release_pad(p);
	}
	fw.advance_ms(2);

	std::vector<std::vector<uint8_t>> ciMsgs;
	m2s::ci::SysEx7Assembler asmblr;
	int epInfoCount = 0, clockCount = 0, noteOnCount = 0, noteOffCount = 0;

	int idle = 0;
	for (int pass = 0; pass < 96; ++pass) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		for (const auto& p : pkts) {
			const uint8_t mt = m2s::umpType(p);
			if (mt == 0xFu && p.words[0] == 0xF0010101u) ++epInfoCount;
			else if (mt == 0x1u && p.words[0] == 0x10F80000u) ++clockCount;
			else if (mt == 0x4u) {
				const uint8_t st = static_cast<uint8_t>((p.words[0] >> 20) & 0xFu);
				if (st == 0x9u) ++noteOnCount;
				else if (st == 0x8u) ++noteOffCount;
			} else if (mt == 0x3u && m2s::umpGroup(p) == 0u) {
				std::vector<uint8_t> msg;
				if (asmblr.feed(p, msg)) ciMsgs.push_back(std::move(msg));
			}
		}
		if (pkts.empty() && midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			if (++idle >= 3) break;
		} else {
			idle = 0;
		}
	}

	E2E_CHECK_EQ(epInfoCount, 1);
	E2E_CHECK_EQ(clockCount, 8);
	E2E_CHECK_EQ(noteOnCount, 4);
	E2E_CHECK_EQ(noteOffCount, 4);
	E2E_CHECK_EQ(ump_tx_reserve_words(), 0u);
	E2E_CHECK_EQ(ciMsgs.size(), 3u);

	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(ciMsgs, ms::MessageType::PropertyGetDataReply, 21u, fwMuid,
	                              peerMuid, 128u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(m2s::ci::ciJsonString(body, "serialNumber") == "TEST0001");
}
#endif

#if E2E_HAS_M2 && E2E_HAS_M3
E2E_TEST(3, "T3-12", "F17+F7+F14: Mutual discovery -> InvalidateMuid purges CiAgent from Launchpad -> re-scan recovers") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "M2S-T3-12";
	e2e::WirePair wire(fw, hostId, 0x0312121u);
	fw.trigger_negotiated_discovery();
	E2E_CHECK(wire.discover_from_agent());

	E2E_CHECK_EQ(fw.peer_count(), 1u);
	E2E_CHECK_EQ(wire.agent().snapshot().peers.size(), 1u);

	// Invalidate CiAgent's MUID -> Launchpad removes peer 0x0312121u from peer table
	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(0x0777777u, 0x0312121u, inv, sizeof(inv));
	wire.inject_sysex7_to_fw(0, inv, static_cast<uint16_t>(invN));
	E2E_CHECK(wire.settle(128));
	E2E_CHECK_EQ(fw.peer_count(), 0u);

	// Re-scan recovers peer cleanly
	fw.start_discovery();
	E2E_CHECK(wire.settle(256));
	E2E_CHECK_EQ(fw.peer_count(), 1u);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK_EQ(info.muid, 0x0312121u);
	E2E_CHECK((info.flags & CI_PEER_FLAG_COMPLETE) != 0u);
}

E2E_TEST(3, "T3-13", "F14(0x04)+F7: 0x04|0x08 peer triggers ProfileInquiry TX and Responder NAKs incoming ProfileInquiry 0x01") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	const uint32_t peerMuid = 0x0313313u;
	send_ci(fw, 0, make_discovery_reply(peerMuid, ourMuid, 0x04u | 0x08u, 512u));

	// Simultaneously send ProfileInquiry (0x20) TO Launchpad Responder
	uint8_t profInq[ms::CommonHeaderByteCount] = {};
	ms::WriteCommonHeader(profInq, sizeof(profInq), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::ProfileInquiry, peerMuid, ourMuid);
	send_ci(fw, 0, std::vector<uint8_t>(profInq, profInq + sizeof(profInq)));

	const auto outMsgs = drain_ci_group(fw, 0, 32);
	bool sawEpInq = false, sawProfInq = false, sawPeCapsInq = false, sawNak01 = false;
	for (const auto& m : outMsgs) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok) continue;
		if (p.Type == ms::MessageType::EndpointInquiry && p.DestinationMuid == peerMuid) {
			sawEpInq = true;
		} else if (p.Type == ms::MessageType::ProfileInquiry && p.DestinationMuid == peerMuid) {
			sawProfInq = true;
		} else if (p.Type == ms::MessageType::PropertyExchangeCapabilitiesInquiry &&
		           p.DestinationMuid == peerMuid) {
			sawPeCapsInq = true;
		} else if (p.Type == ms::MessageType::Nak && p.HasAcknowledgmentFields &&
		           p.DestinationMuid == peerMuid &&
		           p.Acknowledgment.OriginalMessageType ==
		               static_cast<uint8_t>(ms::MessageType::ProfileInquiry) &&
		           p.Acknowledgment.StatusCode == ms::NakStatusMessageNotSupported) {
			sawNak01 = true;
		}
	}
	E2E_CHECK(sawEpInq);
	E2E_CHECK(sawProfInq);
	E2E_CHECK(sawPeCapsInq);
	E2E_CHECK(sawNak01);

	send_ci(fw, 0, make_profile_reply(peerMuid, ourMuid));
	(void)drain_ci_group(fw, 0);
	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK((info.flags & CI_PEER_FLAG_PROFILES) != 0u);
	E2E_CHECK_EQ(info.profiles_enabled, 1u);
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M2
E2E_TEST(3, "T3-14", "F18+F1+F2+F5: 500 corrupt PRNG MT3 packets increment drops, then CiAgent discovers DeviceInfo 200") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t drops0 = fw.reassembly_drops();
	SplitMix64 rng(0x31415926ULL);
	std::vector<uint32_t> corrupt;
	corrupt.reserve(32);
	for (int i = 0; i < 500; ++i) {
		const uint8_t st = static_cast<uint8_t>((rng.next_u8() % 5u) + 2u);  // 2..6 (includes invalid/orphans)
		const uint8_t cnt = static_cast<uint8_t>(rng.next_u8() % 10u);
		const uint32_t w0 = 0x30000000u | (static_cast<uint32_t>(rng.next_u8() & 0x0Fu) << 24) |
		                    (static_cast<uint32_t>(st) << 20) |
		                    (static_cast<uint32_t>(cnt) << 16) |
		                    (static_cast<uint32_t>(rng.next_u8()) << 8) | rng.next_u8();
		corrupt.push_back(w0);
		corrupt.push_back(rng.next_u32());
		if (corrupt.size() >= 16u || i + 1 == 500) {
			fw.send_ump(corrupt.data(), static_cast<uint16_t>(corrupt.size()));
			fw.poll(2);
			(void)fw.drain_ump();
			corrupt.clear();
		}
	}
	E2E_CHECK(fw.reassembly_drops() > drops0);

	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0314141u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK(snap.peers[0].answeredDiscovery);
		E2E_CHECK(snap.peers[0].hasEndpointInfo);
		E2E_CHECK_EQ(snap.peers[0].resourceListStatus, 200);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
}
#endif

#if E2E_HAS_M1 && E2E_HAS_M3
E2E_TEST(3, "T3-15", "F2+F12+USB Reset: Partial SysEx7 across set_alt(0)->set_alt(1) drops cleanly and re-emits boot Discovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	send_ci(fw, 0, make_discovery_reply(0x0315011u, midici_muid(), 0x00u));
	(void)drain_ci_group(fw, 0);
	E2E_CHECK_EQ(fw.peer_count(), 1u);

	const uint32_t drops0 = fw.reassembly_drops();
	// Inject incomplete kStart on group 0 right before alt reset
	const uint32_t partialStart[2] = {0x30167E7Fu, 0x0D700201u};
	fw.send_ump(partialStart, 2u);
	fw.poll(1);

	fw.set_alt(0);
	fw.poll(2);
	fw.set_alt(1);
	fw.poll(2);

	E2E_CHECK_EQ(fw.peer_count(), 0u);

	// Post-reset kEnd on group 0 must NOT stitch with pre-reset kStart -> increments drops by 1
	const uint32_t postEnd[2] = {0x30320102u, 0u};
	fw.send_ump(postEnd, 2u);
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);

	// Re-negotiate UMP Stream -> automatic boot Discovery fires cleanly
	fw.negotiate_ump_stream(UMP_PROTO_MIDI2);
	const auto bootMsgs = drain_ci_group(fw, 0);
	E2E_CHECK_EQ(bootMsgs.size(), 1u);
	if (bootMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(bootMsgs[0].data(), bootMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Discovery);
	}
}
#endif

#if E2E_HAS_M2
E2E_TEST(3, "T3-16", "F5+F10+UID: 24-char serial matches across UMP ST_PRODUCT_ID, MIDI-CI EndpointReply, and PE DeviceInfo") {
	const char* serial24 = "00112233445566778899AABB";
	e2e::FirmwareNode fw;
	fw.set_serial(serial24);
	fw.boot(true, 1);

	const auto epDisc = e2e::make_ep_discovery(0x08u);
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	const DrainedOutputs streamOut = drain_outputs(fw, 0);
	std::string umpProdId;
	for (const auto& s : streamOut.streamMsgs) {
		if (((s[0] >> 16) & 0x3FFu) == 0x004u) {
			const uint8_t c0 = static_cast<uint8_t>((s[0] >> 8) & 0xFFu);
			const uint8_t c1 = static_cast<uint8_t>(s[0] & 0xFFu);
			if (c0) umpProdId.push_back(static_cast<char>(c0));
			if (c1) umpProdId.push_back(static_cast<char>(c1));
			for (size_t w = 1; w < 4; ++w) {
				for (int sh = 24; sh >= 0; sh -= 8) {
					const char c = static_cast<char>((s[w] >> sh) & 0xFFu);
					if (c) umpProdId.push_back(c);
				}
			}
		}
	}
	E2E_CHECK(umpProdId == serial24);

	const uint32_t peerMuid = 0x0316161u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerMuid, 512u);
	E2E_CHECK(fwMuid != 0u);

	std::vector<uint8_t> epInq(ms::EndpointInquiryByteCount, 0u);
	epInq.resize(ms::BuildEndpointInquiry(peerMuid, fwMuid, ms::EndpointStatusProductInstanceId,
	                                      epInq.data(), epInq.size()));
	send_ci(fw, 0u, epInq);
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	std::string ciEpPiid;
	if (epOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(epOut.ciMsgs[0].data(), epOut.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::EndpointReply);
		ciEpPiid.assign(
		    reinterpret_cast<const char*>(epOut.ciMsgs[0].data() + p.Endpoint.InformationOffset),
		    p.Endpoint.InformationByteCount);
	}
	E2E_CHECK(ciEpPiid == serial24);

	send_ci(fw, 0u,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerMuid, fwMuid, 31u,
	                      R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs peOut = drain_outputs(fw, 0);
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(peOut.ciMsgs, ms::MessageType::PropertyGetDataReply, 31u, fwMuid,
	                              peerMuid, 512u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(m2s::ci::ciJsonString(body, "serialNumber") == serial24);
}
#endif

#if E2E_HAS_M3
E2E_TEST(3, "T3-17", "F8+F14: v1 (0x01) 29B DiscoveryReply triggers v2 Launchpad follow-ups and completes v1 PE DeviceInfo") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	fw.start_discovery();
	(void)drain_ci_group(fw, 0);
	const uint32_t ourMuid = midici_muid();

	const uint32_t peerV1 = 0x0317171u;
	send_ci(fw, 0, make_discovery_reply(peerV1, ourMuid, 0x08u, 512u, 0x7Fu, ms::MessageVersion11));
	const auto f1 = drain_ci_group(fw, 0);
	bool sawV2Ep = false, sawV2Caps = false;
	for (const auto& m : f1) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) != ms::ParseStatus::Ok) continue;
		if (p.Type == ms::MessageType::EndpointInquiry && p.VersionFormat == MIDI_CI_VERSION) {
			sawV2Ep = true;
		}
		if (p.Type == ms::MessageType::PropertyExchangeCapabilitiesInquiry &&
		    p.VersionFormat == MIDI_CI_VERSION) {
			sawV2Caps = true;
		}
	}
	E2E_CHECK(sawV2Ep);
	E2E_CHECK(sawV2Caps);

	send_ci(fw, 0, make_endpoint_reply(peerV1, ourMuid, "V1-PEER"));
	send_ci(fw, 0, make_pe_caps_reply(peerV1, ourMuid, 1u, ms::MessageVersion11));
	const auto f2 = drain_ci_group(fw, 0);
	uint8_t devReq = 0;
	for (const auto& m : f2) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			E2E_CHECK_EQ(p.VersionFormat, MIDI_CI_VERSION);
			devReq = p.PropertyExchange.RequestId;
		}
	}
	E2E_CHECK(devReq != 0u);
	send_ci(fw, 0,
	        make_pe_get_reply(peerV1, ourMuid, devReq, R"({"status":200})",
	                          R"({"manufacturer":"LegacyV1"})"));
	(void)drain_ci_group(fw, 0);

	ci_peer_info_t info{};
	E2E_CHECK(fw.get_peer(0, &info));
	E2E_CHECK_EQ(info.version, ms::MessageVersion11);
	E2E_CHECK_EQ(info.function_block, 0x7Fu);
	E2E_CHECK((info.flags & CI_PEER_FLAG_DEVICE_INFO) != 0u);
	E2E_CHECK((info.flags & CI_PEER_FLAG_COMPLETE) != 0u);
	E2E_CHECK_EQ(info.device_info_status, 200u);
}

E2E_TEST(3, "T3-18", "F13+F15+Play UI: Held pad emits Note Off on Setup entry; PAGE_MIDICI pad 11/21 resist note LED overwrite") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// Hold pad 11 (C4 = pitch 60) past PEAK_WINDOW_MS=3
	arm_pad_adc(11u, 800u, 2800u);
	fw.press_pad(11u, 127u);
	fw.advance_ms(10);

	const auto onPkts = fw.drain_tx_packets();
	bool sawNoteOn = false;
	for (const auto& p : onPkts) {
		if (m2s::umpType(p) == 0x4u && ((p.words[0] >> 20) & 0xFu) == 0x9u &&
		    ((p.words[0] >> 8) & 0x7Fu) == 60u) {
			sawNoteOn = true;
		}
	}
	E2E_CHECK(sawNoteOn);
	E2E_CHECK(led_is(11u, 63u, 63u, 63u));

	// Open Setup while pad 11 is held -> pe_release_all emits Note Off, then BTN_MIDICI starts scan
	fw.tap_setup(50);
	const auto offPkts = fw.drain_tx_packets();
	bool sawNoteOff = false;
	for (const auto& p : offPkts) {
		if (m2s::umpType(p) == 0x4u && ((p.words[0] >> 20) & 0xFu) == 0x8u &&
		    ((p.words[0] >> 8) & 0x7Fu) == 60u) {
			sawNoteOff = true;
		}
	}
	E2E_CHECK(sawNoteOff);

	(void)trigger_midici_setup_discovery(fw);
	const uint32_t ourMuid = midici_muid();
	E2E_CHECK(led_is(11u, 0u, 48u, 63u));

	// Complete PE peer -> pad 21 turns bright green, pad 11 remains PAGE_MIDICI (not sounding white/blue)
	send_ci(fw, 0, make_discovery_reply(0x0318011u, ourMuid, 0x08u));
	send_ci(fw, 0, make_endpoint_reply(0x0318011u, ourMuid, "P18"));
	send_ci(fw, 0, make_pe_caps_reply(0x0318011u, ourMuid, 1u));
	const auto reqs = drain_ci_group(fw, 0);
	uint8_t devReq = 0;
	for (const auto& m : reqs) {
		ms::ParsedMessage p{};
		if (ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok &&
		    p.Type == ms::MessageType::PropertyGetDataInquiry && p.HasPropertyExchangeFields) {
			devReq = p.PropertyExchange.RequestId;
		}
	}
	E2E_CHECK(devReq != 0u);
	send_ci(fw, 0,
	        make_pe_get_reply(0x0318011u, ourMuid, devReq, R"({"status":200})",
	                          R"({"manufacturer":"P18"})"));
	(void)drain_ci_group(fw, 0);

	E2E_CHECK(led_is(21u, 0u, 63u, 0u));
	E2E_CHECK(led_is(11u, 0u, 48u, 63u));
	fw.release_pad(11u);
	fw.advance_ms(2);
}
#endif
