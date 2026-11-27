#include "e2e_harness.h"
#include "e2e_stubs.h"

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
#include "platform/platform.h"
#include "ump/ump.h"
}

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if E2E_HAS_M2

namespace {

namespace ms = m2s::ci::ms;

struct DrainedOutputs {
	std::vector<std::vector<uint8_t>> ciMsgs;
	std::vector<std::vector<uint8_t>> otherGroupCiMsgs;
	std::vector<std::array<uint32_t, 4>> streamMsgs;
	std::vector<std::pair<bool, size_t>> orderLog;  // (isStream, index)
};

void send_ci(e2e::FirmwareNode& fw, uint8_t group, const std::vector<uint8_t>& payload) {
	const std::vector<uint32_t> words = e2e::pack_sysex7_words(group, payload);
	fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
}

void feed_pkts_to_result(const std::vector<m2s::UmpPacket>& pkts, uint8_t group,
                         m2s::ci::SysEx7Assembler& mainAsm,
                         m2s::ci::SysEx7Assembler& otherAsm, DrainedOutputs& out) {
	for (const auto& p : pkts) {
		const uint8_t mt = m2s::umpType(p);
		if (mt == 0xFu && p.wordCount == 4) {
			out.orderLog.emplace_back(true, out.streamMsgs.size());
			out.streamMsgs.push_back({p.words[0], p.words[1], p.words[2], p.words[3]});
		} else if (mt == 0x3u && p.wordCount == 2) {
			std::vector<uint8_t> msg;
			if (m2s::umpGroup(p) == (group & 0xFu)) {
				if (mainAsm.feed(p, msg)) {
					out.orderLog.emplace_back(false, out.ciMsgs.size());
					out.ciMsgs.push_back(std::move(msg));
				}
			} else {
				if (otherAsm.feed(p, msg)) out.otherGroupCiMsgs.push_back(std::move(msg));
			}
		}
	}
}

// Allow the expanded ResourceList to drain even with 128-byte PE chunks.
DrainedOutputs drain_outputs(e2e::FirmwareNode& fw, uint8_t group = 0, int maxPolls = 256) {
	DrainedOutputs out;
	m2s::ci::SysEx7Assembler mainAsm;
	m2s::ci::SysEx7Assembler otherAsm;
	int idle = 0;
	for (int i = 0; i < maxPolls; ++i) {
		fw.poll_once();
		const auto pkts = fw.drain_tx_packets();
		const bool hadPkts = !pkts.empty();
		if (hadPkts) {
			feed_pkts_to_result(pkts, group, mainAsm, otherAsm, out);
		}
		if (!hadPkts && midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			if (++idle >= 3) break;
		} else {
			idle = 0;
		}
	}
	return out;
}

DrainedOutputs drain_tx_only_no_poll(e2e::FirmwareNode& fw, uint8_t group = 0) {
	DrainedOutputs out;
	m2s::ci::SysEx7Assembler mainAsm;
	m2s::ci::SysEx7Assembler otherAsm;
	int idle = 0;
	for (int i = 0; i < 64; ++i) {
		if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
		__wrap_usbmidi_poll();
		const auto pkts = fw.drain_tx_packets();
		const bool hadPkts = !pkts.empty();
		if (hadPkts) {
			feed_pkts_to_result(pkts, group, mainAsm, otherAsm, out);
		}
		if (!hadPkts && midici_tx_pending() == 0 && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			if (++idle >= 2) break;
		} else {
			idle = 0;
		}
	}
	return out;
}

std::vector<uint8_t> make_discovery_bytes(uint32_t srcMuid, uint32_t maxSysEx,
                                          uint8_t ver = 0x02u, uint8_t outputPathId = 0x00u,
                                          uint8_t devId = 0x7Fu,
                                          uint32_t dstMuid = ms::MuidBroadcast) {
	const size_t len = (ver == ms::MessageVersion11) ? ms::DiscoveryByteCountVersion11
	                                                 : ms::DiscoveryByteCount;
	std::vector<uint8_t> buf(len, 0u);
	buf[0] = ms::UniversalSystemExclusiveId;
	buf[1] = devId & 0x7Fu;
	buf[2] = ms::SubId1CapabilityInquiry;
	buf[3] = static_cast<uint8_t>(ms::MessageType::Discovery);
	buf[4] = ver & 0x7Fu;
	ms::WriteMuid(&buf[5], srcMuid);
	ms::WriteMuid(&buf[9], dstMuid);
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

uint32_t discover_fw_muid(e2e::FirmwareNode& fw, uint32_t initiatorMuid = 0x01020304u,
                          uint32_t maxSysEx = 4096u, uint8_t outputPathId = 0x00u) {
	send_ci(fw, 0, make_discovery_bytes(initiatorMuid, maxSysEx, 0x02u, outputPathId));
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
                                   uint16_t chunkNum = 1u, const std::string& data = "",
                                   uint8_t ver = 0x02u, uint8_t devId = 0x7Fu) {
	std::vector<uint8_t> buf(ms::PropertyExchangeFixedByteCount + hdr.size() + data.size(), 0u);
	size_t n = 0;
	buf[n++] = ms::UniversalSystemExclusiveId;
	buf[n++] = devId & 0x7Fu;
	buf[n++] = ms::SubId1CapabilityInquiry;
	buf[n++] = static_cast<uint8_t>(type);
	buf[n++] = ver & 0x7Fu;
	ms::WriteMuid(&buf[n], src);
	n += 4;
	ms::WriteMuid(&buf[n], dst);
	n += 4;
	buf[n++] = reqId & 0x7Fu;
	ms::WriteFourteenBitValue(&buf[n], static_cast<uint16_t>(hdr.size()));
	n += 2;
	for (char ch : hdr) {
		buf[n++] = static_cast<uint8_t>(ch) & 0x7Fu;
	}
	ms::WriteFourteenBitValue(&buf[n], chunkCount);
	n += 2;
	ms::WriteFourteenBitValue(&buf[n], chunkNum);
	n += 2;
	ms::WriteFourteenBitValue(&buf[n], static_cast<uint16_t>(data.size()));
	n += 2;
	for (char ch : data) {
		buf[n++] = static_cast<uint8_t>(ch) & 0x7Fu;
	}
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
		if (pe.DataByteCount > 0u) {
			outBody.append(reinterpret_cast<const char*>(m.data() + pe.DataOffset),
			               pe.DataByteCount);
		}
	}
	return true;
}

std::string render_expected_pe(uint8_t res, const struct settings* s, const char* piid) {
	const uint16_t len = ci_pe_body_len(res, s, piid);
	std::string out(len, '\0');
	struct ci_json_out jo {};
	ci_json_out_init(&jo, out.data(), 0, len);
	ci_pe_render(res, s, piid, &jo);
	return out;
}

}  // namespace

