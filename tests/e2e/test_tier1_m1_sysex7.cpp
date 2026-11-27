#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump.h"

extern "C" {
#include "ump/ump.h"
}

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#if E2E_HAS_M1

namespace {

namespace ms = m2s::ci::ms;

struct CapturedCiMsg {
	uint8_t group = 0;
	std::vector<uint8_t> payload;
};

std::vector<CapturedCiMsg>& capture_log() {
	static std::vector<CapturedCiMsg> log;
	return log;
}

void capture_handler(uint8_t group, const uint8_t* payload, uint16_t len) {
	CapturedCiMsg item;
	item.group = group;
	if (payload && len > 0) {
		item.payload.assign(payload, payload + len);
	}
	capture_log().push_back(std::move(item));
}

void install_capture() {
	capture_log().clear();
	midici_set_handler(capture_handler);
}

std::vector<uint8_t> make_ci_payload(size_t len, uint8_t seed) {
	std::vector<uint8_t> out(len, 0u);
	if (len >= 1) out[0] = 0x7Eu;
	if (len >= 2) out[1] = 0x7Fu;
	if (len >= 3) out[2] = 0x0Du;
	if (len >= 4) out[3] = 0x70u;
	if (len >= 5) out[4] = 0x02u;
	for (size_t i = 5; i < len; ++i) {
		out[i] = static_cast<uint8_t>((seed + i * 17u) & 0x7Fu);
	}
	return out;
}

std::vector<uint8_t> make_nonci_payload(size_t len, uint8_t seed) {
	std::vector<uint8_t> out(len, 0u);
	if (len >= 1) out[0] = 0x00u;
	if (len >= 2) out[1] = 0x20u;
	if (len >= 3) out[2] = 0x29u;
	for (size_t i = 3; i < len; ++i) {
		out[i] = static_cast<uint8_t>((seed + i * 13u) & 0x7Fu);
	}
	return out;
}

std::vector<uint8_t> make_discovery_inquiry(uint32_t src_muid, uint32_t max_sysex) {
	ms::DiscoveryReplyFields f{};
	f.SourceMuid = src_muid;
	f.DestinationMuid = ms::MuidBroadcast;
	f.ManufacturerSysExId[0] = 0x7Du;
	f.ManufacturerSysExId[1] = 0x00u;
	f.ManufacturerSysExId[2] = 0x00u;
	f.DeviceFamily = 0x0001u;
	f.DeviceFamilyModelNumber = 0x0001u;
	f.SoftwareRevisionLevel[1] = 0x01u;
	f.CapabilityCategories = 0x08u;
	f.ReceivableMaximumSysExSize = max_sysex;
	f.OutputPathId = 0x00u;
	f.MessageVersion = ms::MessageVersion12;
	std::vector<uint8_t> buf(ms::DiscoveryByteCount, 0u);
	const size_t n = ms::BuildDiscovery(f, buf.data(), buf.size());
	buf.resize(n);
	return buf;
}

}  // namespace

// ============================================================================
// Tier 1 — F1: SysEx7 Per-Group Reassembly (T1-F1-01..05)
// ============================================================================

E2E_TEST(1, "T1-F1-01", "Single-packet kComplete MIDI-CI message dispatches cleanly on group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> six = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x01u};
	const auto words = e2e::pack_sysex7_words(0, six);
	E2E_CHECK_EQ(words.size(), 2u);
	E2E_CHECK_EQ((words[0] >> 20) & 0xFu, 0u);  // kComplete
	fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);

	install_capture();
	const std::vector<uint8_t> five = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u};
	const auto words5 = e2e::pack_sysex7_words(0, five);
	fw.send_ump(words5.data(), static_cast<uint16_t>(words5.size()));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK_EQ(capture_log()[0].group, 0u);
		E2E_CHECK(capture_log()[0].payload == five);
	}
}

