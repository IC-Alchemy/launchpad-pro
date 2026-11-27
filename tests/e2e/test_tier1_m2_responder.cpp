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
	std::vector<m2s::UmpPacket> rawMt3Pkts;
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
}

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
	buf[14] = 0x00u;
	buf[15] = 0x00u;
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
// Tier 1 — F5: Responder Config & Identity (T1-F5-01..05)
// ============================================================================

E2E_TEST(1, "T1-F5-01", "Discovery Reply identity fields match Launchpad identity") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		const auto& msg = out.ciMsgs[0];
		E2E_CHECK_EQ(msg.size(), ms::DiscoveryReplyByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(msg.data(), msg.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(p.DestinationMuid, 0x01020304u);
		if (msg.size() >= 24u) {
			E2E_CHECK_EQ(msg[13], static_cast<uint8_t>(CI_MANUFACTURER_0));
			E2E_CHECK_EQ(msg[14], static_cast<uint8_t>(CI_MANUFACTURER_1));
			E2E_CHECK_EQ(msg[15], static_cast<uint8_t>(CI_MANUFACTURER_2));
			E2E_CHECK_EQ(ms::ReadFourteenBitValue(&msg[16]), static_cast<uint16_t>(CI_FAMILY));
			E2E_CHECK_EQ(ms::ReadFourteenBitValue(&msg[18]), static_cast<uint16_t>(CI_MODEL));
			E2E_CHECK_EQ(msg[20], static_cast<uint8_t>(CI_SW_REV_0));
			E2E_CHECK_EQ(msg[21], static_cast<uint8_t>(CI_SW_REV_1));
			E2E_CHECK_EQ(msg[22], static_cast<uint8_t>(CI_SW_REV_2));
			E2E_CHECK_EQ(msg[23], static_cast<uint8_t>(CI_SW_REV_3));
		}
	}
}

E2E_TEST(1, "T1-F5-02", "Discovery Reply declares PE capability 0x08, maxSysEx 512, and FB 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u && out.ciMsgs[0].size() >= 31u) {
		const auto& msg = out.ciMsgs[0];
		E2E_CHECK_EQ(msg[24], 0x08u);
		E2E_CHECK_EQ(ms::ReadMuid(&msg[25]), 512u);
		E2E_CHECK_EQ(msg[30], 0x00u);
	}
}

E2E_TEST(1, "T1-F5-03", "Endpoint Inquiry status 0x00 returns Product Instance ID") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);
	E2E_CHECK(fwMuid != 0u);

	uint8_t req[ms::EndpointInquiryByteCount] = {};
	const size_t n = ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, req, sizeof(req));
	send_ci(fw, 0, std::vector<uint8_t>(req, req + n));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		const auto& reply = out.ciMsgs[0];
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(reply.data(), reply.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::EndpointReply);
		E2E_CHECK(p.HasEndpointFields);
		E2E_CHECK_EQ(p.Endpoint.Status, 0x00u);
		const std::string gotPiid(
		    reinterpret_cast<const char*>(reply.data() + p.Endpoint.InformationOffset),
		    p.Endpoint.InformationByteCount);
		char expectedPiid[25] = {};
		platform_product_instance_id(expectedPiid);
		E2E_CHECK(gotPiid == expectedPiid);
	}
}

