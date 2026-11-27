#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "common/ump_text.h"

extern "C" {
#include "app.h"
#include "instrument/settings.h"
#include "instrument/ui.h"
#include "midici/ci_identity.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "pads/pad_velocity.h"
#include "platform/pads.h"
#include "platform/platform.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
}

#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <vector>

// ============================================================================
// Tier 1 — F0: Repo & Toolchain Baseline (T1-F0-01..05)
// ============================================================================

E2E_TEST(1, "T1-F0-01", "Mixed C99/C++20 extern C ABI, identity/PE resources, and app_init baseline") {
	E2E_CHECK_EQ(CI_MANUFACTURER_0, 0x00u);
	E2E_CHECK_EQ(CI_MANUFACTURER_1, 0x20u);
	E2E_CHECK_EQ(CI_MANUFACTURER_2, 0x29u);
	E2E_CHECK_EQ(CI_FAMILY, 0x0051u);
	E2E_CHECK_EQ(CI_MODEL, 0x0000u);
	E2E_CHECK_EQ(CI_UMP_IDENTITY_WORD1, 0x00002029u);
	E2E_CHECK_EQ(CI_UMP_IDENTITY_WORD2, 0x51000000u);
	E2E_CHECK_EQ(CI_UMP_IDENTITY_WORD3, 0x00010000u);

	const uint16_t rlLen = ci_pe_body_len(CI_PE_RESOURCE_LIST, nullptr, "TEST0001");
	E2E_CHECK(rlLen > 20u);
	char devBuf[512] = {};
	const uint16_t devLen = ci_pe_body_len(CI_PE_DEVICE_INFO, nullptr, "LPP-BASE-01");
	E2E_CHECK(devLen > 0u && devLen < sizeof(devBuf));
	struct ci_json_out out {};
	ci_json_out_init(&out, devBuf, 0, sizeof(devBuf) - 1u);
	ci_pe_render(CI_PE_DEVICE_INFO, nullptr, "LPP-BASE-01", &out);
	devBuf[devLen] = '\0';
	E2E_CHECK(m2s::ci::ciJsonString(devBuf, "manufacturer") == "Novation");
	E2E_CHECK(m2s::ci::ciJsonString(devBuf, "family") == "Launchpad");
	E2E_CHECK(m2s::ci::ciJsonString(devBuf, "model") == "Launchpad Pro MIDI 2.0");
	E2E_CHECK(m2s::ci::ciJsonString(devBuf, "version") == "0.1.0.0");
	E2E_CHECK(m2s::ci::ciJsonString(devBuf, "serialNumber") == "LPP-BASE-01");

	e2e_stubs_reset();
	app_init(e2e_adc);
	E2E_CHECK_EQ(PadCalibration[0], PAD_LIB_THRESHOLD);
	E2E_CHECK_EQ(settings_valid(ui_settings()), 1u);
}

E2E_TEST(1, "T1-F0-02", "Baseline scales module SCALES table, pitch mapping, and role colors") {
	E2E_CHECK_EQ(SCALE_COUNT, 61);
	E2E_CHECK(std::strcmp(SCALES[0].name, "Major") == 0);
	E2E_CHECK_EQ(SCALES[0].n, 7u);
	E2E_CHECK(std::strcmp(SCALES[11].name, "Minor Pentatonic") == 0);
	E2E_CHECK_EQ(SCALES[11].n, 5u);

	struct scale_ctx ctx = {0, 0, 3, LAYOUT_INKEY_4THS, 0, TUNING_DEFAULT_ID};  // C Major, octave 3
	E2E_CHECK_EQ(scale_pad_pitch(&ctx, 11), 48);                               // C3
	E2E_CHECK_EQ(scale_pad_pitch(&ctx, 18), 60);                               // C4
	E2E_CHECK_EQ(scale_pitch_role(&ctx, 48), ROLE_ROOT);
	E2E_CHECK_EQ(scale_pitch_role(&ctx, 50), ROLE_SCALE);
	E2E_CHECK_EQ(scale_pitch_role(&ctx, 52), ROLE_THIRD);
	E2E_CHECK_EQ(scale_pitch_role(&ctx, 55), ROLE_FIFTH);
	E2E_CHECK_EQ(scale_pitch_role(&ctx, 49), ROLE_OUT);
	E2E_CHECK_EQ(scale_pitch_role(&ctx, -1), ROLE_OFF);
}

