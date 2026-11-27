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
		out[i] = static_cast<uint8_t>((seed + i * 29u) & 0x7Fu);
	}
	return out;
}

std::vector<uint8_t> make_nonci_payload(size_t len, uint8_t seed) {
	std::vector<uint8_t> out(len, 0u);
	if (len >= 1) out[0] = 0x00u;
	if (len >= 2) out[1] = 0x20u;
	if (len >= 3) out[2] = 0x29u;
	for (size_t i = 3; i < len; ++i) {
		out[i] = static_cast<uint8_t>((seed + i * 19u) & 0x7Fu);
	}
	return out;
}

std::vector<uint8_t> make_discovery_inquiry(uint32_t src_muid, uint32_t max_sysex) {
	ms::DiscoveryReplyFields f{};
	f.SourceMuid = src_muid;
	f.DestinationMuid = ms::MuidBroadcast;
	f.ManufacturerSysExId[0] = 0x7Du;
	f.DeviceFamily = 0x0001u;
	f.DeviceFamilyModelNumber = 0x0001u;
	f.SoftwareRevisionLevel[1] = 0x01u;
	f.CapabilityCategories = 0x08u;
	f.ReceivableMaximumSysExSize = max_sysex;
	f.MessageVersion = ms::MessageVersion12;
	std::vector<uint8_t> buf(ms::DiscoveryByteCount, 0u);
	buf.resize(ms::BuildDiscovery(f, buf.data(), buf.size()));
	return buf;
}

void append_le32(std::vector<uint8_t>& out, uint32_t w) {
	out.push_back(static_cast<uint8_t>(w & 0xFFu));
	out.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
}

}  // namespace

// ============================================================================
// Tier 2 — F1: SysEx7 Per-Group Reassembly Edge Cases (T2-F1-01..05)
// ============================================================================

E2E_TEST(2, "T2-F1-01", "MIDI-CI header boundary classification and split 7E..0D header across kStart+kEnd") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	// Non-CI boundaries: len=2 {7E,7F}, wrong sub-id {7E,7F,0E}, wrong universal id {7D,7F,0D}
	const std::vector<uint8_t> non1 = {0x7Eu, 0x7Fu};
	const std::vector<uint8_t> non2 = {0x7Eu, 0x7Fu, 0x0Eu};
	const std::vector<uint8_t> non3 = {0x7Du, 0x7Fu, 0x0Du};
	for (const auto& v : {non1, non2, non3}) {
		const auto w = e2e::pack_sysex7_words(0, v);
		fw.send_ump(w.data(), static_cast<uint16_t>(w.size()));
	}
	fw.poll(2);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK(capture_log().empty());

	// Valid MIDI-CI boundaries: deviceId=0x00, minimal 3-byte {7E,7F,0D}, and split header kStart{7E,7F}+kEnd{0D,70}
	const std::vector<uint8_t> ciDev0 = {0x7Eu, 0x00u, 0x0Du};
	const std::vector<uint8_t> ciMin = {0x7Eu, 0x7Fu, 0x0Du};
	const auto wDev0 = e2e::pack_sysex7_words(0, ciDev0);
	const auto wMin = e2e::pack_sysex7_words(1, ciMin);
	const uint32_t split[4] = {
	    0x32127E7Fu, 0x00000000u,  // group 2 kStart cnt=2: 7E 7F
	    0x32320D70u, 0x00000000u,  // group 2 kEnd   cnt=2: 0D 70
	};

	fw.send_ump(wDev0.data(), static_cast<uint16_t>(wDev0.size()));
	fw.send_ump(wMin.data(), static_cast<uint16_t>(wMin.size()));
	fw.send_ump(split, 4);
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 3u);
	E2E_CHECK_EQ(capture_log().size(), 3u);
	if (capture_log().size() == 3u) {
		E2E_CHECK(capture_log()[0].payload == ciDev0);
		E2E_CHECK(capture_log()[1].payload == ciMin);
		E2E_CHECK_EQ(capture_log()[2].group, 2u);
		E2E_CHECK(capture_log()[2].payload == std::vector<uint8_t>({0x7Eu, 0x7Fu, 0x0Du, 0x70u}));
	}
}