// ============================================================================
// Tier 2 — F5 Boundaries (T2-F5-01..05)
// ============================================================================

E2E_TEST(2, "T2-F5-01", "Discovery DestinationMuid filtering (broadcast required, unicast/foreign ignored)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x00u, 0x7Fu, fwMuid));
	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x00u, 0x7Fu, 0x0111111u));
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x00u, 0x7Fu, ms::MuidBroadcast));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 1u);
}

E2E_TEST(2, "T2-F5-02", "EndpointInquiry unsupported status (0x01, 0x7F) and truncated 13B header -> NAK 0x01 / 0x41") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	for (uint8_t st : {uint8_t{0x01u}, uint8_t{0x7Fu}}) {
		uint8_t req[ms::EndpointInquiryByteCount] = {};
		const size_t n = ms::BuildEndpointInquiry(0x01020304u, fwMuid, st, req, sizeof(req));
		send_ci(fw, 0, std::vector<uint8_t>(req, req + n));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.Acknowledgment.OriginalMessageType, 0x72u);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageNotSupported);
		}
	}

	// Truncated 13-byte EndpointInquiry (missing status byte) -> NAK 0x41
	std::vector<uint8_t> trunc(ms::CommonHeaderByteCount, 0u);
	ms::WriteCommonHeader(trunc.data(), trunc.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::EndpointInquiry, 0x01020304u, fwMuid);
	send_ci(fw, 0, trunc);
	const DrainedOutputs truncOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(truncOut.ciMsgs.size(), 1u);
	if (truncOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(truncOut.ciMsgs[0].data(), truncOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageMalformed);
	}
}

E2E_TEST(2, "T2-F5-03", "ProductInstanceId 7-bit printable ASCII & invalid SourceMuid rejection") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	uint8_t req[ms::EndpointInquiryByteCount] = {};
	ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, req, sizeof(req));
	send_ci(fw, 0, std::vector<uint8_t>(req, req + sizeof(req)));
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	if (epOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(epOut.ciMsgs[0].data(), epOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		const uint16_t len = p.Endpoint.InformationByteCount;
		E2E_CHECK(len >= 1u && len <= ms::ProductInstanceIdMaximumByteCount);
		const uint8_t* ptr = epOut.ciMsgs[0].data() + p.Endpoint.InformationOffset;
		E2E_CHECK(ms::ProductInstanceIdIsValid(ptr, len));
	}

	E2E_CHECK_EQ(midici_muid_valid(0x0000000u), 0);
	for (uint32_t badSrc : {0x0FFFFF00u, 0x0FFFFF80u, 0x0FFFFFFFu}) {
		send_ci(fw, 0, make_discovery_bytes(badSrc, 512u));
		uint8_t epBad[ms::EndpointInquiryByteCount] = {};
		ms::BuildEndpointInquiry(badSrc, fwMuid, 0x00u, epBad, sizeof(epBad));
		send_ci(fw, 0, std::vector<uint8_t>(epBad, epBad + sizeof(epBad)));
	}
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());
}

E2E_TEST(2, "T2-F5-04", "DeviceId filtering (0x00, 0x7E, 0x10..0x7D)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	// Discovery and EndpointInquiry with DeviceId != 0x7F -> ignored
	for (uint8_t devId : {uint8_t{0x00u}, uint8_t{0x7Eu}}) {
		send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x00u, devId));
		std::vector<uint8_t> ep(ms::EndpointInquiryByteCount, 0u);
		ms::WriteCommonHeader(ep.data(), ep.size(), devId, ms::MessageType::EndpointInquiry,
		                      0x01020304u, fwMuid);
		ep[13] = 0x00u;
		send_ci(fw, 0, ep);
	}
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());

	// DeviceId in 0x10..0x7D -> ignored even for PE/Profile
	for (uint8_t devId : {uint8_t{0x10u}, uint8_t{0x7Du}}) {
		send_ci(fw, 0,
		        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 1u,
		                      R"({"resource":"DeviceInfo"})", 1u, 1u, "", 0x02u, devId));
	}
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());

	// PE Get with DeviceId 0x00 and 0x7E -> NAK 0x01; ProfileInquiry 0x00 -> NAK 0x01
	for (uint8_t devId : {uint8_t{0x00u}, uint8_t{0x7Eu}}) {
		send_ci(fw, 0,
		        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 1u,
		                      R"({"resource":"DeviceInfo"})", 1u, 1u, "", 0x02u, devId));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageNotSupported);
		}
	}

	uint8_t prof[32] = {};
	ms::ProfileMessageFields pf{};
	pf.Type = ms::MessageType::ProfileInquiry;
	pf.DeviceId = 0x00u;
	pf.SourceMuid = 0x01020304u;
	pf.DestinationMuid = fwMuid;
	const size_t profN = ms::BuildProfileMessage(pf, prof, sizeof(prof));
	send_ci(fw, 0, std::vector<uint8_t>(prof, prof + profN));
	const DrainedOutputs profOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(profOut.ciMsgs.size(), 1u);
	if (profOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(profOut.ciMsgs[0].data(), profOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageNotSupported);
	}
}