E2E_TEST(1, "T1-F1-02", "Multi-packet MIDI-CI message reassembly (kStart + kContinue + kEnd)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg40 = make_ci_payload(40, 0x11u);
	const auto words40 = e2e::pack_sysex7_words(3, msg40);
	E2E_CHECK_EQ(words40.size(), 14u);  // 7 packets: kStart + 5*kContinue + kEnd
	fw.send_ump(words40.data(), static_cast<uint16_t>(words40.size()));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK_EQ(capture_log()[0].group, 3u);
		E2E_CHECK(capture_log()[0].payload == msg40);
	}

	// End-to-end Responder verification on group 0 with multi-packet Discovery Inquiry
	midici_responder_init();
	const std::vector<uint8_t> disc = make_discovery_inquiry(0x01020304u, 512u);
	const auto discWords = e2e::pack_sysex7_words(0, disc);
	fw.send_ump(discWords.data(), static_cast<uint16_t>(discWords.size()));
	fw.poll(12);

	const std::vector<m2s::UmpPacket> txPkts = fw.drain_tx_packets();
	std::vector<uint8_t> reply;
	uint64_t m2sDrops = 0;
	E2E_CHECK(e2e::reassemble_sysex7(txPkts, 0, reply, &m2sDrops));
	E2E_CHECK_EQ(m2sDrops, 0u);
	E2E_CHECK(reply.size() >= 31u);
	if (reply.size() >= 4u) {
		E2E_CHECK_EQ(reply[0], 0x7Eu);
		E2E_CHECK_EQ(reply[1], 0x7Fu);
		E2E_CHECK_EQ(reply[2], 0x0Du);
		E2E_CHECK_EQ(reply[3], 0x71u);
	}
}

E2E_TEST(1, "T1-F1-03", "Max 512-byte MIDI-CI SysEx7 message reassembly on groups 0 and 15") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msgA = make_ci_payload(512, 0x21u);
	const std::vector<uint8_t> msgB = make_ci_payload(512, 0x53u);
	const auto wordsA = e2e::pack_sysex7_words(0, msgA);
	const auto wordsB = e2e::pack_sysex7_words(15, msgB);
	E2E_CHECK_EQ(wordsA.size(), 86u * 2u);

	fw.send_ump(wordsA.data(), static_cast<uint16_t>(wordsA.size()));
	fw.poll(4);
	fw.send_ump(wordsB.data(), static_cast<uint16_t>(wordsB.size()));
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);
	if (capture_log().size() == 2u) {
		E2E_CHECK_EQ(capture_log()[0].group, 0u);
		E2E_CHECK(capture_log()[0].payload == msgA);
		E2E_CHECK_EQ(capture_log()[1].group, 15u);
		E2E_CHECK(capture_log()[1].payload == msgB);
	}
}

E2E_TEST(1, "T1-F1-04", "MIDI-CI vs non-CI SysEx7 dispatch discrimination across groups 0, 1, 2") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);  // Default Responder handler active

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const auto nonCi0 = e2e::pack_sysex7_words(0, make_nonci_payload(12, 0x05u));
	const auto nonCi1 = e2e::pack_sysex7_words(1, make_nonci_payload(20, 0x15u));
	const auto nonCi2 = e2e::pack_sysex7_words(2, make_nonci_payload(20, 0x25u));
	fw.send_ump(nonCi0.data(), static_cast<uint16_t>(nonCi0.size()));
	fw.send_ump(nonCi1.data(), static_cast<uint16_t>(nonCi1.size()));
	fw.send_ump(nonCi2.data(), static_cast<uint16_t>(nonCi2.size()));
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(fw.drain_ump().empty());

	// MIDI-CI on groups 1 and 2 counts in midici_messages_received(), but Responder only replies on group 0
	const std::vector<uint8_t> disc = make_discovery_inquiry(0x01230001u, 512u);
	const auto ci1 = e2e::pack_sysex7_words(1, disc);
	const auto ci2 = e2e::pack_sysex7_words(2, disc);
	fw.send_ump(ci1.data(), static_cast<uint16_t>(ci1.size()));
	fw.send_ump(ci2.data(), static_cast<uint16_t>(ci2.size()));
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK(fw.drain_ump().empty());
}