E2E_TEST(2, "T2-F1-02", "RX high-bit (& 0x7F) masking in raw MT 0x3 words across boundary groups") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	// Single packet on group 2 with high bits set: 0xFE->0x7E, 0xAA->0x2A, 0x8D->0x0D, 0xC1->0x41
	const uint32_t single[2] = {0x3204FEAAu, 0x8DC10000u};
	// Multi-packet on group 15 with high bits set in kStart and kEnd
	const uint32_t multi[4] = {
	    0x3F16FEFFu, 0x8DF08280u,  // 7E 7F 0D 70 02 00
	    0x3F32C3F7u, 0x00000000u,  // 43 77
	};
	fw.send_ump(single, 2);
	fw.send_ump(multi, 4);
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);
	if (capture_log().size() == 2u) {
		E2E_CHECK_EQ(capture_log()[0].group, 2u);
		E2E_CHECK(capture_log()[0].payload == std::vector<uint8_t>({0x7Eu, 0x2Au, 0x0Du, 0x41u}));
		E2E_CHECK_EQ(capture_log()[1].group, 15u);
		E2E_CHECK(capture_log()[1].payload ==
		          std::vector<uint8_t>({0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x00u, 0x43u, 0x77u}));
	}
}

E2E_TEST(2, "T2-F1-03", "EP1 OUT ISR 32-bit framing split across USB packets and mixed MT 0x4 + MT 0x3 + MT 0xF") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	// Split 2-word MT 0x3 kComplete across two 4-byte EP1 OUT USB frames
	std::vector<uint8_t> usb0, usb1;
	append_le32(usb0, 0x30047E7Fu);
	append_le32(usb1, 0x0D700000u);

	e2e_host_send_ep1_raw(usb0.data(), static_cast<uint16_t>(usb0.size()));
	fw.poll(2);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());

	e2e_host_send_ep1_raw(usb1.data(), static_cast<uint16_t>(usb1.size()));
	fw.poll(2);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK(capture_log()[0].payload == std::vector<uint8_t>({0x7Eu, 0x7Fu, 0x0Du, 0x70u}));
	}

	// Single USB packet containing MT4 NoteOn (2w) + MT3 MIDI-CI (2w) + MT15 EP Discovery (4w) = 8 words (32B)
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	const uint32_t mixed[8] = {
	    0x40903C00u, 0x80000000u,
	    0x30057E7Fu, 0x0D700200u,
	    epDisc[0], epDisc[1], epDisc[2], epDisc[3],
	};
	fw.send_ump(mixed, 8);
	fw.poll(4);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);

	const std::vector<uint32_t> tx = fw.drain_ump();
	bool sawEpInfo = false;
	for (size_t i = 0; i < tx.size();) {
		const uint8_t wc = ump_word_count(tx[i]);
		if (wc == 0 || i + wc > tx.size()) break;
		if ((tx[i] >> 28) == 0xFu && ((tx[i] >> 16) & 0x3FFu) == 0x001u) {
			sawEpInfo = true;
		}
		i += wc;
	}
	E2E_CHECK(sawEpInfo);
}