E2E_TEST(2, "T2-F5-05", "Non-management inquiries addressed to foreign or broadcast MUID are ignored") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	(void)discover_fw_muid(fw);

	for (uint32_t dst : {0x0222222u, ms::MuidBroadcast}) {
		uint8_t ep[ms::EndpointInquiryByteCount] = {};
		const size_t epN = ms::BuildEndpointInquiry(0x01020304u, dst, 0x00u, ep, sizeof(ep));
		send_ci(fw, 0, std::vector<uint8_t>(ep, ep + epN));

		std::vector<uint8_t> caps(ms::PropertyExchangeCapabilitiesByteCount, 0u);
		ms::WriteCommonHeader(caps.data(), caps.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
		                      dst);
		send_ci(fw, 0, caps);

		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, dst,
		                             1u, R"({"resource":"DeviceInfo"})"));

		uint8_t prof[32] = {};
		ms::ProfileMessageFields pf{};
		pf.Type = ms::MessageType::ProfileInquiry;
		pf.SourceMuid = 0x01020304u;
		pf.DestinationMuid = dst;
		const size_t profN = ms::BuildProfileMessage(pf, prof, sizeof(prof));
		send_ci(fw, 0, std::vector<uint8_t>(prof, prof + profN));
	}
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());
}

// ============================================================================
// Tier 2 — F6 Boundaries (T2-F6-01..05)
// ============================================================================

E2E_TEST(2, "T2-F6-01", "FB 1 (MIDI Port) advertises MIDI-CI version 0x00 while FB 0 advertises 0x02") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const auto fbAll = e2e::make_fb_discovery(0xFFu, 0x01u);
	fw.send_ump(fbAll.data(), static_cast<uint16_t>(fbAll.size()));
	const DrainedOutputs out = drain_outputs(fw, 0);

	bool sawFb0 = false, sawFb1 = false;
	for (const auto& s : out.streamMsgs) {
		if (s[0] == 0xF0118033u) {
			sawFb0 = true;
			E2E_CHECK_EQ(s[1], 0x00010200u);
			E2E_CHECK_EQ((s[1] >> 8) & 0xFFu, 0x02u);
		} else if (s[0] == 0xF011813Bu) {
			sawFb1 = true;
			E2E_CHECK_EQ(s[1], 0x01010000u);
			E2E_CHECK_EQ((s[1] >> 8) & 0xFFu, 0x00u);
		}
	}
	E2E_CHECK(sawFb0);
	E2E_CHECK(sawFb1);
}

E2E_TEST(2, "T2-F6-02", "MIDI-CI inquiries on groups 2..15 (no Function Block) are dropped silently") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	for (uint8_t g : {uint8_t{2u}, uint8_t{15u}}) {
		send_ci(fw, g, make_discovery_bytes(0x01020304u, 512u));
		uint8_t ep[ms::EndpointInquiryByteCount] = {};
		ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, ep, sizeof(ep));
		send_ci(fw, g, std::vector<uint8_t>(ep, ep + sizeof(ep)));
		send_ci(fw, g, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             1u, R"({"resource":"DeviceInfo"})"));
	}
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK(out.ciMsgs.empty());
	E2E_CHECK(out.otherGroupCiMsgs.empty());
	E2E_CHECK_EQ(discover_fw_muid(fw), fwMuid);
}

E2E_TEST(2, "T2-F6-03", "Interleaved group 0 and group 1 MIDI-CI traffic preserves group 0 isolation") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u));
	std::vector<uint8_t> caps1(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	ms::WriteCommonHeader(caps1.data(), caps1.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
	                      0x0111111u);
	send_ci(fw, 1, caps1);
	const DrainedOutputs out1 = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out1.ciMsgs.size(), 1u);
	E2E_CHECK(out1.otherGroupCiMsgs.empty());

	uint32_t fwMuid = 0;
	if (out1.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out1.ciMsgs[0].data(), out1.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		fwMuid = p.SourceMuid;
	}

	uint8_t ep[ms::EndpointInquiryByteCount] = {};
	ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, ep, sizeof(ep));
	send_ci(fw, 0, std::vector<uint8_t>(ep, ep + sizeof(ep)));
	send_ci(fw, 1, make_discovery_bytes(0x01020304u, 512u));
	const DrainedOutputs out2 = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out2.ciMsgs.size(), 1u);
	E2E_CHECK(out2.otherGroupCiMsgs.empty());
	if (out2.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out2.ciMsgs[0].data(), out2.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::EndpointReply);
	}
}

E2E_TEST(2, "T2-F6-04", "OutputPathId 0x00 and 0x7F boundaries in v2 Discovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	for (uint8_t path : {uint8_t{0x00u}, uint8_t{0x7Fu}}) {
		send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, path));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
			if (out.ciMsgs[0].size() >= 30u) {
				E2E_CHECK_EQ(out.ciMsgs[0][29], path);
			}
		}
	}
}

E2E_TEST(2, "T2-F6-05", "Legacy 29B v1 Discovery on group 0 zeroes OutputPathId; short 28B v1 Discovery NAKs 0x41") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x01u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
		if (out.ciMsgs[0].size() >= 30u) {
			E2E_CHECK_EQ(out.ciMsgs[0][29], 0x00u);
		}
	}

	auto shortV1 = make_discovery_bytes(0x01020304u, 512u, 0x01u);
	shortV1.pop_back();  // 28B
	send_ci(fw, 0, shortV1);
	const DrainedOutputs nakOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(nakOut.ciMsgs.size(), 1u);
	if (nakOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(nakOut.ciMsgs[0].data(), nakOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageMalformed);
	}
}