E2E_TEST(1, "T1-F1-05", "Four simultaneous interleaved MIDI-CI groups (0, 1, 3, 15) reassemble cleanly") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint8_t groups[4] = {0u, 1u, 3u, 15u};
	const std::vector<uint8_t> msgs[4] = {
	    make_ci_payload(18, 0x01u),  // 3 pkts
	    make_ci_payload(14, 0x11u),  // 3 pkts
	    make_ci_payload(20, 0x21u),  // 4 pkts
	    make_ci_payload(25, 0x31u),  // 5 pkts
	};
	std::vector<uint32_t> pkts[4];
	size_t maxPkts = 0;
	for (int i = 0; i < 4; ++i) {
		pkts[i] = e2e::pack_sysex7_words(groups[i], msgs[i]);
		const size_t n = pkts[i].size() / 2u;
		if (n > maxPkts) maxPkts = n;
	}

	// Interleave kStart + kContinue across all 4 groups, holding each group's kEnd
	for (size_t p = 0; p + 1 < maxPkts; ++p) {
		for (int g = 0; g < 4; ++g) {
			const size_t totalPkts = pkts[g].size() / 2u;
			if (p + 1 < totalPkts) {
				fw.send_ump(&pkts[g][2u * p], 2);
			}
		}
		fw.poll(1);
	}
	E2E_CHECK_EQ(capture_log().size(), 0u);

	// Send kEnd in reverse group order (15, 3, 1, 0)
	for (int g = 3; g >= 0; --g) {
		const size_t last = (pkts[g].size() / 2u) - 1u;
		fw.send_ump(&pkts[g][2u * last], 2);
		fw.poll(1);
	}

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 4u);
	E2E_CHECK_EQ(capture_log().size(), 4u);
	if (capture_log().size() == 4u) {
		// Reverse kEnd order determines completion order: 15, 3, 1, 0
		for (int idx = 0; idx < 4; ++idx) {
			const int g = 3 - idx;
			E2E_CHECK_EQ(capture_log()[static_cast<size_t>(idx)].group, groups[g]);
			E2E_CHECK(capture_log()[static_cast<size_t>(idx)].payload == msgs[g]);
		}
	}
}

// ============================================================================
// Tier 1 — F2: SysEx7 Malformed Drop & Count (T1-F2-01..05)
// ============================================================================

E2E_TEST(1, "T1-F2-01", "Start-while-active on same group discards partial, increments drops, completes new") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msgA = make_ci_payload(18, 0x0Au);
	const std::vector<uint8_t> msgB = make_ci_payload(10, 0x0Bu);
	const auto wordsA = e2e::pack_sysex7_words(0, msgA);
	const auto wordsB = e2e::pack_sysex7_words(0, msgB);

	// Send only kStart of A
	fw.send_ump(wordsA.data(), 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(capture_log().size(), 0u);

	// Send kStart + kEnd of B on same group 0
	fw.send_ump(wordsB.data(), static_cast<uint16_t>(wordsB.size()));
	fw.poll(1);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK(capture_log()[0].payload == msgB);
	}
}

E2E_TEST(1, "T1-F2-02", "Orphaned kContinue on idle groups (0, 1, 2, 15) is dropped and counted") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint32_t orphans[8] = {
	    0x30267E7Fu, 0x0D700201u,
	    0x31267E7Fu, 0x0D700201u,
	    0x32267E7Fu, 0x0D700201u,
	    0x3F267E7Fu, 0x0D700201u,
	};
	fw.send_ump(orphans, 8);
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 4u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());
}

E2E_TEST(1, "T1-F2-03", "Orphaned kEnd on idle groups (0, 1, 15) is dropped and counted") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint32_t orphans[6] = {
	    0x30327E7Fu, 0x00000000u,
	    0x31327E7Fu, 0x00000000u,
	    0x3F327E7Fu, 0x00000000u,
	};
	fw.send_ump(orphans, 6);
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 3u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());
}