E2E_TEST(2, "T2-F1-04", "4-slot LRU pool eviction boundary: kContinue promotion, kComplete non-eviction, 5th-group eviction") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> m0 = make_ci_payload(18, 0x01u);  // 3 pkts
	const std::vector<uint8_t> m1 = make_ci_payload(12, 0x02u);  // 2 pkts
	const std::vector<uint8_t> m3 = make_ci_payload(12, 0x03u);  // 2 pkts
	const std::vector<uint8_t> m15 = make_ci_payload(12, 0x04u); // 2 pkts
	const std::vector<uint8_t> m7 = make_ci_payload(12, 0x05u);  // 2 pkts

	const auto w0 = e2e::pack_sysex7_words(0, m0);
	const auto w1 = e2e::pack_sysex7_words(1, m1);
	const auto w3 = e2e::pack_sysex7_words(3, m3);
	const auto w15 = e2e::pack_sysex7_words(15, m15);
	const auto w7 = e2e::pack_sysex7_words(7, m7);

	// Fill all 4 slots: LRU order from least->most recent becomes 0, 1, 3, 15
	fw.send_ump(&w0[0], 2);
	fw.send_ump(&w1[0], 2);
	fw.send_ump(&w3[0], 2);
	fw.send_ump(&w15[0], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);

	// Promote group 0 via kContinue -> group 1 becomes LRU
	fw.send_ump(&w0[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);

	// kComplete on idle group 5 uses stack buffer and must NOT evict any slot
	const std::vector<uint8_t> m5 = {0x7Eu, 0x7Fu, 0x0Du, 0x70u};
	const auto w5 = e2e::pack_sysex7_words(5, m5);
	fw.send_ump(w5.data(), static_cast<uint16_t>(w5.size()));
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(capture_log().size(), 1u);

	// 5th multi-packet kStart on group 7 evicts LRU group 1 (+1 drop)
	fw.send_ump(&w7[0], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);

	// Finish surviving groups 0, 3, 15, 7 -> all 4 complete cleanly
	fw.send_ump(&w0[4], 2);
	fw.send_ump(&w3[2], 2);
	fw.send_ump(&w15[2], 2);
	fw.send_ump(&w7[2], 2);
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 5u);
	E2E_CHECK_EQ(capture_log().size(), 5u);

	// Trailing kEnd of evicted group 1 is now orphaned (+2 drops total)
	fw.send_ump(&w1[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 2u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 5u);
}

E2E_TEST(2, "T2-F1-05", "USB alt 1 -> alt 0 -> alt 1 and ump_rx_reset mid-reassembly discards partial slots cleanly") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg = make_ci_payload(12, 0x3Au);
	const auto w = e2e::pack_sysex7_words(0, msg);

	fw.send_ump(&w[0], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);

	fw.set_alt(0);
	fw.poll(2);
	fw.set_alt(1);
	fw.poll(2);

	// kEnd of pre-reset message must drop as orphaned
	fw.send_ump(&w[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
	E2E_CHECK(capture_log().empty());

	// Fresh full message reassembles cleanly
	fw.send_ump(w.data(), static_cast<uint16_t>(w.size()));
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK(capture_log()[0].payload == msg);
	}
}

// ============================================================================
// Tier 2 — F2: SysEx7 Malformed Drop & Count Edge Cases (T2-F2-01..05)
// ============================================================================

E2E_TEST(2, "T2-F2-01", "kComplete while active on same group drops partial, delivers kComplete, orphans trailing kEnd") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msgA = make_ci_payload(12, 0x0Cu);
	const std::vector<uint8_t> msgB = make_ci_payload(5, 0x0Du);
	const auto wA = e2e::pack_sysex7_words(0, msgA);
	const auto wB = e2e::pack_sysex7_words(0, msgB);

	fw.send_ump(&wA[0], 2);
	fw.send_ump(wB.data(), static_cast<uint16_t>(wB.size()));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK(capture_log()[0].payload == msgB);
	}

	// Trailing kEnd of msgA is orphaned (+2)
	fw.send_ump(&wA[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
}

E2E_TEST(2, "T2-F2-02", "Over-long drop mid-stream on kContinue (518B) + over-long non-CI (514B) on group 1") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg518 = make_ci_payload(518, 0x1Eu);
	const auto w518 = e2e::pack_sysex7_words(0, msg518);
	E2E_CHECK_EQ(w518.size(), 87u * 2u);

	// Packets 0..85 include packet 85 (kContinue bringing len 510 -> 516 > 512)
	fw.send_ump(w518.data(), static_cast<uint16_t>(86u * 2u));
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK(capture_log().empty());

	// Packet 86 (kEnd) finds freed slot -> orphaned (+2)
	fw.send_ump(&w518[86u * 2u], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 2u);

	// Over-long non-CI (514B = 86 pkts) on group 1 -> +1 drop, rx unchanged
	const std::vector<uint8_t> non514 = make_nonci_payload(514, 0x2Eu);
	const auto wNon = e2e::pack_sysex7_words(1, non514);
	fw.send_ump(wNon.data(), static_cast<uint16_t>(wNon.size()));
	fw.poll(2);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 3u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 0u);
}