E2E_TEST(1, "T1-F5-04", "MIDI-CI EndpointReply ProductInstanceId matches UMP ST_PRODUCT_ID byte-for-byte") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const auto epDisc = e2e::make_ep_discovery(0x0Au);  // ST_DEV_IDENTITY (0x02) + ST_PRODUCT_ID (0x08)
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	const DrainedOutputs streamOut = drain_outputs(fw, 0);

	std::string umpPiid;
	uint32_t idW1 = 0, idW2 = 0, idW3 = 0;
	for (const auto& s : streamOut.streamMsgs) {
		const uint16_t st = static_cast<uint16_t>((s[0] >> 16) & 0x3FFu);
		if (st == 0x002u) {
			idW1 = s[1];
			idW2 = s[2];
			idW3 = s[3];
		} else if (st == 0x004u) {
			const uint8_t c0 = static_cast<uint8_t>((s[0] >> 8) & 0xFFu);
			const uint8_t c1 = static_cast<uint8_t>(s[0] & 0xFFu);
			if (c0) umpPiid.push_back(static_cast<char>(c0));
			if (c1) umpPiid.push_back(static_cast<char>(c1));
			for (size_t w = 1; w < 4; ++w) {
				for (int sh = 24; sh >= 0; sh -= 8) {
					const char ch = static_cast<char>((s[w] >> sh) & 0xFFu);
					if (ch) umpPiid.push_back(ch);
				}
			}
		}
	}
	E2E_CHECK(!umpPiid.empty());
	E2E_CHECK_EQ(idW1, CI_UMP_IDENTITY_WORD1);
	E2E_CHECK_EQ(idW2, CI_UMP_IDENTITY_WORD2);
	E2E_CHECK_EQ(idW3, CI_UMP_IDENTITY_WORD3);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u));
	const DrainedOutputs discOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(discOut.ciMsgs.size(), 1u);
	uint32_t fwMuid = 0;
	if (discOut.ciMsgs.size() == 1u && discOut.ciMsgs[0].size() >= 24u) {
		const auto& d = discOut.ciMsgs[0];
		ms::ParsedMessage pd{};
		E2E_CHECK(ms::Parse(d.data(), d.size(), pd) == ms::ParseStatus::Ok);
		fwMuid = pd.SourceMuid;
		const uint32_t mfgWord = (static_cast<uint32_t>(d[13]) << 16) |
		                         (static_cast<uint32_t>(d[14]) << 8) | d[15];
		const uint32_t famModWord = (static_cast<uint32_t>(d[16]) << 24) |
		                            (static_cast<uint32_t>(d[17]) << 16) |
		                            (static_cast<uint32_t>(d[18]) << 8) | d[19];
		const uint32_t revWord = (static_cast<uint32_t>(d[20]) << 24) |
		                         (static_cast<uint32_t>(d[21]) << 16) |
		                         (static_cast<uint32_t>(d[22]) << 8) | d[23];
		E2E_CHECK_EQ(mfgWord, idW1);
		E2E_CHECK_EQ(famModWord, idW2);
		E2E_CHECK_EQ(revWord, idW3);
	}

	uint8_t epReq[ms::EndpointInquiryByteCount] = {};
	const size_t n = ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, epReq, sizeof(epReq));
	send_ci(fw, 0, std::vector<uint8_t>(epReq, epReq + n));
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	if (epOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage pe{};
		E2E_CHECK(ms::Parse(epOut.ciMsgs[0].data(), epOut.ciMsgs[0].size(), pe) ==
		          ms::ParseStatus::Ok);
		const std::string ciPiid(
		    reinterpret_cast<const char*>(epOut.ciMsgs[0].data() + pe.Endpoint.InformationOffset),
		    pe.Endpoint.InformationByteCount);
		E2E_CHECK(ciPiid == umpPiid);
	}
}

E2E_TEST(1, "T1-F5-05", "PE Capabilities Inquiry returns simultaneousRequests=1 and PE v0.0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	std::vector<uint8_t> req(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	ms::WriteCommonHeader(req.data(), req.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
	                      fwMuid, 0x02u);
	req[13] = 0x01u;
	req[14] = 0x00u;
	req[15] = 0x00u;
	send_ci(fw, 0, req);

	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		const auto& r = out.ciMsgs[0];
		E2E_CHECK_EQ(r.size(), ms::PropertyExchangeCapabilitiesByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(r.data(), r.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::PropertyExchangeCapabilitiesReply);
		if (r.size() >= 16u) {
			E2E_CHECK_EQ(r[13], 1u);
			E2E_CHECK_EQ(r[14], 0u);
			E2E_CHECK_EQ(r[15], 0u);
		}
	}
}

// ============================================================================
// Tier 1 — F6: FB MIDI-CI Version & Same-Group Reply (T1-F6-01..05)
// ============================================================================

E2E_TEST(1, "T1-F6-01", "UMP FB 0 Discovery announces MIDI-CI version 0x02") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const auto fbDisc = e2e::make_fb_discovery(0x00u, 0x01u);
	fw.send_ump(fbDisc.data(), static_cast<uint16_t>(fbDisc.size()));
	const DrainedOutputs out = drain_outputs(fw, 0);

	bool sawFb0 = false;
	for (const auto& s : out.streamMsgs) {
		if (s[0] == 0xF0118033u) {
			sawFb0 = true;
			E2E_CHECK_EQ(s[1], 0x00010200u);
			E2E_CHECK_EQ((s[1] >> 8) & 0xFFu, 0x02u);
		}
	}
	E2E_CHECK(sawFb0);
}

E2E_TEST(1, "T1-F6-02", "Discovery Reply routed on incoming UMP group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK(out.rawMt3Pkts.size() >= 6u);
	for (const auto& pkt : out.rawMt3Pkts) {
		E2E_CHECK_EQ(m2s::umpGroup(pkt), 0u);
	}
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
	}
}

E2E_TEST(1, "T1-F6-03", "MIDI-CI traffic on group 1 (MIDI Port) emits zero MIDI-CI reply and preserves group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t beforeRx = midici_messages_received();
	send_ci(fw, 1, make_discovery_bytes(0x01020304u, 512u));
	const DrainedOutputs g1Out = drain_outputs(fw, 1);
	E2E_CHECK_EQ(midici_messages_received() - beforeRx, 1u);
	E2E_CHECK(g1Out.ciMsgs.empty());
	E2E_CHECK(g1Out.otherGroupCiMsgs.empty());
	E2E_CHECK(g1Out.rawMt3Pkts.empty());

	// Group 0 responder remains fully operational
	const uint32_t fwMuid = discover_fw_muid(fw);
	E2E_CHECK(fwMuid != 0u);
}