E2E_TEST(1, "T1-F0-03", "Baseline pad_velocity slot mapping, velocity16, and pressure scaling") {
	E2E_CHECK_EQ(pad_to_adcmax_slot(81), 0u);
	E2E_CHECK_EQ(pad_to_adcmax_slot(88), 7u);
	E2E_CHECK_EQ(pad_to_adcmax_slot(11), 56u);
	E2E_CHECK_EQ(pad_to_adcmax_slot(18), 63u);
	E2E_CHECK_EQ(pad_to_adcmax_slot(91), PAD_NO_SLOT);

	E2E_CHECK_EQ(pad_velocity16(0, 20, 1023), 1u);
	E2E_CHECK_EQ(pad_velocity16(1023, 20, 1023), 0xFFFFu);
	E2E_CHECK_EQ(pad_velocity16_raw12(4u * 500u, 20, 1023), pad_velocity16(500u, 20, 1023));

	E2E_CHECK_EQ(pad_pressure12(PRESSURE_START_MED, THRESH_MED), 0u);
	E2E_CHECK_EQ(pad_pressure12(PRESSURE_FULL_RAW, THRESH_MED), 4095u);
	E2E_CHECK_EQ(pad_pressure32(4095u), 0xFFFFFFFFu);
}

E2E_TEST(1, "T1-F0-04", "Baseline ump_stream EP Discovery and platform_product_instance_id") {
	e2e::FirmwareNode fw;
	fw.set_serial("LPP-BASELINE-01");
	fw.boot(true, 1);

	const uint32_t disc[4] = {0xF0000101u, 0x0000001Fu, 0u, 0u};
	fw.send_ump(disc, 4);
	fw.poll(4);

	const std::vector<uint32_t> words = fw.drain_ump();
	bool gotInfo = false, gotId = false, gotConfig = false;
	std::string name, prodId;

	for (size_t pos = 0; pos < words.size();) {
		const uint8_t wc = ump_word_count(words[pos]);
		E2E_CHECK(wc > 0 && pos + wc <= words.size());
		if (wc == 0 || pos + wc > words.size()) break;
		if ((words[pos] >> 28) == 0xFu && wc == 4) {
			const uint16_t st = static_cast<uint16_t>((words[pos] >> 16) & 0x3FFu);
			if (st == 0x001u) {
				gotInfo = (words[pos] == 0xF0010101u && words[pos + 1] == 0x82000300u);
			} else if (st == 0x002u) {
				gotId = (words[pos + 1] == CI_UMP_IDENTITY_WORD1 &&
				         words[pos + 2] == CI_UMP_IDENTITY_WORD2 &&
				         words[pos + 3] == CI_UMP_IDENTITY_WORD3);
			} else if (st == 0x003u || st == 0x004u) {
				std::string* dst = (st == 0x003u) ? &name : &prodId;
				const uint8_t c0 = static_cast<uint8_t>((words[pos] >> 8) & 0xFFu);
				const uint8_t c1 = static_cast<uint8_t>(words[pos] & 0xFFu);
				if (c0) dst->push_back(static_cast<char>(c0));
				if (c1) dst->push_back(static_cast<char>(c1));
				for (size_t w = 1; w < 4; ++w) {
					for (int sh = 24; sh >= 0; sh -= 8) {
						const char c = static_cast<char>((words[pos + w] >> sh) & 0xFFu);
						if (c) dst->push_back(c);
					}
				}
			} else if (st == 0x006u) {
				gotConfig = (words[pos] == 0xF0060200u);
			}
		}
		pos += wc;
	}

	E2E_CHECK(gotInfo);
	E2E_CHECK(gotId);
	E2E_CHECK(gotConfig);
	E2E_CHECK(name == "Launchpad Pro MIDI 2.0");
	E2E_CHECK(prodId == "LPP-BASELINE-01");
}