E2E_TEST(2, "T2-F2-03", "Invalid cnt > 6 or form >= 4 on group with active partial frees that group's slot") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const auto w12 = e2e::pack_sysex7_words(2, make_ci_payload(12, 0x51u));

	// Start partial on group 2, then send invalid cnt=7 on group 2 -> drops (+1) and frees slot
	fw.send_ump(&w12[0], 2);
	const uint32_t badCnt[2] = {0x32070000u, 0u};
	fw.send_ump(badCnt, 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);

	// Subsequent kEnd is orphaned (+2)
	fw.send_ump(&w12[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 2u);

	// Repeat with invalid form=4 on group 2 -> (+3 on bad form, +4 on orphaned kEnd)
	fw.send_ump(&w12[0], 2);
	const uint32_t badForm[2] = {0x32410000u, 0u};
	fw.send_ump(badForm, 2);
	fw.send_ump(&w12[2], 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 4u);
	E2E_CHECK(capture_log().empty());
}

E2E_TEST(2, "T2-F2-04", "Malformed drops on group 1 do not disturb active partial reassembly on group 0") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg20 = make_ci_payload(20, 0x61u);
	const auto w0 = e2e::pack_sysex7_words(0, msg20);
	const auto wOver1 = e2e::pack_sysex7_words(1, make_ci_payload(516, 0x62u));

	fw.send_ump(&w0[0], 2);  // kStart on group 0

	// Malformed bursts on group 1: orphaned kContinue, cnt=9, and 516B overlong
	const uint32_t badG1[4] = {0x31267E7Fu, 0x0D000000u, 0x31090000u, 0x00000000u};
	fw.send_ump(badG1, 4);
	fw.send_ump(wOver1.data(), static_cast<uint16_t>(wOver1.size()));
	fw.poll(2);
	E2E_CHECK(fw.reassembly_drops() - drops0 >= 3u);
	E2E_CHECK(capture_log().empty());

	// Finish group 0
	fw.send_ump(&w0[2], static_cast<uint16_t>(w0.size() - 2u));
	fw.poll(2);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);
	E2E_CHECK_EQ(capture_log().size(), 1u);
	if (capture_log().size() == 1u) {
		E2E_CHECK_EQ(capture_log()[0].group, 0u);
		E2E_CHECK(capture_log()[0].payload == msg20);
	}
}

E2E_TEST(2, "T2-F2-05", "CiRxQueue overflow (>64 unpolled MT 0x3 packets) counted without corrupting queued packets") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t rx0 = midici_messages_received();
	E2E_CHECK_EQ(midici_rx_overflows(), 0u);
	E2E_CHECK_EQ(midici_rx_free_packets(), static_cast<uint16_t>(CI_RX_QUEUE_PACKETS));

	const uint32_t pkt[2] = {0x30057E7Fu, 0x0D700200u};
	for (uint16_t i = 0; i < CI_RX_QUEUE_PACKETS; ++i) {
		E2E_CHECK_EQ(midici_rx_push(pkt), 0);
	}
	E2E_CHECK_EQ(midici_rx_free_packets(), 0u);
	E2E_CHECK_EQ(midici_rx_overflows(), 0u);

	for (int i = 0; i < 3; ++i) {
		E2E_CHECK_EQ(midici_rx_push(pkt), -1);
	}
	E2E_CHECK_EQ(midici_rx_overflows(), 3u);

	fw.poll(2);
	E2E_CHECK_EQ(midici_messages_received() - rx0, static_cast<uint32_t>(CI_RX_QUEUE_PACKETS));
	E2E_CHECK_EQ(capture_log().size(), static_cast<size_t>(CI_RX_QUEUE_PACKETS));
	E2E_CHECK_EQ(midici_rx_free_packets(), static_cast<uint16_t>(CI_RX_QUEUE_PACKETS));
}

// ============================================================================
// Tier 2 — F3: packetizeSysEx7 & midici_send_sysex Edge Cases (T2-F3-01..05)
// ============================================================================