// ============================================================================
// Tier 2 — F7 Boundaries (T2-F7-01..05)
// ============================================================================

E2E_TEST(2, "T2-F7-01", "InvalidateMuid mid-PE-chunking aborts remaining chunks immediately") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x01234561u, 0x02345672u, 0x03456783u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);
	const uint32_t oldMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, oldMuid,
	                             1u, R"({"resource":"ResourceList"})"));
	__wrap_midi_parsers_poll();
	const DrainedOutputs c1 = drain_tx_only_no_poll(fw, 0);
	E2E_CHECK_EQ(c1.ciMsgs.size(), 1u);
	if (c1.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(c1.ciMsgs[0].data(), c1.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, 1u);
		E2E_CHECK(p.PropertyExchange.ChunkCount > 5u);
	}

	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	ms::BuildInvalidateMuid(0x01020304u, oldMuid, inv, sizeof(inv));
	send_ci(fw, 0, std::vector<uint8_t>(inv, inv + sizeof(inv)));
	const DrainedOutputs after = drain_outputs(fw, 0);
	E2E_CHECK(after.ciMsgs.empty());
	bool sawFb0 = false;
	for (const auto& s : after.streamMsgs) {
		if (s[0] == 0xF0118033u && s[1] == 0x00010200u) sawFb0 = true;
	}
	E2E_CHECK(sawFb0);

	const uint32_t newMuid = discover_fw_muid(fw, 0x01020304u, 128u);
	E2E_CHECK(newMuid != oldMuid);
}

E2E_TEST(2, "T2-F7-02", "MuidCollision on Discovery withdraws old MUID, replaces MUID, resets PE, and sends FB0 alert") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x02223331u, 0x03334442u, 0x04445553u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);
	const uint32_t oldMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, 0x01020304u,
	                             oldMuid, 1u, R"({"command":"start","resource":"X-PlayOptions"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 1u);

	// Colliding Discovery with SourceMuid == oldMuid
	send_ci(fw, 0, make_discovery_bytes(oldMuid, 512u));
	const DrainedOutputs colOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(colOut.ciMsgs.size(), 1u);

	uint32_t newMuidFromInv = 0;
	if (colOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(colOut.ciMsgs[0].data(), colOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::InvalidateMuid);
		E2E_CHECK_EQ(p.TargetMuid, oldMuid);
		E2E_CHECK(p.SourceMuid != oldMuid);
		E2E_CHECK(ms::MuidIsUsable(p.SourceMuid));
		newMuidFromInv = p.SourceMuid;
	}

	bool sawFb0 = false;
	size_t fbIdx = 0;
	for (size_t i = 0; i < colOut.orderLog.size(); ++i) {
		if (colOut.orderLog[i].first) {
			const auto& s = colOut.streamMsgs[colOut.orderLog[i].second];
			if (s[0] == 0xF0118033u && s[1] == 0x00010200u) {
				sawFb0 = true;
				fbIdx = i;
				break;
			}
		}
	}
	E2E_CHECK(sawFb0);
	size_t invIdx = 0;
	bool sawInvIdx = false;
	for (size_t i = 0; i < colOut.orderLog.size(); ++i) {
		if (!colOut.orderLog[i].first && colOut.orderLog[i].second == 0u) {
			sawInvIdx = true;
			invIdx = i;
			break;
		}
	}
	E2E_CHECK(sawInvIdx && sawFb0 && invIdx < fbIdx);

	// Old subscription was reset
	struct settings next = *ui_settings();
	next.channel = static_cast<uint8_t>((next.channel + 1u) % 16u);
	ui_apply_settings(&next, fw.clock().now_ms());
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());

	// Old peer maxSysEx=128 was reset -> GET DeviceInfo returns 1 chunk (default 512/320)
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u,
	                             newMuidFromInv, 2u, R"({"resource":"DeviceInfo"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 1u);
}

E2E_TEST(2, "T2-F7-03", "InvalidateMuid targeting unknown or broadcast MUID preserves our MUID") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	for (uint32_t target : {0x0999999u, ms::MuidBroadcast}) {
		uint8_t inv[ms::InvalidateMuidByteCount] = {};
		ms::BuildInvalidateMuid(0x01020304u, target, inv, sizeof(inv));
		send_ci(fw, 0, std::vector<uint8_t>(inv, inv + sizeof(inv)));
	}
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK(out.ciMsgs.empty());
	E2E_CHECK(out.streamMsgs.empty());
	E2E_CHECK_EQ(discover_fw_muid(fw), fwMuid);
}

E2E_TEST(2, "T2-F7-04", "Replies, Reports, ACK, NAK, PropertyNotify, and Unknown 0x55 never trigger NAK") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	const uint8_t types[] = {0x71u, 0x73u, 0x21u, 0x31u, 0x35u, 0x37u,
	                         0x39u, 0x3Fu, 0x7Du, 0x7Fu, 0x55u};
	for (uint8_t t : types) {
		std::vector<uint8_t> msg(32u, 0u);
		ms::WriteCommonHeader(msg.data(), msg.size(), ms::DeviceIdFunctionBlock,
		                      static_cast<ms::MessageType>(t), 0x01020304u, fwMuid, 0x02u);
		if (t == 0x35u || t == 0x37u || t == 0x39u || t == 0x3Fu) {
			msg.resize(ms::PropertyExchangeFixedByteCount, 0u);
			msg[13] = 1u;
			ms::WriteFourteenBitValue(&msg[16], 1u);
			ms::WriteFourteenBitValue(&msg[18], 1u);
		}
		send_ci(fw, 0, msg);
	}
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());
}