E2E_TEST(1, "T1-F0-05", "Baseline MIDI2Sound CiAgent self-pair PE loopback and ump_text decode") {
	using namespace m2s;
	using namespace m2s::ci;

	std::deque<UmpPacket> toA, toB;
	auto pushWords = [](std::deque<UmpPacket>& dst, const uint32_t* w, size_t n, uint16_t src) {
		for (size_t i = 0; i + 1 < n; i += 2) {
			UmpPacket p{};
			p.words[0] = w[i];
			p.words[1] = w[i + 1];
			p.wordCount = 2;
			p.source = src;
			dst.push_back(p);
		}
	};

	CiIdentity ia, ib;
	ib.productInstanceId = "M2S-BASE-01";
	CiAgent a(
	    ia,
	    [&](uint16_t s, const uint32_t* w, size_t n) {
		    if (s != 20) return false;
		    pushWords(toB, w, n, 10);
		    return true;
	    },
	    [](const UmpPacket&) {}, [] { return std::vector<uint16_t>{20}; }, 0x0111111);
	CiAgent b(
	    ib,
	    [&](uint16_t s, const uint32_t* w, size_t n) {
		    if (s != 10) return false;
		    pushWords(toA, w, n, 20);
		    return true;
	    },
	    [](const UmpPacket&) {}, [] { return std::vector<uint16_t>{10}; }, 0x0222222);

	std::wstring err;
	E2E_CHECK(a.discover(&err));
	for (int r = 0; r < 50; ++r) {
		while (!toA.empty()) {
			a.receive(toA.front());
			toA.pop_front();
		}
		while (!toB.empty()) {
			b.receive(toB.front());
			toB.pop_front();
		}
		if (a.pump() + b.pump() == 0 && toA.empty() && toB.empty()) break;
	}

	const CiSnapshot sa = a.snapshot();
	E2E_CHECK_EQ(sa.peers.size(), 1u);
	if (sa.peers.size() == 1) {
		E2E_CHECK_EQ(sa.peers[0].resourceListStatus, 200);
		E2E_CHECK_EQ(sa.peers[0].deviceInfoStatus, 200);
		E2E_CHECK(sa.peers[0].productInstanceId == "M2S-BASE-01");
	}

	UmpPacket discPkt{};
	discPkt.words[0] = 0x30167E7Fu;
	discPkt.words[1] = 0x0D700201u;
	discPkt.wordCount = 2;
	E2E_CHECK(umpDescribe(discPkt).find("[MIDI-CI Discovery]") != std::string::npos);
}

// ============================================================================
// Tier 1 — F16: E2E Suite Meta-Checks (T1-F16-01..05)
// ============================================================================

E2E_TEST(1, "T1-F16-01", "Stub reset isolation clears LEDs, flash, serial, entropy, and TX FIFO") {
	e2e_set_serial("DIRTY-SERIAL");
	const uint32_t ent[2] = {0x11111111u, 0x22222222u};
	e2e_set_entropy(ent, 2);
	hal_plot_led(TYPEPAD, 11, 63, 0, 0);
	hal_plot_led(TYPESETUP, 0, 0, 63, 0);
	const uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
	hal_write_flash(0, data, sizeof(data));

	e2e_set_device_state(USB_STATE_CONFIGURED);
	usb_midi2_set_alt(1);
	__wrap_midi_parsers_poll();
	const uint32_t msg[2] = {0x30037E7Fu, 0x0D000000u};
	E2E_CHECK_EQ(ump_tx_push(UMP_TX_CONTROL, msg, 2), 0);
	__wrap_usbmidi_poll();
	E2E_CHECK(e2e_tx_count() > 0u);

	e2e_stubs_reset();

	E2E_CHECK_EQ(e2e_tx_count(), 0u);
	E2E_CHECK_EQ(e2e_flash_writes, 0);
	E2E_CHECK_EQ(e2e_flash[0], 0xFFu);
	E2E_CHECK_EQ(e2e_leds[11][0], 0u);
	E2E_CHECK_EQ(e2e_setup_led[1], 0u);

	char id[25] = {};
	platform_product_instance_id(id);
	E2E_CHECK(std::strcmp(id, "TEST0001") == 0);
	E2E_CHECK_EQ(bDeviceState, 0u);
}