E2E_TEST(2, "T2-F3-01", "midici_packetize_sysex7 NULL/capacity guards and no OOB write") {
	const uint8_t payload[8] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x01u, 0x02u, 0x03u};
	uint32_t guarded[4][2] = {};
	for (auto& row : guarded) {
		row[0] = 0xDEADBEEFu;
		row[1] = 0xDEADBEEFu;
	}

	E2E_CHECK_EQ(midici_packetize_sysex7(0, payload, 6, nullptr, 1), 0u);
	E2E_CHECK_EQ(midici_packetize_sysex7(0, nullptr, 1, &guarded[1], 1), 0u);
	E2E_CHECK_EQ(midici_packetize_sysex7(0, nullptr, 0, &guarded[1], 0), 0u);
	E2E_CHECK_EQ(midici_packetize_sysex7(0, payload, 7, &guarded[1], 1), 0u);

	const std::vector<uint8_t> big(512, 0x11u);
	E2E_CHECK_EQ(midici_packetize_sysex7(0, big.data(), 512, &guarded[1], 85), 0u);

	// Sentinel words remained untouched
	for (const auto& row : guarded) {
		E2E_CHECK_EQ(row[0], 0xDEADBEEFu);
		E2E_CHECK_EQ(row[1], 0xDEADBEEFu);
	}
}

E2E_TEST(2, "T2-F3-02", "midici_packetize_sysex7 exact 6-byte boundary transitions (6, 7, 12, 13, 18, 19, 510, 511, 512)") {
	const struct {
		uint16_t len;
		uint16_t pkts;
		uint8_t lastCnt;
	} cases[] = {
	    {6u, 1u, 6u},   {7u, 2u, 1u},   {12u, 2u, 6u},
	    {13u, 3u, 1u},  {18u, 3u, 6u},  {19u, 4u, 1u},
	    {510u, 85u, 6u}, {511u, 86u, 1u}, {512u, 86u, 2u},
	};

	uint32_t out[90][2] = {};
	for (const auto& c : cases) {
		const std::vector<uint8_t> data = make_ci_payload(c.len, static_cast<uint8_t>(c.len & 0x7Fu));
		const uint16_t n = midici_packetize_sysex7(4, data.data(), c.len, out, 90);
		E2E_CHECK_EQ(n, c.pkts);
		if (n == c.pkts) {
			const uint8_t firstForm = static_cast<uint8_t>((out[0][0] >> 20) & 0xFu);
			const uint8_t lastForm = static_cast<uint8_t>((out[n - 1u][0] >> 20) & 0xFu);
			const uint8_t lastCnt = static_cast<uint8_t>((out[n - 1u][0] >> 16) & 0xFu);
			E2E_CHECK_EQ(firstForm, c.pkts == 1u ? 0u : 1u);
			E2E_CHECK_EQ(lastForm, c.pkts == 1u ? 0u : 3u);
			E2E_CHECK_EQ(lastCnt, c.lastCnt);
			if (n >= 3u) {
				E2E_CHECK_EQ((out[1][0] >> 20) & 0xFu, 2u);
			}
		}
	}
}

E2E_TEST(2, "T2-F3-03", "midici_send_sysex all-or-nothing CiTxQueue overflow protection") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	E2E_CHECK_EQ(midici_tx_free_packets(), static_cast<uint16_t>(CI_TX_QUEUE_PACKETS));
	E2E_CHECK_EQ(midici_tx_overflows(), 0u);
	E2E_CHECK_EQ(midici_send_sysex(0, nullptr, 1), -1);
	E2E_CHECK_EQ(midici_tx_overflows(), 0u);

	const std::vector<uint8_t> chunk120 = make_ci_payload(120, 0x31u);  // 20 packets each
	for (int i = 0; i < 6; ++i) {
		E2E_CHECK_EQ(fw.send_sysex(0, chunk120.data(), static_cast<uint16_t>(chunk120.size())), 0);
	}
	E2E_CHECK_EQ(midici_tx_pending(), 120u);
	E2E_CHECK_EQ(midici_tx_free_packets(), 8u);

	// 7th 120B (20 packets > 8 free) rejected atomically
	E2E_CHECK_EQ(fw.send_sysex(0, chunk120.data(), static_cast<uint16_t>(chunk120.size())), -1);
	E2E_CHECK_EQ(midici_tx_overflows(), 1u);
	E2E_CHECK_EQ(midici_tx_pending(), 120u);
	E2E_CHECK_EQ(midici_tx_free_packets(), 8u);

	// 1-packet message still fits in remaining 8 packets
	const uint8_t small[6] = {0xF0u, 0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0xF7u};
	E2E_CHECK_EQ(fw.send_sysex(0, small, sizeof(small)), 0);
	E2E_CHECK_EQ(midici_tx_pending(), 121u);
	E2E_CHECK_EQ(midici_tx_free_packets(), 7u);
}