E2E_TEST(1, "T1-F2-04", "Over-long SysEx7 (>512 bytes) is dropped, counted, and frees slot for recovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg516 = make_ci_payload(516, 0x44u);
	const auto words516 = e2e::pack_sysex7_words(0, msg516);
	E2E_CHECK_EQ(words516.size(), 86u * 2u);

	// First 85 packets = 510 bytes (within 512B limit)
	fw.send_ump(words516.data(), static_cast<uint16_t>(85u * 2u));
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK(capture_log().empty());

	// 86th packet pushes total to 516 > 512
	fw.send_ump(&words516[85u * 2u], 2);
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());

	// Subsequent valid 12-byte message on group 0 succeeds cleanly
	const std::vector<uint8_t> msg12 = make_ci_payload(12, 0x19u);
	const auto words12 = e2e::pack_sysex7_words(0, msg12);
	fw.send_ump(words12.data(), static_cast<uint16_t>(words12.size()));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK(capture_log()[0].payload == msg12);
	}
}

E2E_TEST(1, "T1-F2-05", "Invalid form (>= 4) and invalid byte count (> 6) MT 0x3 packets drop and count") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint32_t bad[8] = {
	    0x30077E7Fu, 0x0D010203u,  // cnt = 7
	    0x300F7E7Fu, 0x0D010203u,  // cnt = 15
	    0x30417E00u, 0x00000000u,  // form = 4
	    0x30F17E00u, 0x00000000u,  // form = 15
	};
	fw.send_ump(bad, 8);
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 4u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());
}

// ============================================================================
// Tier 1 — F3: packetizeSysEx7 Port & midici_send_sysex (T1-F3-01..05)
// ============================================================================

E2E_TEST(1, "T1-F3-01", "midici_packetize_sysex7 kComplete forms for 0, 1, 5, 6 bytes and zero-padding") {
	uint32_t pkts[2][2] = {};

	// 0 bytes -> 1 empty kComplete packet
	const uint16_t n0 = midici_packetize_sysex7(5, nullptr, 0, pkts, 2);
	E2E_CHECK_EQ(n0, 1u);
	E2E_CHECK_EQ(pkts[0][0], 0x35000000u);
	E2E_CHECK_EQ(pkts[0][1], 0x00000000u);

	// 1 byte
	const uint8_t d1[1] = {0x55u};
	const uint16_t n1 = midici_packetize_sysex7(2, d1, 1, pkts, 2);
	E2E_CHECK_EQ(n1, 1u);
	E2E_CHECK_EQ(pkts[0][0], 0x32015500u);
	E2E_CHECK_EQ(pkts[0][1], 0x00000000u);

	// 5 bytes
	const uint8_t d5[5] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u};
	const uint16_t n5 = midici_packetize_sysex7(15, d5, 5, pkts, 2);
	E2E_CHECK_EQ(n5, 1u);
	E2E_CHECK_EQ(pkts[0][0], 0x3F057E7Fu);
	E2E_CHECK_EQ(pkts[0][1], 0x0D700200u);

	// 6 bytes
	const uint8_t d6[6] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u};
	const uint16_t n6 = midici_packetize_sysex7(0, d6, 6, pkts, 2);
	E2E_CHECK_EQ(n6, 1u);
	E2E_CHECK_EQ(pkts[0][0], 0x30061122u);
	E2E_CHECK_EQ(pkts[0][1], 0x33445566u);
}