E2E_TEST(1, "T1-F16-02", "Multi-packet EP1 IN TX FIFO captures full burst without overwrite") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// Push 12 x 64-bit UMP packets (96 bytes -> requires two 64-byte EP1 IN USB packets)
	for (uint32_t i = 0; i < 12; ++i) {
		const uint32_t pkt[2] = {0x30010000u | (i + 1u), 0xA0000000u | i};
		E2E_CHECK_EQ(ump_tx_push(UMP_TX_CONTROL, pkt, 2), 0);
	}

	fw.poll(4);  // polls __wrap_usbmidi_poll() and simulates host IN ACK across packets
	const std::vector<uint32_t> rx = fw.drain_ump();
	E2E_CHECK_EQ(rx.size(), 24u);
	for (uint32_t i = 0; i < 12 && (2u * i + 1u) < rx.size(); ++i) {
		E2E_CHECK_EQ(rx[2u * i], 0x30010000u | (i + 1u));
		E2E_CHECK_EQ(rx[2u * i + 1u], 0xA0000000u | i);
	}
}

E2E_TEST(1, "T1-F16-03", "VirtualClock deterministically advances 1 kHz app_timer_event and ui_poll") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	const uint32_t t0 = fw.clock().now_ms();

	// Advance 3000 ms deterministically (relative to monotonic process clock t0)
	fw.advance_ms(3000);
	E2E_CHECK_EQ(fw.clock().now_ms() - t0, 3000u);
	fw.poll(1);
	E2E_CHECK_EQ(e2e_flash_writes, 0);  // clean settings at defaults should not write flash
}

E2E_TEST(1, "T1-F16-04", "WirePair FirmwareNode <-> CiAgent bidirectional transport settles cleanly") {
	e2e::FirmwareNode fw;
	fw.set_serial("WIRE-01");
	fw.boot(true, 1);

	m2s::ci::CiIdentity hostId;
	hostId.productInstanceId = "HOST-01";
	e2e::WirePair wire(fw, hostId, 0x0555555u);

	// Verify settle() terminates cleanly even when non-MT3 stream traffic occurs on EP1 IN
	const uint32_t disc[4] = {0xF0000101u, 0x00000001u, 0u, 0u};
	fw.send_ump(disc, 4);
	const bool settled = wire.settle(32);
	E2E_CHECK(settled);
	E2E_CHECK_EQ(wire.agent().muid(), 0x0555555u);
}

E2E_TEST(1, "T1-F16-05", "Test registry uniqueness, gate macros, and Tier 1 meta completeness") {
	E2E_CHECK(E2E_HAS_M1 == 0 || E2E_HAS_M1 == 1);
	E2E_CHECK(E2E_HAS_M2 == 0 || E2E_HAS_M2 == 1);
	E2E_CHECK(E2E_HAS_M3 == 0 || E2E_HAS_M3 == 1);

	std::set<std::string> ids;
	int t1F0 = 0, t1F16 = 0, t2F0 = 0, t2F16 = 0;
	for (const auto& t : e2e::registry()) {
		const std::string id(t.id);
		E2E_CHECK(ids.insert(id).second);
		if (t.tier == 1 && id.rfind("T1-F0-", 0) == 0) ++t1F0;
		if (t.tier == 1 && id.rfind("T1-F16-", 0) == 0) ++t1F16;
		if (t.tier == 2 && id.rfind("T2-F0-", 0) == 0) ++t2F0;
		if (t.tier == 2 && id.rfind("T2-F16-", 0) == 0) ++t2F16;
	}
	E2E_CHECK_EQ(t1F0, 5);
	E2E_CHECK_EQ(t1F16, 5);
	E2E_CHECK_EQ(t2F0, 5);
	E2E_CHECK_EQ(t2F16, 5);
}