E2E_TEST(2, "T2-F3-04", "CI_TX_DRAIN_MAX (4 packets/poll) pacing and UMP_TX_CONTROL_STREAM_HEADROOM preservation") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const std::vector<uint8_t> msg60 = make_ci_payload(60, 0x45u);  // 10 packets
	E2E_CHECK_EQ(fw.send_sysex(2, msg60.data(), static_cast<uint16_t>(msg60.size())), 0);
	E2E_CHECK_EQ(midici_tx_pending(), 10u);

	fw.poll_once();
	E2E_CHECK_EQ(midici_tx_pending(), 6u);
	fw.poll_once();
	E2E_CHECK_EQ(midici_tx_pending(), 2u);
	fw.poll_once();
	E2E_CHECK_EQ(midici_tx_pending(), 0u);

	std::vector<uint8_t> out;
	uint64_t drops = 0;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 2, out, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK(out == msg60);

	// Fill UMP_TX_CONTROL right up to UMP_TX_CONTROL_STREAM_HEADROOM (32 words free)
	fw.boot(true, 1);
	const uint32_t filler[2] = {0x31000000u, 0u};
	int pushed = 0;
	while (ump_tx_push_control_app(filler, 2) == 0) {
		++pushed;
	}
	E2E_CHECK_EQ(pushed, static_cast<int>((UMP_TX_CONTROL_WORDS - UMP_TX_CONTROL_STREAM_HEADROOM) / 2u));

	const uint8_t msg6[6] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x01u};
	E2E_CHECK_EQ(fw.send_sysex(0, msg6, sizeof(msg6)), 0);
	E2E_CHECK_EQ(midici_tx_drain(4), 0u);
	E2E_CHECK_EQ(midici_tx_pending(), 1u);

	// UMP Stream EP Discovery still succeeds into the reserved 32-word headroom
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	ump_rx_poll();

	// Drain TX control ring across polls and verify both EP Info and SysEx7 arrive cleanly
	fw.poll(16);
	const auto pkts = fw.drain_tx_packets();
	bool sawEpInfo = false;
	for (const auto& p : pkts) {
		if (m2s::umpType(p) == 0xFu && ((p.words[0] >> 16) & 0x3FFu) == 0x001u) sawEpInfo = true;
	}
	E2E_CHECK(sawEpInfo);
	std::vector<uint8_t> decoded;
	E2E_CHECK(e2e::reassemble_sysex7(pkts, 0, decoded, &drops));
	E2E_CHECK(decoded == std::vector<uint8_t>(msg6, msg6 + sizeof(msg6)));
}

E2E_TEST(2, "T2-F3-05", "midici_send_sysex asymmetric F0/F7 stripping and 0-byte NULL handling") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const uint8_t onlyF0[1] = {0xF0u};
	const uint8_t onlyF7[1] = {0xF7u};
	const uint8_t leadF0[2] = {0xF0u, 0x11u};
	const uint8_t trailF7[2] = {0x11u, 0xF7u};

	const struct {
		const uint8_t* ptr;
		uint16_t len;
		std::vector<uint8_t> expected;
	} cases[] = {
	    {onlyF0, 1u, {}},
	    {onlyF7, 1u, {}},
	    {leadF0, 2u, {0x11u}},
	    {trailF7, 2u, {0x11u}},
	    {nullptr, 0u, {}},
	};

	for (const auto& c : cases) {
		E2E_CHECK_EQ(fw.send_sysex(0, c.ptr, c.len), 0);
		fw.poll(2);
		std::vector<uint8_t> out;
		uint64_t drops = 0;
		E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, out, &drops));
		E2E_CHECK_EQ(drops, 0u);
		E2E_CHECK(out == c.expected);
	}
}