E2E_TEST(1, "T1-F6-04", "Endpoint and chunked PE replies emit all packets on group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	uint8_t epReq[ms::EndpointInquiryByteCount] = {};
	const size_t epN = ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, epReq, sizeof(epReq));
	send_ci(fw, 0, std::vector<uint8_t>(epReq, epReq + epN));
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	E2E_CHECK(epOut.otherGroupCiMsgs.empty());
	for (const auto& pkt : epOut.rawMt3Pkts) {
		E2E_CHECK_EQ(m2s::umpGroup(pkt), 0u);
	}

	const auto getRl = make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                                 3u, R"({"resource":"ResourceList"})");
	send_ci(fw, 0, getRl);
	const DrainedOutputs peOut = drain_outputs(fw, 0);
	E2E_CHECK(peOut.ciMsgs.size() > 1u);
	E2E_CHECK(peOut.otherGroupCiMsgs.empty());
	for (const auto& pkt : peOut.rawMt3Pkts) {
		E2E_CHECK_EQ(m2s::umpGroup(pkt), 0u);
	}
}

E2E_TEST(1, "T1-F6-05", "OutputPathId from v2 Discovery is echoed in DiscoveryReply") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x42u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
		if (out.ciMsgs[0].size() >= 30u) {
			E2E_CHECK_EQ(out.ciMsgs[0][29], 0x42u);
		}
	}
}

// ============================================================================
// Tier 1 — F7: MUID Lifecycle, Auto-NAK & Self-Echo (T1-F7-01..05)
// ============================================================================

E2E_TEST(1, "T1-F7-01", "Initial responder MUID is valid 28-bit non-reserved value") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	E2E_CHECK(fwMuid != 0u);
	E2E_CHECK(fwMuid <= 0x0FFFFFFFu);
	E2E_CHECK(ms::MuidIsUsable(fwMuid));
	E2E_CHECK_EQ(midici_muid_valid(fwMuid), 1);
}

E2E_TEST(1, "T1-F7-02", "InvalidateMuid targeting Launchpad regenerates MUID and emits FB0 alert") {
	e2e::FirmwareNode fw;
	const uint32_t ent[] = {0x01112233u, 0x04445566u, 0x07778899u};
	fw.set_entropy(ent, 3);
	fw.boot(true, 1);

	const uint32_t oldMuid = discover_fw_muid(fw);
	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t n = ms::BuildInvalidateMuid(0x01020304u, oldMuid, inv, sizeof(inv));
	send_ci(fw, 0, std::vector<uint8_t>(inv, inv + n));

	const DrainedOutputs invOut = drain_outputs(fw, 0);
	E2E_CHECK(invOut.ciMsgs.empty());
	bool sawFb0Alert = false;
	for (const auto& s : invOut.streamMsgs) {
		if (s[0] == 0xF0118033u && s[1] == 0x00010200u) sawFb0Alert = true;
	}
	E2E_CHECK(sawFb0Alert);

	const uint32_t newMuid = discover_fw_muid(fw);
	E2E_CHECK(newMuid != oldMuid);
	E2E_CHECK(ms::MuidIsUsable(newMuid));
}

E2E_TEST(1, "T1-F7-03", "InitiatorMuidInvalidated purges cached peer maxSysEx and subscriptions") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t peerA = 0x0111111u;
	const uint32_t peerB = 0x0222222u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerA, 128u);

	// Verify peerA gets 3-chunk DeviceInfo (<= 128B)
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 1u,
	                             R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs chunked = drain_outputs(fw, 0);
	E2E_CHECK_EQ(chunked.ciMsgs.size(), 3u);

	// Subscribe peerA to X-PlayOptions
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, peerA, fwMuid, 2u,
	                             R"({"command":"start","resource":"X-PlayOptions"})"));
	const DrainedOutputs subOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(subOut.ciMsgs.size(), 1u);

	// Peer B withdraws peerA's MUID
	uint8_t inv[ms::InvalidateMuidByteCount] = {};
	const size_t invN = ms::BuildInvalidateMuid(peerB, peerA, inv, sizeof(inv));
	send_ci(fw, 0, std::vector<uint8_t>(inv, inv + invN));
	drain_outputs(fw, 0);

	// Settings change must produce 0 notify messages for invalidated peerA
	struct settings next = *ui_settings();
	next.channel = static_cast<uint8_t>((next.channel + 1u) % 16u);
	ui_apply_settings(&next, fw.clock().now_ms());
	const DrainedOutputs afterSettings = drain_outputs(fw, 0);
	E2E_CHECK(afterSettings.ciMsgs.empty());

	// Peer A's GET DeviceInfo now defaults to 512 (clamped 320) -> 1 chunk (> 128 bytes)
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 3u,
	                             R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs unchunked = drain_outputs(fw, 0);
	E2E_CHECK_EQ(unchunked.ciMsgs.size(), 1u);
	if (unchunked.ciMsgs.size() == 1u) {
		E2E_CHECK(unchunked.ciMsgs[0].size() + 2u > 128u);
	}
}