E2E_TEST(2, "T2-F7-05", "Reserved entropy rejection (0, 0x0FFFFF00..0x0FFFFFFF) across 20 MUID invalidations") {
	e2e::FirmwareNode fw;
	std::vector<uint32_t> ent;
	for (int i = 0; i < 6; ++i) {
		ent.push_back(0x00000000u);
		ent.push_back(0x0FFFFF00u);
		ent.push_back(0x0FFFFF80u);
		ent.push_back(0x0FFFFFFFu);
		ent.push_back(0xFFFFFFFFu);
	}
	for (uint32_t i = 1u; i <= 30u; ++i) {
		ent.push_back(0x01000000u + i * 0x101u);
	}
	fw.set_entropy(ent.data(), static_cast<int>(ent.size()));
	fw.boot(true, 1);

	uint32_t curMuid = discover_fw_muid(fw);
	for (int iter = 0; iter < 20; ++iter) {
		E2E_CHECK(curMuid >= 1u && curMuid <= 0x0FFFFEFFu);
		E2E_CHECK(ms::MuidIsUsable(curMuid));
		uint8_t inv[ms::InvalidateMuidByteCount] = {};
		ms::BuildInvalidateMuid(0x01020304u, curMuid, inv, sizeof(inv));
		send_ci(fw, 0, std::vector<uint8_t>(inv, inv + sizeof(inv)));
		drain_outputs(fw, 0);
		curMuid = discover_fw_muid(fw);
	}
	E2E_CHECK(curMuid >= 1u && curMuid <= 0x0FFFFEFFu);
	E2E_CHECK(ms::MuidIsUsable(curMuid));
}

// ============================================================================
// Tier 2 — F8 Boundaries (T2-F8-01..05)
// ============================================================================

E2E_TEST(2, "T2-F8-01", "VersionFormat 0x00 on inquiries returns NAK 0x02 with VersionFormat 0x02") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	const auto disc0 = make_discovery_bytes(0x01020304u, 512u, 0x00u);
	uint8_t ep0[ms::EndpointInquiryByteCount] = {};
	ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, ep0, sizeof(ep0));
	ep0[ms::OffsetVersionFormat] = 0x00u;
	const auto pe0 = make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                               1u, R"({"resource":"DeviceInfo"})", 1u, 1u, "", 0x00u);

	const struct {
		std::vector<uint8_t> pkt;
		uint8_t origType;
	} cases[] = {
	    {disc0, 0x70u},
	    {std::vector<uint8_t>(ep0, ep0 + sizeof(ep0)), 0x72u},
	    {pe0, 0x34u},
	};
	for (const auto& c : cases) {
		send_ci(fw, 0, c.pkt);
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.VersionFormat, 0x02u);
			E2E_CHECK_EQ(p.Acknowledgment.OriginalMessageType, c.origType);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, 0x02u);
		}
	}

	// Non-inquiry DiscoveryReply with ver=0x00 produces 0 replies
	std::vector<uint8_t> discRep(ms::DiscoveryReplyByteCount, 0u);
	ms::WriteCommonHeader(discRep.data(), discRep.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::DiscoveryReply, 0x01020304u, fwMuid, 0x00u);
	send_ci(fw, 0, discRep);
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());
}

E2E_TEST(2, "T2-F8-02", "Future version 0x03/0x0F replies 0x02; reserved bits 0x22/0x7F return NAK 0x02") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	for (uint8_t ver : {uint8_t{0x03u}, uint8_t{0x0Fu}}) {
		send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, ver));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
			E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		}
	}

	for (uint8_t badVer : {uint8_t{0x22u}, uint8_t{0x7Fu}}) {
		send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, badVer));
		const DrainedOutputs dOut = drain_outputs(fw, 0);
		E2E_CHECK_EQ(dOut.ciMsgs.size(), 1u);
		if (dOut.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(dOut.ciMsgs[0].data(), dOut.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.VersionFormat, 0x02u);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, 0x02u);
		}

		uint8_t ep[ms::EndpointInquiryByteCount] = {};
		ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, ep, sizeof(ep));
		ep[ms::OffsetVersionFormat] = badVer;
		send_ci(fw, 0, std::vector<uint8_t>(ep, ep + sizeof(ep)));
		const DrainedOutputs eOut = drain_outputs(fw, 0);
		E2E_CHECK_EQ(eOut.ciMsgs.size(), 1u);
		if (eOut.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(eOut.ciMsgs[0].data(), eOut.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.VersionFormat, 0x02u);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, 0x02u);
		}
	}
}

E2E_TEST(2, "T2-F8-03", "Truncated v1 vs v2 Discovery boundary (29B vs 30B)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// 29B with ver=0x02 (missing byte 29 OutputPathId) -> NAK 0x41
	auto truncV2 = make_discovery_bytes(0x01020304u, 512u, 0x02u);
	truncV2.pop_back();
	E2E_CHECK_EQ(truncV2.size(), 29u);
	send_ci(fw, 0, truncV2);
	const DrainedOutputs nakOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(nakOut.ciMsgs.size(), 1u);
	if (nakOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(nakOut.ciMsgs[0].data(), nakOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageMalformed);
	}

	// 29B with ver=0x01 -> DiscoveryReply 31B, VersionFormat=0x02, OutputPathId=0x00
	const auto validV1 = make_discovery_bytes(0x01020304u, 512u, 0x01u);
	E2E_CHECK_EQ(validV1.size(), 29u);
	send_ci(fw, 0, validV1);
	const DrainedOutputs repOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(repOut.ciMsgs.size(), 1u);
	if (repOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(repOut.ciMsgs[0].data(), repOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		E2E_CHECK_EQ(p.OutputPathId, 0x00u);
	}
}

E2E_TEST(2, "T2-F8-04", "v1 (14B) vs v2 (16B) PE Capabilities Inquiry both emit v2 (16B) 0x02 reply") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	std::vector<uint8_t> req1(ms::PropertyExchangeCapabilitiesByteCountVersion11, 0u);
	ms::WriteCommonHeader(req1.data(), req1.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
	                      fwMuid, 0x01u);
	req1[13] = 1u;

	std::vector<uint8_t> req2(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	ms::WriteCommonHeader(req2.data(), req2.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
	                      fwMuid, 0x02u);
	req2[13] = 1u;

	for (const auto& r : {req1, req2}) {
		send_ci(fw, 0, r);
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::PropertyExchangeCapabilitiesByteCount);
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK_EQ(p.VersionFormat, 0x02u);
			if (out.ciMsgs[0].size() >= 16u) {
				E2E_CHECK_EQ(out.ciMsgs[0][13], 1u);
				E2E_CHECK_EQ(out.ciMsgs[0][14], 0u);
				E2E_CHECK_EQ(out.ciMsgs[0][15], 0u);
			}
		}
	}
}