// ============================================================================
// Tier 2 — F4: SysEx7 Round-Trip Edge Cases (T2-F4-01..05)
// ============================================================================

E2E_TEST(2, "T2-F4-01", "Cross-implementation interop: midici_packetize_sysex7 <-> SysEx7Assembler and packetizeSysEx7 <-> midici_poll") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const size_t lengths[] = {0u, 1u, 5u, 6u, 7u, 13u, 512u};
	uint32_t rawPkts[90][2] = {};
	for (size_t len : lengths) {
		const std::vector<uint8_t> input =
		    (len >= 3u) ? make_ci_payload(len, static_cast<uint8_t>(len + 0x10u))
		                : make_nonci_payload(len, 0x22u);

		// Path A: firmware packetizer -> m2s::ci::SysEx7Assembler
		const uint16_t np = midici_packetize_sysex7(
		    4, input.empty() ? nullptr : input.data(), static_cast<uint16_t>(input.size()), rawPkts, 90);
		E2E_CHECK(np >= 1u);
		std::vector<m2s::UmpPacket> umpPkts(np);
		for (uint16_t i = 0; i < np; ++i) {
			umpPkts[i].words[0] = rawPkts[i][0];
			umpPkts[i].words[1] = rawPkts[i][1];
			umpPkts[i].wordCount = 2;
		}
		std::vector<uint8_t> outA;
		uint64_t drops = 0;
		E2E_CHECK(e2e::reassemble_sysex7(umpPkts, 4, outA, &drops));
		E2E_CHECK_EQ(drops, 0u);
		E2E_CHECK(outA == input);

		// Path B: MIDI2Sound packetizer -> firmware midici_poll
		if (len >= 5u) {
			capture_log().clear();
			const auto wordsB = e2e::pack_sysex7_words(4, input);
			fw.send_ump(wordsB.data(), static_cast<uint16_t>(wordsB.size()));
			fw.poll(4);
			E2E_CHECK_EQ(capture_log().size(), 1u);
			if (capture_log().size() == 1u) {
				E2E_CHECK(capture_log()[0].payload == input);
			}
		}
	}
}

E2E_TEST(2, "T2-F4-02", "Orphaned kContinue mid-stream on group 1 does not corrupt active group 0 and group 2 round-trips") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> msg0 = make_ci_payload(20, 0x05u);
	const std::vector<uint8_t> msg2 = make_ci_payload(15, 0x15u);
	const auto w0 = e2e::pack_sysex7_words(0, msg0);
	const auto w2 = e2e::pack_sysex7_words(2, msg2);

	fw.send_ump(&w0[0], 2);
	fw.send_ump(&w2[0], 2);
	fw.poll(1);

	const uint32_t orphanG1[2] = {0x31267E7Fu, 0x0D700200u};
	fw.send_ump(orphanG1, 2);
	fw.poll(1);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);

	fw.send_ump(&w0[2], static_cast<uint16_t>(w0.size() - 2u));
	fw.send_ump(&w2[2], static_cast<uint16_t>(w2.size() - 2u));
	fw.poll(2);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 1u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 2u);
	E2E_CHECK_EQ(capture_log().size(), 2u);
	if (capture_log().size() == 2u) {
		E2E_CHECK(capture_log()[0].payload == msg0);
		E2E_CHECK(capture_log()[1].payload == msg2);
	}
}