E2E_TEST(1, "T1-F7-04", "Unsupported inquiries receive automatic NAK 0x01") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	// ProfileInquiry (0x20)
	uint8_t profBuf[32] = {};
	ms::ProfileMessageFields pf{};
	pf.Type = ms::MessageType::ProfileInquiry;
	pf.SourceMuid = 0x01020304u;
	pf.DestinationMuid = fwMuid;
	const size_t profN = ms::BuildProfileMessage(pf, profBuf, sizeof(profBuf));

	// SetProfileOn (0x22)
	uint8_t setProfBuf[32] = {};
	pf.Type = ms::MessageType::SetProfileOn;
	pf.ProfileId[0] = 0x7Eu;
	const size_t setProfN = ms::BuildProfileMessage(pf, setProfBuf, sizeof(setProfBuf));

	// ProcessInquiryCapabilities (0x40)
	uint8_t procBuf[ms::ProcessInquiryCapabilitiesByteCount] = {};
	const size_t procN = ms::BuildProcessInquiryCapabilities(0x01020304u, fwMuid, procBuf, sizeof(procBuf));

	// MidiMessageReport (0x42)
	uint8_t mmrBuf[ms::MidiMessageReportByteCount] = {};
	ms::MidiMessageReportFields mf{};
	mf.MessageDataControl = ms::MessageDataControlNone;
	const size_t mmrN = ms::BuildMidiMessageReport(ms::DeviceIdFunctionBlock, 0x01020304u,
	                                               fwMuid, mf, mmrBuf, sizeof(mmrBuf));

	const struct {
		const uint8_t* data;
		size_t len;
		uint8_t origType;
	} cases[] = {
	    {profBuf, profN, 0x20u},
	    {setProfBuf, setProfN, 0x22u},
	    {procBuf, procN, 0x40u},
	    {mmrBuf, mmrN, 0x42u},
	};

	for (const auto& c : cases) {
		send_ci(fw, 0, std::vector<uint8_t>(c.data, c.data + c.len));
		const DrainedOutputs out = drain_outputs(fw, 0);
		E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
		if (out.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK(p.Type == ms::MessageType::Nak);
			E2E_CHECK_EQ(p.VersionFormat, MIDI_CI_VERSION);
			E2E_CHECK(p.HasAcknowledgmentFields);
			E2E_CHECK_EQ(p.Acknowledgment.OriginalMessageType, c.origType);
			E2E_CHECK_EQ(p.Acknowledgment.StatusCode, ms::NakStatusMessageNotSupported);
		}
	}
}

E2E_TEST(1, "T1-F7-05", "Non-Discovery self-echoes are ignored with zero replies, NAKs, or state loss") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t peerA = 0x01020304u;
	const uint32_t fwMuid = discover_fw_muid(fw, peerA, 512u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, peerA, fwMuid, 1u,
	                             R"({"command":"start","resource":"X-PlayOptions"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 1u);

	const uint8_t initialCh = ui_settings()->channel;
	const uint8_t nextCh = static_cast<uint8_t>((initialCh + 1u) % 16u + 1u);

	// EndpointInquiry self-echo
	uint8_t ep[ms::EndpointInquiryByteCount] = {};
	const size_t epN = ms::BuildEndpointInquiry(fwMuid, fwMuid, 0x00u, ep, sizeof(ep));
	send_ci(fw, 0, std::vector<uint8_t>(ep, ep + epN));

	// ProfileInquiry self-echo
	uint8_t prof[32] = {};
	ms::ProfileMessageFields pf{};
	pf.Type = ms::MessageType::ProfileInquiry;
	pf.SourceMuid = fwMuid;
	pf.DestinationMuid = fwMuid;
	const size_t profN = ms::BuildProfileMessage(pf, prof, sizeof(prof));
	send_ci(fw, 0, std::vector<uint8_t>(prof, prof + profN));

	// PE Caps self-echo
	std::vector<uint8_t> caps(ms::PropertyExchangeCapabilitiesByteCount, 0u);
	ms::WriteCommonHeader(caps.data(), caps.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, fwMuid, fwMuid);
	send_ci(fw, 0, caps);

	// PE Get / Set / Subscribe self-echoes
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, fwMuid, fwMuid, 2u,
	                             R"({"resource":"DeviceInfo"})"));
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySetDataInquiry, fwMuid, fwMuid, 3u,
	                             R"({"resource":"X-PlayOptions"})", 1u, 1u,
	                             "{\"channel\":" + std::to_string(nextCh) + "}"));
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, fwMuid, fwMuid, 4u,
	                             R"({"command":"end","resource":"X-PlayOptions"})"));

	// InvalidateMuid self-echo targeting fwMuid and peerA
	uint8_t inv1[ms::InvalidateMuidByteCount] = {};
	ms::BuildInvalidateMuid(fwMuid, fwMuid, inv1, sizeof(inv1));
	send_ci(fw, 0, std::vector<uint8_t>(inv1, inv1 + sizeof(inv1)));
	uint8_t inv2[ms::InvalidateMuidByteCount] = {};
	ms::BuildInvalidateMuid(fwMuid, peerA, inv2, sizeof(inv2));
	send_ci(fw, 0, std::vector<uint8_t>(inv2, inv2 + sizeof(inv2)));

	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK(out.ciMsgs.empty());
	E2E_CHECK(out.streamMsgs.empty());
	E2E_CHECK_EQ(ui_settings()->channel, initialCh);
	E2E_CHECK_EQ(discover_fw_muid(fw, peerA, 512u), fwMuid);
}