E2E_TEST(2, "T2-F8-05", "v1 PE Get (0x34) and v1 PE Set (0x36) emit v2 (0x02) replies") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"DeviceInfo"})", 1u, 1u, "", 0x01u));
	std::string getHdr, getBody;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 1u, fwMuid, 0x01020304u,
	                              320u, getHdr, getBody));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(getHdr, "status", 0), 200);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySetDataInquiry, 0x01020304u, fwMuid,
	                             2u, R"({"resource":"X-PlayOptions"})", 1u, 1u, "{}", 0x01u));
	std::string setHdr, setBody;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertySetDataReply, 2u, fwMuid, 0x01020304u,
	                              320u, setHdr, setBody));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(setHdr, "status", 0), 200);
}

// ============================================================================
// Tier 2 — F9 Boundaries (T2-F9-01..05)
// ============================================================================

E2E_TEST(2, "T2-F9-01", "Empty, missing-resource, and non-object JSON headers return status 400") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const char* badHeaders[] = {"", "{}", "not json", "[]"};
	for (uint8_t i = 0; i < 4u; ++i) {
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 1u), badHeaders[i]));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK(hdr == R"({"status":400})");
		E2E_CHECK(body.empty());
	}
}

E2E_TEST(2, "T2-F9-02", "Non-string resource JSON values (int, null, bool, array) return status 400") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const char* headers[] = {
	    R"({"resource":123})",
	    R"({"resource":null})",
	    R"({"resource":true})",
	    R"({"resource":["DeviceInfo"]})",
	};
	for (uint8_t i = 0; i < 4u; ++i) {
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 1u), headers[i]));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK_EQ(hdr, std::string(R"({"status":400})"));
	}
}

E2E_TEST(2, "T2-F9-03", "Unterminated string and truncated JSON header return status 400 with zero OOB reads") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const char* badHeaders[] = {
	    R"({"resource":"DeviceInf)",
	    R"({"resource":)",
	    R"({"resource":"DeviceInfo"}x)",
	};
	for (uint8_t i = 0; i < 3u; ++i) {
		struct ci_json_it it {};
		struct ci_json_member m {};
		ci_json_it_init(&it, badHeaders[i], static_cast<uint16_t>(std::strlen(badHeaders[i])));
		int rc = 1;
		while ((rc = ci_json_next(&it, &m)) == 1) {
		}
		E2E_CHECK_EQ(rc, -1);

		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 1u), badHeaders[i]));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK(hdr == R"({"status":400})");
	}
}

E2E_TEST(2, "T2-F9-04", "Trailing backslash and control char inside JSON string return status 400") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const std::string h1 = std::string("{\"resource\":\"a\\");
	const std::string h2 = std::string("{\"resource\":\"a\x01") + "b\"}";
	for (uint8_t i = 0; i < 2u; ++i) {
		const std::string& raw = (i == 0u) ? h1 : h2;
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 1u), raw));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK(hdr == R"({"status":400})");
	}
}

E2E_TEST(2, "T2-F9-05", "Overlong 128-char resource name returns 404; depth > 8 returns 400") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const std::string longHdr = std::string(R"({"resource":")") + std::string(128u, 'A') + "\"}";
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, longHdr));
	std::string hdr1, body1;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 1u, fwMuid, 0x01020304u,
	                              320u, hdr1, body1));
	E2E_CHECK(hdr1 == R"({"status":404})");

	send_ci(fw, 0,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 2u,
	                      R"({"x":[[[[[[[[[1]]]]]]]]],"resource":"DeviceInfo"})"));
	std::string hdr2, body2;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 2u, fwMuid, 0x01020304u,
	                              320u, hdr2, body2));
	E2E_CHECK(hdr2 == R"({"status":400})");
}

// ============================================================================
// Tier 2 — F10 Boundaries (T2-F10-01..05)
// ============================================================================

E2E_TEST(2, "T2-F10-01", "Empty resource \"\" -> 404; ChannelList / X-ScaleList / X-TuningList -> 200") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":""})"));
	std::string hdr0, body0;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 1u, fwMuid, 0x01020304u,
	                              320u, hdr0, body0));
	E2E_CHECK(hdr0 == R"({"status":404})");
	E2E_CHECK(body0.empty());

	const struct {
		const char* name;
		uint8_t res;
	} resources[] = {
	    {"ChannelList", CI_PE_CHANNEL_LIST},
	    {"X-ScaleList", CI_PE_SCALE_LIST},
	    {"X-TuningList", CI_PE_TUNING_LIST},
	};
	for (uint8_t i = 0; i < 3u; ++i) {
		const std::string q = std::string(R"({"resource":")") + resources[i].name + "\"}";
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 2u), q));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 2u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK(hdr == R"({"status":200})");
		E2E_CHECK(body == render_expected_pe(resources[i].res, ui_settings(), "TEST0001"));
	}
}