E2E_TEST(1, "T1-F3-02", "midici_packetize_sysex7 multi-packet forms (7, 12, 13, 512 bytes) match packetizeSysEx7") {
	const struct {
		size_t len;
		uint8_t group;
		uint16_t expectedPkts;
	} cases[] = {
	    {7u, 0u, 2u},
	    {12u, 3u, 2u},
	    {13u, 7u, 3u},
	    {512u, 15u, 86u},
	};

	for (const auto& c : cases) {
		const std::vector<uint8_t> payload = make_ci_payload(c.len, static_cast<uint8_t>(c.len & 0x7Fu));
		const auto ref = m2s::ci::packetizeSysEx7(c.group, payload.data(), payload.size());
		E2E_CHECK_EQ(ref.size(), c.expectedPkts);

		std::array<uint32_t, 2> out[90] = {};
		uint32_t rawOut[90][2] = {};
		const uint16_t got = midici_packetize_sysex7(c.group, payload.data(), static_cast<uint16_t>(c.len),
		                                             rawOut, 90);
		E2E_CHECK_EQ(got, c.expectedPkts);
		for (uint16_t i = 0; i < got && i < ref.size(); ++i) {
			out[i][0] = rawOut[i][0];
			out[i][1] = rawOut[i][1];
			E2E_CHECK_EQ(out[i][0], ref[i][0]);
			E2E_CHECK_EQ(out[i][1], ref[i][1]);
			const uint8_t form = static_cast<uint8_t>((out[i][0] >> 20) & 0xFu);
			if (i == 0) E2E_CHECK_EQ(form, 1u);
			else if (i + 1u == got) E2E_CHECK_EQ(form, 3u);
			else E2E_CHECK_EQ(form, 2u);
		}
	}
}

E2E_TEST(1, "T1-F3-03", "midici_packetize_sysex7 masks bytes & 0x7F and preserves F0/F7 as raw payload") {
	const uint8_t raw[7] = {0xF0u, 0x81u, 0xFEu, 0xFFu, 0x80u, 0xABu, 0xF7u};
	uint32_t pkts[2][2] = {};
	const uint16_t n = midici_packetize_sysex7(1, raw, 7, pkts, 2);
	E2E_CHECK_EQ(n, 2u);
	// 0xF0&7F=0x70, 0x81&7F=0x01, 0xFE&7F=0x7E, 0xFF&7F=0x7F, 0x80&7F=0x00, 0xAB&7F=0x2B; 0xF7&7F=0x77
	E2E_CHECK_EQ(pkts[0][0], 0x31167001u);
	E2E_CHECK_EQ(pkts[0][1], 0x7E7F002Bu);
	E2E_CHECK_EQ(pkts[1][0], 0x31317700u);
	E2E_CHECK_EQ(pkts[1][1], 0x00000000u);
}

E2E_TEST(1, "T1-F3-04", "midici_send_sysex strips optional F0/F7 framing and queues valid SysEx7 on EP1 IN") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// Wrapped with F0..F7
	const uint8_t framed[6] = {0xF0u, 0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0xF7u};
	E2E_CHECK_EQ(fw.send_sysex(0, framed, sizeof(framed)), 0);
	fw.poll(4);
	std::vector<uint8_t> out1;
	uint64_t drops = 0;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, out1, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK(out1 == std::vector<uint8_t>({0x7Eu, 0x7Fu, 0x0Du, 0x70u}));

	// Empty {0xF0, 0xF7} -> 1 empty kComplete packet
	const uint8_t emptyFramed[2] = {0xF0u, 0xF7u};
	E2E_CHECK_EQ(fw.send_sysex(0, emptyFramed, sizeof(emptyFramed)), 0);
	fw.poll(4);
	const auto emptyWords = fw.drain_ump();
	E2E_CHECK_EQ(emptyWords.size(), 2u);
	if (emptyWords.size() == 2u) {
		E2E_CHECK_EQ(emptyWords[0], 0x30000000u);
		E2E_CHECK_EQ(emptyWords[1], 0x00000000u);
	}

	// Raw payload without F0/F7
	const uint8_t raw[4] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u};
	E2E_CHECK_EQ(fw.send_sysex(0, raw, sizeof(raw)), 0);
	fw.poll(4);
	std::vector<uint8_t> out2;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, out2, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK(out2 == std::vector<uint8_t>({0x7Eu, 0x7Fu, 0x0Du, 0x70u}));
}