// ============================================================================
// Tier 1 — F8: ReplyVersionFor Choice 0x02 (T1-F8-01..05)
// ============================================================================

E2E_TEST(1, "T1-F8-01", "v1 (0x01) Discovery receives v2 (0x02) DiscoveryReply (31B)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const auto v1Disc = make_discovery_bytes(0x01020304u, 512u, 0x01u);
	E2E_CHECK_EQ(v1Disc.size(), ms::DiscoveryByteCountVersion11);
	send_ci(fw, 0, v1Disc);
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		if (out.ciMsgs[0].size() >= 31u) {
			E2E_CHECK_EQ(out.ciMsgs[0][29], 0x00u);
			E2E_CHECK_EQ(out.ciMsgs[0][30], 0x00u);
		}
	}
}

E2E_TEST(1, "T1-F8-02", "v2 (0x02) Discovery receives v2 (0x02) DiscoveryReply (31B)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	send_ci(fw, 0, make_discovery_bytes(0x01020304u, 512u, 0x02u, 0x15u));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		E2E_CHECK_EQ(out.ciMsgs[0].size(), ms::DiscoveryReplyByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::DiscoveryReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		if (out.ciMsgs[0].size() >= 30u) {
			E2E_CHECK_EQ(out.ciMsgs[0][29], 0x15u);
		}
	}
}

E2E_TEST(1, "T1-F8-03", "v1 EndpointInquiry receives v2 (0x02) EndpointReply") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	uint8_t epReq[ms::EndpointInquiryByteCount] = {};
	ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, epReq, sizeof(epReq));
	epReq[ms::OffsetVersionFormat] = 0x01u;
	send_ci(fw, 0, std::vector<uint8_t>(epReq, epReq + sizeof(epReq)));

	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(out.ciMsgs[0].data(), out.ciMsgs[0].size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::EndpointReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		E2E_CHECK_EQ(p.Endpoint.Status, 0x00u);
		const std::string got(
		    reinterpret_cast<const char*>(out.ciMsgs[0].data() + p.Endpoint.InformationOffset),
		    p.Endpoint.InformationByteCount);
		E2E_CHECK(got == "TEST0001");
	}
}

E2E_TEST(1, "T1-F8-04", "v1 PE Capabilities Inquiry (14B) receives v2 (0x02) 16B reply") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	std::vector<uint8_t> req(ms::PropertyExchangeCapabilitiesByteCountVersion11, 0u);
	ms::WriteCommonHeader(req.data(), req.size(), ms::DeviceIdFunctionBlock,
	                      ms::MessageType::PropertyExchangeCapabilitiesInquiry, 0x01020304u,
	                      fwMuid, 0x01u);
	req[13] = 0x01u;
	send_ci(fw, 0, req);

	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	if (out.ciMsgs.size() == 1u) {
		const auto& r = out.ciMsgs[0];
		E2E_CHECK_EQ(r.size(), ms::PropertyExchangeCapabilitiesByteCount);
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(r.data(), r.size(), p) == ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::PropertyExchangeCapabilitiesReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
		if (r.size() >= 16u) {
			E2E_CHECK_EQ(r[13], 1u);
			E2E_CHECK_EQ(r[14], 0u);
			E2E_CHECK_EQ(r[15], 0u);
		}
	}
}

E2E_TEST(1, "T1-F8-05", "PE Get and NAK replies always carry VersionFormat 0x02") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw);

	// v1 PE Get DeviceInfo
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             5u, R"({"resource":"DeviceInfo"})", 1u, 1u, "", 0x01u));
	const DrainedOutputs peOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(peOut.ciMsgs.size(), 1u);
	if (peOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(peOut.ciMsgs[0].data(), peOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::PropertyGetDataReply);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
	}

	// v1 ProfileInquiry -> NAK with VersionFormat 0x02
	uint8_t prof[32] = {};
	ms::ProfileMessageFields pf{};
	pf.Type = ms::MessageType::ProfileInquiry;
	pf.SourceMuid = 0x01020304u;
	pf.DestinationMuid = fwMuid;
	pf.MessageVersion = 0x01u;
	const size_t n = ms::BuildProfileMessage(pf, prof, sizeof(prof));
	send_ci(fw, 0, std::vector<uint8_t>(prof, prof + n));
	const DrainedOutputs nakOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(nakOut.ciMsgs.size(), 1u);
	if (nakOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(nakOut.ciMsgs[0].data(), nakOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK(p.Type == ms::MessageType::Nak);
		E2E_CHECK_EQ(p.VersionFormat, 0x02u);
	}
}