E2E_TEST(2, "T2-F10-02", "Case-sensitive resource lookup -> 404") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const char* names[] = {"deviceinfo", "RESOURCELIST", "DeviceInfo "};
	for (uint8_t i = 0; i < 3u; ++i) {
		const std::string q = std::string(R"({"resource":")") + names[i] + "\"}";
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                             static_cast<uint8_t>(i + 1u), q));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 320u,
		                              hdr, body));
		E2E_CHECK_EQ(hdr, std::string(R"({"status":404})"));
	}
}

E2E_TEST(2, "T2-F10-03", "PropertySetDataInquiry (0x36) full status matrix (200/400/404/405/413)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	auto doSet = [&](uint8_t reqId, const std::string& hdrJson, const std::string& body) {
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySetDataInquiry, 0x01020304u, fwMuid,
		                             reqId, hdrJson, 1u, 1u, body));
		std::string rHdr, rBody;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertySetDataReply, reqId, fwMuid,
		                              0x01020304u, 320u, rHdr, rBody));
		return m2s::ci::ciJsonInt(rHdr, "status", 0);
	};

	E2E_CHECK_EQ(doSet(1u, R"({"resource":"DeviceInfo"})", "{}"), 405);
	E2E_CHECK_EQ(doSet(2u, R"({"resource":"X-Nope"})", "{}"), 404);
	E2E_CHECK_EQ(doSet(3u, R"({"resource":"X-ScaleSettings","setPartial":true})", R"({"root":1})"),
	             400);

	// > 256B Set body across 2 chunks -> 413
	const std::string bigPart(140u, ' ');
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySetDataInquiry, 0x01020304u, fwMuid, 4u,
	                             R"({"resource":"X-ScaleSettings"})", 2u, 1u,
	                             "{" + bigPart));
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySetDataInquiry, 0x01020304u, fwMuid, 4u,
	                             "", 2u, 2u, bigPart + R"("root":0})"));
	std::string rHdr413, rBody413;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertySetDataReply, 4u, fwMuid, 0x01020304u,
	                              320u, rHdr413, rBody413));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(rHdr413, "status", 0), 413);

	// Valid Set X-PlayOptions {"channel":7} -> 200 and updates settings + ChannelList
	E2E_CHECK_EQ(doSet(5u, R"({"resource":"X-PlayOptions"})", R"({"channel":7})"), 200);
	E2E_CHECK_EQ(ui_settings()->channel, 6u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             6u, R"({"resource":"ChannelList"})"));
	std::string chHdr, chBody;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 6u, fwMuid, 0x01020304u,
	                              320u, chHdr, chBody));
	E2E_CHECK(chBody.find(R"("channel":7)") != std::string::npos);
}

E2E_TEST(2, "T2-F10-04", "PropertySubscriptionInquiry (0x38) full command matrix (200/400/404/405)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	auto doSub = [&](uint8_t reqId, const std::string& hdrJson) {
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, 0x01020304u,
		                             fwMuid, reqId, hdrJson));
		std::string rHdr, rBody;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertySubscriptionReply, reqId, fwMuid,
		                              0x01020304u, 320u, rHdr, rBody));
		return rHdr;
	};

	E2E_CHECK_EQ(m2s::ci::ciJsonInt(doSub(1u, R"({"command":"start","resource":"DeviceInfo"})"),
	                                "status", 0),
	             405);
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(doSub(2u, R"({"command":"start","resource":"X-ScaleList"})"),
	                                "status", 0),
	             405);
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(doSub(3u, R"({"command":"start","resource":"X-Nope"})"),
	                                "status", 0),
	             404);
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(doSub(4u, R"({"command":"full","resource":"X-PlayOptions"})"),
	                                "status", 0),
	             400);

	const std::string startHdr = doSub(5u, R"({"command":"start","resource":"X-PlayOptions"})");
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(startHdr, "status", 0), 200);
	const std::string subId = m2s::ci::ciJsonString(startHdr, "subscribeId");
	E2E_CHECK(subId.rfind("lp", 0) == 0);

	const std::string endHdr = doSub(
	    6u, std::string(R"({"command":"end","subscribeId":")") + subId + "\"}");
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(endHdr, "status", 0), 200);

	// Changing settings after subscription end emits 0 notify packets
	struct settings next = *ui_settings();
	next.channel = static_cast<uint8_t>((next.channel + 1u) % 16u);
	ui_apply_settings(&next, fw.clock().now_ms());
	E2E_CHECK(drain_outputs(fw, 0).ciMsgs.empty());
}

E2E_TEST(2, "T2-F10-05", "Malformed PE length fields (0x3FFF header/data, truncated 14B 0x34) safely dropped") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	// 0x3FFF HeaderByteCount
	auto badHdrLen = make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 1u,
	                               R"({"resource":"DeviceInfo"})");
	ms::WriteFourteenBitValue(&badHdrLen[14], 0x3FFFu);
	send_ci(fw, 0, badHdrLen);

	// 0x3FFF DataByteCount
	auto badDataLen = make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                                2u, R"({"resource":"DeviceInfo"})");
	ms::WriteFourteenBitValue(&badDataLen[badDataLen.size() - 2u], 0x3FFFu);
	send_ci(fw, 0, badDataLen);

	// Truncated 14B 0x34
	std::vector<uint8_t> trunc(14u, 0u);
	ms::WriteCommonHeader(trunc.data(), trunc.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid);
	trunc[13] = 3u;
	send_ci(fw, 0, trunc);

	// Malformed 0x34 lengths are NAKed 0x41 (never crash / OOB)
	const DrainedOutputs errOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(errOut.ciMsgs.size(), 3u);
	for (const auto& m : errOut.ciMsgs) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(m.data(), m.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageMalformed);
	}

	// Responder recovers cleanly on immediate valid GET DeviceInfo
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             4u, R"({"resource":"DeviceInfo"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 4u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
}