E2E_TEST(1, "T1-F3-05", "Group encoding 0..15 in midici_packetize_sysex7 and midici_send_sysex") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	for (uint8_t g = 0; g < 16; ++g) {
		const uint8_t payload[5] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, g};
		uint32_t pkts[1][2] = {};
		E2E_CHECK_EQ(midici_packetize_sysex7(g, payload, sizeof(payload), pkts, 1), 1u);
		E2E_CHECK_EQ((pkts[0][0] >> 28) & 0xFu, 3u);
		E2E_CHECK_EQ((pkts[0][0] >> 24) & 0xFu, g);

		E2E_CHECK_EQ(fw.send_sysex(g, payload, sizeof(payload)), 0);
		fw.poll(2);
		const auto tx = fw.drain_tx_packets();
		E2E_CHECK_EQ(tx.size(), 1u);
		if (tx.size() == 1u) {
			E2E_CHECK_EQ(m2s::umpGroup(tx[0]), g);
		}
		std::vector<uint8_t> decoded;
		uint64_t drops = 0;
		E2E_CHECK(e2e::reassemble_sysex7(tx, g, decoded, &drops));
		E2E_CHECK_EQ(drops, 0u);
		E2E_CHECK(decoded == std::vector<uint8_t>(payload, payload + sizeof(payload)));
	}
}

// ============================================================================
// Tier 1 — F4: SysEx7 Round-Trip Verification (T1-F4-01..05)
// ============================================================================

E2E_TEST(1, "T1-F4-01", "TX round-trip (midici_send_sysex -> EP1 IN -> reassemble_sysex7) for 0, 1, 5, 6, 7, 13, 512 bytes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const size_t lengths[] = {0u, 1u, 5u, 6u, 7u, 13u, 512u};
	for (size_t len : lengths) {
		const std::vector<uint8_t> input =
		    (len >= 3u) ? make_ci_payload(len, static_cast<uint8_t>(len & 0x7Fu))
		                : make_nonci_payload(len, 0x33u);
		const uint8_t* ptr = input.empty() ? nullptr : input.data();
		E2E_CHECK_EQ(fw.send_sysex(0, ptr, static_cast<uint16_t>(input.size())), 0);
		fw.poll(25);  // 512B = 86 pkts -> ceil(86/4) = 22 passes

		const auto pkts = fw.drain_tx_packets();
		std::vector<uint8_t> decoded;
		uint64_t drops = 0;
		E2E_CHECK(e2e::reassemble_sysex7(pkts, 0, decoded, &drops));
		E2E_CHECK_EQ(drops, 0u);
		E2E_CHECK(decoded == input);
	}
}

E2E_TEST(1, "T1-F4-02", "RX round-trip (pack_sysex7_words -> EP1 OUT -> midici_poll) for 0, 1, 5, 6, 7, 13, 512 bytes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	uint32_t expectedRx = 0;
	const uint32_t rx0 = midici_messages_received();

	// 5, 6, 7, 13, 512 bytes as MIDI-CI on group 2
	const size_t ciLengths[] = {5u, 6u, 7u, 13u, 512u};
	for (size_t len : ciLengths) {
		capture_log().clear();
		const std::vector<uint8_t> input = make_ci_payload(len, static_cast<uint8_t>(len & 0x7Fu));
		const auto words = e2e::pack_sysex7_words(2, input);
		fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
		fw.poll(4);
		++expectedRx;

		E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
		E2E_CHECK_EQ(midici_messages_received() - rx0, expectedRx);
		E2E_CHECK_EQ(capture_log().size(), 1u);
		if (capture_log().size() == 1u) {
			E2E_CHECK_EQ(capture_log()[0].group, 2u);
			E2E_CHECK(capture_log()[0].payload == input);
		}
	}

	// 0 and 1 byte non-CI payloads on group 0 process cleanly with zero drops and zero CI count
	capture_log().clear();
	for (size_t len : {size_t{0}, size_t{1}}) {
		const std::vector<uint8_t> nonCi = make_nonci_payload(len, 0x2Au);
		const auto words = e2e::pack_sysex7_words(0, nonCi);
		fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
		fw.poll(2);
	}
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, expectedRx);
	E2E_CHECK(capture_log().empty());
}