// ============================================================================
// Tier 1 — F9: ciJsonString / ci_json Heap-Free Port (T1-F9-01..05)
// ============================================================================

E2E_TEST(1, "T1-F9-01", "Compact JSON resource extraction over PE Get") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"ResourceList"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 1u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(!body.empty() && body.front() == '[' && body.back() == ']');
}

E2E_TEST(1, "T1-F9-02", "JSON whitespace around braces, keys, colons, and values") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             2u, " { \"resource\" : \t \"DeviceInfo\"\r\n } "));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 2u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(m2s::ci::ciJsonString(body, "manufacturer") == "Novation");
}

E2E_TEST(1, "T1-F9-03", "Multi-key PE request header extracts resource regardless of field order") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 3u,
	                      R"({"offset":0,"limit":10,"resource":"DeviceInfo","mutualEncoding":"ASCII"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 3u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(m2s::ci::ciJsonString(body, "model") == CI_ENDPOINT_NAME);
}

E2E_TEST(1, "T1-F9-04", "Escaped quotes/backslashes inside JSON string value handled safely") {
	const char hdrJson[] = R"({"resource":"Bad\"Name"})";
	struct ci_json_it it {};
	struct ci_json_member m {};
	ci_json_it_init(&it, hdrJson, static_cast<uint16_t>(sizeof(hdrJson) - 1u));
	E2E_CHECK_EQ(ci_json_next(&it, &m), 1);
	E2E_CHECK_EQ(ci_json_key_is(&m, "resource"), 1u);
	E2E_CHECK_EQ(m.type, static_cast<uint8_t>(CI_JSON_STR));
	E2E_CHECK_EQ(m.str_len, 9u);
	E2E_CHECK_EQ(ci_pe_lookup(m.str, m.str_len), static_cast<uint8_t>(CI_PE_NONE));
	E2E_CHECK_EQ(ci_json_next(&it, &m), 0);

	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             4u, hdrJson));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 4u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 404);
	E2E_CHECK(body.empty());
}

E2E_TEST(1, "T1-F9-05", "Key prefix collision and nested object skipping") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0,
	        make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid, 5u,
	                      R"({"resources":"DeviceInfo","meta":{"resource":"DeviceInfo"},"resource":"ChannelList"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 5u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(body.find("Standalone Port") != std::string::npos);
	E2E_CHECK(body.find("Novation") == std::string::npos);
}

// ============================================================================
// Tier 1 — F10: Flash PE Resources & 404/405 (T1-F10-01..05)
// ============================================================================

E2E_TEST(1, "T1-F10-01", "GET ResourceList returns 200 with all 7 advertised resources (excluding self)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             10u, R"({"resource":"ResourceList"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 10u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK(hdr == R"({"status":200})");
	E2E_CHECK(body == render_expected_pe(CI_PE_RESOURCE_LIST, ui_settings(), "TEST0001"));
	E2E_CHECK(body.find(R"("resource":"ResourceList")") == std::string::npos);
	for (const char* name : {"DeviceInfo", "ChannelList", "X-ScaleSettings", "X-PlayOptions",
	                         "X-ScaleList", "X-TuningList", "X-SurfaceSettings"}) {
		E2E_CHECK(body.find(name) != std::string::npos);
	}
}

E2E_TEST(1, "T1-F10-02", "GET DeviceInfo returns 200 with 7-bit LSB-first IDs and identity strings") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             11u, R"({"resource":"DeviceInfo"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 11u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK(hdr == R"({"status":200})");
	E2E_CHECK(body.find(R"("manufacturerId":[0,32,41])") != std::string::npos);
	E2E_CHECK(body.find(R"("familyId":[81,0])") != std::string::npos);
	E2E_CHECK(body.find(R"("modelId":[0,0])") != std::string::npos);
	E2E_CHECK(body.find(R"("versionId":[0,1,0,0])") != std::string::npos);
	E2E_CHECK(m2s::ci::ciJsonString(body, "manufacturer") == CI_MANUFACTURER_NAME);
	E2E_CHECK(m2s::ci::ciJsonString(body, "family") == CI_FAMILY_NAME);
	E2E_CHECK(m2s::ci::ciJsonString(body, "model") == CI_ENDPOINT_NAME);
	E2E_CHECK(m2s::ci::ciJsonString(body, "version") == "0.1.0.0");
}

E2E_TEST(1, "T1-F10-03", "GET DeviceInfo serialNumber matches EndpointReply ProductInstanceId") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	uint8_t epReq[ms::EndpointInquiryByteCount] = {};
	const size_t epN = ms::BuildEndpointInquiry(0x01020304u, fwMuid, 0x00u, epReq, sizeof(epReq));
	send_ci(fw, 0, std::vector<uint8_t>(epReq, epReq + epN));
	const DrainedOutputs epOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(epOut.ciMsgs.size(), 1u);
	std::string endpointPiid;
	if (epOut.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(epOut.ciMsgs[0].data(), epOut.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		endpointPiid.assign(
		    reinterpret_cast<const char*>(epOut.ciMsgs[0].data() + p.Endpoint.InformationOffset),
		    p.Endpoint.InformationByteCount);
	}

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             12u, R"({"resource":"DeviceInfo"})"));
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
	                              ms::MessageType::PropertyGetDataReply, 12u, fwMuid, 0x01020304u,
	                              320u, hdr, body));
	E2E_CHECK_EQ(m2s::ci::ciJsonInt(hdr, "status", 0), 200);
	E2E_CHECK(!endpointPiid.empty());
	E2E_CHECK(m2s::ci::ciJsonString(body, "serialNumber") == endpointPiid);
}