E2E_TEST(2, "T2-F4-03", "Multi-packet Responder Discovery Inquiry -> Discovery Reply round-trip via EP1") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);  // Default Responder handler active

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> disc = make_discovery_inquiry(0x01234567u, 512u);
	const auto words = e2e::pack_sysex7_words(0, disc);
	fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
	fw.poll(12);

	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 1u);

	std::vector<uint8_t> reply;
	uint64_t drops = 0;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, reply, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK_EQ(reply.size(), ms::DiscoveryReplyByteCount);

	ms::ParsedMessage parsed{};
	E2E_CHECK(ms::Parse(reply.data(), reply.size(), parsed) == ms::ParseStatus::Ok);
	E2E_CHECK(parsed.Type == ms::MessageType::DiscoveryReply);
	E2E_CHECK_EQ(parsed.VersionFormat, 0x02u);
	E2E_CHECK_EQ(parsed.DestinationMuid, 0x01234567u);
	if (reply.size() >= 31u) {
		E2E_CHECK_EQ(reply[13], 0x00u);
		E2E_CHECK_EQ(reply[14], 0x20u);
		E2E_CHECK_EQ(reply[15], 0x29u);
		E2E_CHECK_EQ(ms::ReadFourteenBitValue(&reply[16]), 0x0051u);
		E2E_CHECK_EQ(ms::ReadMuid(&reply[25]), 512u);
	}
}

E2E_TEST(2, "T2-F4-04", "512B TX multi-USB-frame drain (86 packets / 11 EP1 IN frames) and alt 1->0->1 recovery") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const std::vector<uint8_t> msg512 = make_ci_payload(512, 0x77u);
	E2E_CHECK_EQ(fw.send_sysex(0, msg512.data(), static_cast<uint16_t>(msg512.size())), 0);
	fw.poll(25);

	std::vector<uint8_t> out1;
	uint64_t drops = 0;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, out1, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK(out1 == msg512);

	// Queue another 512B message, switch alt 1 -> 0 -> 1, verify pending discarded cleanly
	E2E_CHECK_EQ(fw.send_sysex(0, msg512.data(), static_cast<uint16_t>(msg512.size())), 0);
	E2E_CHECK(midici_tx_pending() > 0u);
	fw.set_alt(0);
	fw.poll(2);
	fw.set_alt(1);
	fw.poll(2);
	E2E_CHECK_EQ(midici_tx_pending(), 0u);
	E2E_CHECK_EQ(e2e_tx_count(), 0u);

	// Subsequent 512B TX succeeds intact
	const std::vector<uint8_t> msg512b = make_ci_payload(512, 0x3Bu);
	E2E_CHECK_EQ(fw.send_sysex(0, msg512b.data(), static_cast<uint16_t>(msg512b.size())), 0);
	fw.poll(25);
	std::vector<uint8_t> out2;
	E2E_CHECK(e2e::reassemble_sysex7(fw.drain_tx_packets(), 0, out2, &drops));
	E2E_CHECK_EQ(drops, 0u);
	E2E_CHECK(out2 == msg512b);
}

E2E_TEST(2, "T2-F4-05", "Burst of 4 interleaved 512-byte MIDI-CI messages (344 MT 0x3 packets) through e2e_host_send_ump") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_capture();

	const uint32_t drops0 = fw.reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const uint8_t groups[4] = {0u, 2u, 3u, 15u};
	std::vector<uint8_t> msgs[4];
	std::vector<uint32_t> pkts[4];
	for (int i = 0; i < 4; ++i) {
		msgs[i] = make_ci_payload(512, static_cast<uint8_t>(0x10u + i * 0x17u));
		pkts[i] = e2e::pack_sysex7_words(groups[i], msgs[i]);
		E2E_CHECK_EQ(pkts[i].size(), 86u * 2u);
	}

	std::vector<uint32_t> interleaved;
	interleaved.reserve(4u * 86u * 2u);
	for (size_t p = 0; p < 86u; ++p) {
		for (int g = 0; g < 4; ++g) {
			interleaved.push_back(pkts[g][2u * p]);
			interleaved.push_back(pkts[g][2u * p + 1u]);
		}
	}

	fw.send_ump(interleaved.data(), static_cast<uint16_t>(interleaved.size()));
	fw.poll(4);

	E2E_CHECK_EQ(midici_rx_overflows(), 0u);
	E2E_CHECK_EQ(fw.reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, 4u);
	E2E_CHECK_EQ(capture_log().size(), 4u);
	if (capture_log().size() == 4u) {
		for (int g = 0; g < 4; ++g) {
			E2E_CHECK_EQ(capture_log()[static_cast<size_t>(g)].group, groups[g]);
			E2E_CHECK(capture_log()[static_cast<size_t>(g)].payload == msgs[g]);
		}
	}
}

#endif  // E2E_HAS_M1