// ============================================================================
// Tier 2 — F11 Boundaries (T2-F11-01..05)
// ============================================================================

E2E_TEST(2, "T2-F11-01", "Exact MIDI-CI PE minimum maxSysEx=128 on ResourceList & X-ScaleList") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	for (uint8_t i = 0; i < 2u; ++i) {
		const uint8_t res = (i == 0u) ? static_cast<uint8_t>(CI_PE_RESOURCE_LIST)
		                              : static_cast<uint8_t>(CI_PE_SCALE_LIST);
		const char* name = (i == 0u) ? "ResourceList" : "X-ScaleList";
		send_ci(fw, 0,
		        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
		                      static_cast<uint8_t>(i + 1u),
		                      std::string(R"({"resource":")") + name + "\"}"));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, 0x01020304u, 128u,
		                              hdr, body));
		E2E_CHECK(body == render_expected_pe(res, ui_settings(), "TEST0001"));
	}
}

E2E_TEST(2, "T2-F11-02", "Undersized maxSysEx (0, 64, 127) falls back safely to default 512 (clamped 320)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t sizes[] = {0u, 64u, 127u};
	for (uint8_t i = 0; i < 3u; ++i) {
		const uint32_t peer = 0x0330001u + i;
		const uint32_t fwMuid = discover_fw_muid(fw, peer, sizes[i]);
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peer, fwMuid,
		                             static_cast<uint8_t>(i + 1u),
		                             R"({"resource":"DeviceInfo"})"));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(out.ciMsgs, ms::MessageType::PropertyGetDataReply,
		                              static_cast<uint8_t>(i + 1u), fwMuid, peer, 320u, hdr,
		                              body));
		E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	}
}

E2E_TEST(2, "T2-F11-03", "Large maxSysEx (4096 and 0x0FFFFFFF) clamps to CI_PE_MESSAGE_MAX=320") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 0x0FFFFFFFu);
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"X-TuningList"})"));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK(out.ciMsgs.size() > 1u);
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(out.ciMsgs, ms::MessageType::PropertyGetDataReply, 1u, fwMuid,
	                              0x01020304u, 320u, hdr, body));
	E2E_CHECK(body == render_expected_pe(CI_PE_TUNING_LIST, ui_settings(), "TEST0001"));
}

E2E_TEST(2, "T2-F11-04", "New GET arriving mid-chunked-GET replaces in-flight reply cleanly (simultaneous=1)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"ResourceList"})"));
	__wrap_midi_parsers_poll();
	const DrainedOutputs first = drain_tx_only_no_poll(fw, 0);
	E2E_CHECK_EQ(first.ciMsgs.size(), 1u);
	if (first.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(first.ciMsgs[0].data(), first.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK_EQ(p.PropertyExchange.RequestId, 1u);
		E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, 1u);
	}

	// Send req=2 mid-stream
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             2u, R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs rest = drain_outputs(fw, 0);
	E2E_CHECK_EQ(rest.ciMsgs.size(), 3u);
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(rest.ciMsgs, ms::MessageType::PropertyGetDataReply, 2u, fwMuid,
	                              0x01020304u, 128u, hdr, body));
	E2E_CHECK(body == render_expected_pe(CI_PE_DEVICE_INFO, ui_settings(), "TEST0001"));
}

E2E_TEST(2, "T2-F11-05", "CiTxQueue backpressure mid-PE-chunking stalls without loss or overflow") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"DeviceInfo"})"));
	__wrap_midi_parsers_poll();
	const DrainedOutputs c1 = drain_tx_only_no_poll(fw, 0);
	E2E_CHECK_EQ(c1.ciMsgs.size(), 1u);
	E2E_CHECK_EQ(midici_tx_free_packets(), static_cast<uint16_t>(CI_TX_QUEUE_PACKETS));

	// Fill ci_tx on group 1 to leave 18 free packets (< 21 required for chunk 2)
	const std::vector<uint8_t> filler60(60u, 0x55u);  // 10 packets each
	for (int i = 0; i < 11; ++i) {
		E2E_CHECK_EQ(midici_send_sysex(1, filler60.data(), static_cast<uint16_t>(filler60.size())), 0);
	}
	E2E_CHECK_EQ(midici_tx_free_packets(), 18u);
	const uint16_t pendingBefore = midici_tx_pending();

	// Polling without draining ci_tx must stall chunk 2 cleanly with 0 overflows
	__wrap_midi_parsers_poll();
	__wrap_midi_parsers_poll();
	E2E_CHECK_EQ(midici_tx_overflows(), 0u);
	E2E_CHECK_EQ(midici_tx_pending(), pendingBefore);

	// Drain group 1 filler, then poll group 0 to completion
	const DrainedOutputs fillOut = drain_tx_only_no_poll(fw, 0);
	E2E_CHECK(fillOut.ciMsgs.empty());
	E2E_CHECK_EQ(fillOut.otherGroupCiMsgs.size(), 11u);

	const DrainedOutputs rest = drain_outputs(fw, 0);
	E2E_CHECK_EQ(rest.ciMsgs.size(), 2u);
	std::vector<std::vector<uint8_t>> allChunks;
	if (c1.ciMsgs.size() == 1u) allChunks.push_back(c1.ciMsgs[0]);
	for (const auto& m : rest.ciMsgs) allChunks.push_back(m);

	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(allChunks, ms::MessageType::PropertyGetDataReply, 1u, fwMuid,
	                              0x01020304u, 128u, hdr, body));
	E2E_CHECK(body == render_expected_pe(CI_PE_DEVICE_INFO, ui_settings(), "TEST0001"));
}

#endif  // E2E_HAS_M2