E2E_TEST(1, "T1-F10-04", "GET unknown resource returns status 404 with empty body") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             42u, R"({"resource":"ProgramList"})"));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 1u);
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(out.ciMsgs, ms::MessageType::PropertyGetDataReply, 42u, fwMuid,
	                              0x01020304u, 320u, hdr, body));
	E2E_CHECK(hdr == R"({"status":404})");
	E2E_CHECK(body.empty());
}

E2E_TEST(1, "T1-F10-05", "Subscription Inquiry on read-only resources returns status 405") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	const char* roHeaders[] = {
	    R"({"command":"start","resource":"DeviceInfo"})",
	    R"({"command":"start","resource":"ResourceList"})",
	};
	for (uint8_t reqId = 1u; reqId <= 2u; ++reqId) {
		send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertySubscriptionInquiry, 0x01020304u,
		                             fwMuid, reqId, roHeaders[reqId - 1u]));
		std::string hdr, body;
		E2E_CHECK(reassemble_pe_reply(drain_outputs(fw, 0).ciMsgs,
		                              ms::MessageType::PropertySubscriptionReply, reqId, fwMuid,
		                              0x01020304u, 320u, hdr, body));
		E2E_CHECK(hdr == R"({"status":405})");
		E2E_CHECK(body.empty());
	}
}

// ============================================================================
// Tier 1 — F11: Chunked PE TX & Peer MaxSysEx Cache (T1-F11-01..05)
// ============================================================================

E2E_TEST(1, "T1-F11-01", "Unchunked DeviceInfo (1/1) and 320B-capped ResourceList for maxSysEx=4096") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             1u, R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs devOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(devOut.ciMsgs.size(), 1u);
	std::string hdr, devBody;
	E2E_CHECK(reassemble_pe_reply(devOut.ciMsgs, ms::MessageType::PropertyGetDataReply, 1u, fwMuid,
	                              0x01020304u, 320u, hdr, devBody));
	E2E_CHECK(devBody == render_expected_pe(CI_PE_DEVICE_INFO, ui_settings(), "TEST0001"));

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             2u, R"({"resource":"ResourceList"})"));
	const DrainedOutputs rlOut = drain_outputs(fw, 0);
	E2E_CHECK(rlOut.ciMsgs.size() > 1u);
	std::string rlHdr, rlBody;
	E2E_CHECK(reassemble_pe_reply(rlOut.ciMsgs, ms::MessageType::PropertyGetDataReply, 2u, fwMuid,
	                              0x01020304u, 320u, rlHdr, rlBody));
	E2E_CHECK(rlBody == render_expected_pe(CI_PE_RESOURCE_LIST, ui_settings(), "TEST0001"));
}

E2E_TEST(1, "T1-F11-02", "Chunked DeviceInfo (N>=2 chunks <= 128B) for maxSysEx=128") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 128u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             9u, R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs out = drain_outputs(fw, 0);
	E2E_CHECK_EQ(out.ciMsgs.size(), 3u);
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(out.ciMsgs, ms::MessageType::PropertyGetDataReply, 9u, fwMuid,
	                              0x01020304u, 128u, hdr, body));
	E2E_CHECK(hdr == R"({"status":200})");
	E2E_CHECK(body == render_expected_pe(CI_PE_DEVICE_INFO, ui_settings(), "TEST0001"));
}

E2E_TEST(1, "T1-F11-03", "Unknown initiator MUID defaults to maxSysEx=512 (clamped to 320)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 128u);
	const uint32_t unknownMuid = 0x0555555u;

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, unknownMuid, fwMuid,
	                             1u, R"({"resource":"DeviceInfo"})"));
	const DrainedOutputs devOut = drain_outputs(fw, 0);
	E2E_CHECK_EQ(devOut.ciMsgs.size(), 1u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, unknownMuid, fwMuid,
	                             2u, R"({"resource":"ResourceList"})"));
	const DrainedOutputs rlOut = drain_outputs(fw, 0);
	E2E_CHECK(rlOut.ciMsgs.size() > 1u);
	if (!rlOut.ciMsgs.empty()) {
		E2E_CHECK_EQ(rlOut.ciMsgs[0].size() + 2u, 320u);
	}
	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(rlOut.ciMsgs, ms::MessageType::PropertyGetDataReply, 2u, fwMuid,
	                              unknownMuid, 320u, hdr, body));
	E2E_CHECK(body == render_expected_pe(CI_PE_RESOURCE_LIST, ui_settings(), "TEST0001"));
}