E2E_TEST(1, "T1-F4-03", "Full loopback round-trip (midici_send_sysex -> EP1 IN -> EP1 OUT -> RX handler) for 5, 6, 7, 13, 512 bytes") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const size_t lengths[] = {5u, 6u, 7u, 13u, 512u};
	for (size_t len : lengths) {
		capture_log().clear();
		const std::vector<uint8_t> input = make_ci_payload(len, static_cast<uint8_t>(len + 5u));
		E2E_CHECK_EQ(fw.send_sysex(0, input.data(), static_cast<uint16_t>(input.size())), 0);
		fw.poll(25);

		const std::vector<uint32_t> wireWords = fw.drain_ump();
		E2E_CHECK(!wireWords.empty());
		fw.send_ump(wireWords.data(), static_cast<uint16_t>(wireWords.size()));
		fw.poll(4);

		E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
		E2E_CHECK_EQ(capture_log().size(), 1u);
		if (capture_log().size() == 1u) {
			E2E_CHECK_EQ(capture_log()[0].group, 0u);
			E2E_CHECK(capture_log()[0].payload == input);
		}
	}
}

E2E_TEST(1, "T1-F4-04", "Interleaved multi-group round-trip (MIDI-CI group 0 + non-CI group 1 + MIDI-CI group 2)") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> ci0 = make_ci_payload(20, 0x07u);    // 4 pkts
	const std::vector<uint8_t> non1 = make_nonci_payload(10, 0x17u);  // 2 pkts
	const std::vector<uint8_t> ci2 = make_ci_payload(15, 0x27u);    // 3 pkts

	const auto w0 = e2e::pack_sysex7_words(0, ci0);
	const auto w1 = e2e::pack_sysex7_words(1, non1);
	const auto w2 = e2e::pack_sysex7_words(2, ci2);

	// Step 1: kStart for all 3
	fw.send_ump(&w0[0], 2);
	fw.send_ump(&w1[0], 2);
	fw.send_ump(&w2[0], 2);
	fw.poll(1);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);

	// Step 2: group 0 kContinue, group 1 kEnd (non-CI finishes silently), group 2 kContinue
	fw.send_ump(&w0[2], 2);
	fw.send_ump(&w1[2], 2);
	fw.send_ump(&w2[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);

	// Step 3: group 2 kEnd -> completes (+1), group 0 kContinue
	fw.send_ump(&w2[4], 2);
	fw.send_ump(&w0[4], 2);
	fw.poll(1);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);

	// Step 4: group 0 kEnd -> completes (+2)
	fw.send_ump(&w0[6], 2);
	fw.poll(1);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);
	if (capture_log().size() == 2u) {
		E2E_CHECK_EQ(capture_log()[0].group, 2u);
		E2E_CHECK(capture_log()[0].payload == ci2);
		E2E_CHECK_EQ(capture_log()[1].group, 0u);
		E2E_CHECK(capture_log()[1].payload == ci0);
	}
}

E2E_TEST(1, "T1-F4-05", "Orphaned kContinue before and between valid 5B and 13B MIDI-CI round-trips") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint32_t orphanCont[2] = {0x30267E7Fu, 0x0D700201u};
	const std::vector<uint8_t> msg5 = make_ci_payload(5, 0x41u);
	const std::vector<uint8_t> msg13 = make_ci_payload(13, 0x42u);
	const auto w5 = e2e::pack_sysex7_words(0, msg5);
	const auto w13 = e2e::pack_sysex7_words(0, msg13);

	fw.send_ump(orphanCont, 2);
	fw.poll(1);
	fw.send_ump(w5.data(), static_cast<uint16_t>(w5.size()));
	fw.poll(1);
	fw.send_ump(orphanCont, 2);
	fw.poll(1);
	fw.send_ump(w13.data(), static_cast<uint16_t>(w13.size()));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 2u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);
	if (capture_log().size() == 2u) {
		E2E_CHECK(capture_log()[0].payload == msg5);
		E2E_CHECK(capture_log()[1].payload == msg13);
	}
}

#endif  // E2E_HAS_M1