E2E_TEST(1, "T1-F11-04", "Multi-MUID maxSysEx cache serves 2+ simultaneous peers and evicts LRU past 8") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t peerA = 0x0110001u;
	const uint32_t peerB = 0x0110002u;

	const uint32_t fwMuid = discover_fw_muid(fw, peerA, 128u);
	E2E_CHECK_EQ(discover_fw_muid(fw, peerB, 4096u), fwMuid);

	// Peer A -> 3 chunks (128B); Peer B -> 1 chunk (320B)
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 1u,
	                             R"({"resource":"DeviceInfo"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 3u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerB, fwMuid, 2u,
	                             R"({"resource":"DeviceInfo"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 1u);

	// Fill remaining 6 slots (peers 3..8) with maxSysEx=160 -> peerA & peerB are at slots 7, 6 (MRU=0)
	for (uint32_t i = 3u; i <= 8u; ++i) {
		E2E_CHECK_EQ(discover_fw_muid(fw, 0x0110000u + i, 160u), fwMuid);
	}

	// Touch peerA via GET DeviceInfo -> promotes peerA to MRU slot 0, leaving peerB at LRU slot 7
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 3u,
	                             R"({"resource":"DeviceInfo"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 3u);

	// Register 9th peer (0x0110009, 192B) -> evicts peerB
	E2E_CHECK_EQ(discover_fw_muid(fw, 0x0110009u, 192u), fwMuid);

	// Peer A is still cached at 128B (3 chunks); evicted Peer B falls back to 512->320B
	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerA, fwMuid, 4u,
	                             R"({"resource":"DeviceInfo"})"));
	E2E_CHECK_EQ(drain_outputs(fw, 0).ciMsgs.size(), 3u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, peerB, fwMuid, 5u,
	                             R"({"resource":"ResourceList"})"));
	const DrainedOutputs bRl = drain_outputs(fw, 0);
	E2E_CHECK(bRl.ciMsgs.size() > 1u);
	if (!bRl.ciMsgs.empty()) {
		E2E_CHECK_EQ(bRl.ciMsgs[0].size() + 2u, 320u);
	}
}

E2E_TEST(1, "T1-F11-05", "PE reply emits strictly 1 chunk per main-loop pass") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t fwMuid = discover_fw_muid(fw, 0x01020304u, 4096u);

	send_ci(fw, 0, make_pe_bytes(ms::MessageType::PropertyGetDataInquiry, 0x01020304u, fwMuid,
	                             7u, R"({"resource":"ResourceList"})"));

	// Pass 1: processes GET and emits strictly chunk 1
	__wrap_midi_parsers_poll();
	std::vector<std::vector<uint8_t>> chunks;
	DrainedOutputs pass1 = drain_tx_only_no_poll(fw, 0);
	E2E_CHECK_EQ(pass1.ciMsgs.size(), 1u);
	uint16_t totalChunks = 0;
	if (pass1.ciMsgs.size() == 1u) {
		ms::ParsedMessage p{};
		E2E_CHECK(ms::Parse(pass1.ciMsgs[0].data(), pass1.ciMsgs[0].size(), p) ==
		          ms::ParseStatus::Ok);
		E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, 1u);
		totalChunks = p.PropertyExchange.ChunkCount;
		E2E_CHECK(totalChunks > 2u);
		chunks.push_back(pass1.ciMsgs[0]);
	}

	for (uint16_t c = 2u; c <= totalChunks; ++c) {
		// Extra TX drain without poll must produce 0 messages
		E2E_CHECK(drain_tx_only_no_poll(fw, 0).ciMsgs.empty());

		__wrap_midi_parsers_poll();
		DrainedOutputs passC = drain_tx_only_no_poll(fw, 0);
		E2E_CHECK_EQ(passC.ciMsgs.size(), 1u);
		if (passC.ciMsgs.size() == 1u) {
			ms::ParsedMessage p{};
			E2E_CHECK(ms::Parse(passC.ciMsgs[0].data(), passC.ciMsgs[0].size(), p) ==
			          ms::ParseStatus::Ok);
			E2E_CHECK_EQ(p.PropertyExchange.ChunkNumber, c);
			chunks.push_back(passC.ciMsgs[0]);
		}
	}

	__wrap_midi_parsers_poll();
	E2E_CHECK(drain_tx_only_no_poll(fw, 0).ciMsgs.empty());

	std::string hdr, body;
	E2E_CHECK(reassemble_pe_reply(chunks, ms::MessageType::PropertyGetDataReply, 7u, fwMuid,
	                              0x01020304u, 320u, hdr, body));
	E2E_CHECK(body == render_expected_pe(CI_PE_RESOURCE_LIST, ui_settings(), "TEST0001"));
}

#endif  // E2E_HAS_M2
