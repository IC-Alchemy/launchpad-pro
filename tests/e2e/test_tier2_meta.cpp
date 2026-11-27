#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"

extern "C" {
#include "app.h"
#include "instrument/settings.h"
#include "midici/midici.h"
#include "pads/pad_velocity.h"
#include "platform/platform.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
}

#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Tier 2 — F0: Repo & Toolchain Baseline Edge Cases (T2-F0-01..05)
// ============================================================================

E2E_TEST(2, "T2-F0-01", "scales boundaries: invalid pads, pitch > 127, and absolute_root") {
	struct scale_ctx ctx = {0, 0, 3, LAYOUT_INKEY_4THS, 0, TUNING_DEFAULT_ID};
	const uint8_t badPads[] = {0, 10, 19, 89, 91, 255};
	for (uint8_t p : badPads) {
		E2E_CHECK_EQ(scale_pad_pitch(&ctx, p), -1);
	}

	struct scale_ctx hi = {11, 0, 7, LAYOUT_CHROM_GUITAR, 0, TUNING_DEFAULT_ID};
	E2E_CHECK_EQ(scale_pad_pitch(&hi, 88), -1);  // exceeds MIDI 127
	E2E_CHECK_EQ(scale_pitch_role(&hi, -1), ROLE_OFF);

	struct scale_ctx abs0 = {0, 0, 3, LAYOUT_INKEY_4THS, 1, TUNING_DEFAULT_ID};
	struct scale_ctx abs2 = {2, 0, 3, LAYOUT_INKEY_4THS, 1, TUNING_DEFAULT_ID};
	E2E_CHECK(scale_pad_pitch(&abs0, 11) != scale_pad_pitch(&abs2, 11));
}

E2E_TEST(2, "T2-F0-02", "pad_velocity and pad_pressure saturation, zero-div guard, and curves") {
	// Lower boundary (threshold + 80 = 100) -> 1; top boundary -> 0xFFFF
	E2E_CHECK_EQ(pad_velocity16(100, 20, 1023), 1u);
	E2E_CHECK(pad_velocity16(101, 20, 1023) >= 1u);
	E2E_CHECK_EQ(pad_velocity16(1023, 20, 1023), 0xFFFFu);

	// Degenerate full_scale <= lo must not divide by zero
	E2E_CHECK_EQ(pad_velocity16(200, 20, 50), 0xFFFFu);
	E2E_CHECK_EQ(pad_velocity16_raw12(800, 20, 50), 0xFFFFu);

	// Curve floor >= 1 even at minimal input
	E2E_CHECK(pad_curve(1, CURVE_HARD) >= 1u);
	E2E_CHECK(pad_curve(1, CURVE_SOFT) >= 1u);
	E2E_CHECK_EQ(pad_curve(0xFFFFu, CURVE_LINEAR), 0xFFFFu);

	// Pressure bounds
	E2E_CHECK_EQ(pad_pressure12(0, THRESH_HIGH), 0u);
	E2E_CHECK_EQ(pad_pressure12(4095, THRESH_LOW), 4095u);
}

E2E_TEST(2, "T2-F0-03", "ump_stream protocol switch ordering (behind_notes) and invalid requests") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	E2E_CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI2);

	// Queue a Standalone Note On before switching to MIDI 1.0 protocol
	const uint32_t noteOn[2] = {0x40903C00u, 0x80000000u};
	E2E_CHECK_EQ(ump_tx_push(UMP_TX_NOTE, noteOn, 2), 0);

	const uint32_t toMidi1[4] = {0xF0050100u, 0u, 0u, 0u};
	fw.send_ump(toMidi1, 4);
	fw.poll(4);
	E2E_CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI1);

	const std::vector<uint32_t> words = fw.drain_ump();
	// Note On (2 words) must precede ST_CONFIG_NOTIFY (4 words)
	E2E_CHECK(words.size() >= 6u);
	if (words.size() >= 6u) {
		E2E_CHECK_EQ(words[0], noteOn[0]);
		E2E_CHECK_EQ(words[2], 0xF0060100u);
	}

	// Invalid protocol 0x03 keeps MIDI1; nonexistent FB 0x02 produces 0 words
	const uint32_t badProto[4] = {0xF0050300u, 0u, 0u, 0u};
	const uint32_t badFb[4] = {0xF0100203u, 0u, 0u, 0u};
	fw.send_ump(badProto, 4);
	fw.send_ump(badFb, 4);
	fw.poll(4);
	E2E_CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI1);
	const std::vector<uint32_t> words2 = fw.drain_ump();
	E2E_CHECK_EQ(words2.size(), 4u);
	if (words2.size() == 4u) {
		E2E_CHECK_EQ(words2[0], 0xF0060100u);
	}
}

E2E_TEST(2, "T2-F0-04", "platform_product_instance_id boundary lengths and ST_PRODUCT_ID framing") {
	auto queryProductId = [](const char* serial) {
		e2e::FirmwareNode fw;
		fw.set_serial(serial);
		fw.boot(true, 1);
		const uint32_t disc[4] = {0xF0000101u, 0x00000008u, 0u, 0u};
		fw.send_ump(disc, 4);
		fw.poll(4);
		const std::vector<uint32_t> w = fw.drain_ump();
		std::string out;
		for (size_t pos = 0; pos + 3 < w.size(); pos += 4) {
			if (((w[pos] >> 16) & 0x3FFu) == 0x004u) {
				const uint8_t b0 = static_cast<uint8_t>((w[pos] >> 8) & 0xFFu);
				const uint8_t b1 = static_cast<uint8_t>(w[pos] & 0xFFu);
				if (b0) out.push_back(static_cast<char>(b0));
				if (b1) out.push_back(static_cast<char>(b1));
				for (size_t k = 1; k < 4; ++k) {
					for (int sh = 24; sh >= 0; sh -= 8) {
						const char c = static_cast<char>((w[pos + k] >> sh) & 0xFFu);
						if (c) out.push_back(c);
					}
				}
			}
		}
		return out;
	};

	E2E_CHECK(queryProductId("") == "");
	E2E_CHECK(queryProductId("A") == "A");
	E2E_CHECK(queryProductId("0123456789ABCDEFGHIJKLMN") == "0123456789ABCDEFGHIJKLMN");
	// Overlong (>24 chars) safely clamped to 24 characters without buffer overrun
	E2E_CHECK(queryProductId("0123456789ABCDEFGHIJKLMN_EXTRA") == "0123456789ABCDEFGHIJKLMN");
}

E2E_TEST(2, "T2-F0-05", "CiAgent edge cases: empty sources, resetTransport, and JSON fallbacks") {
	using namespace m2s::ci;

	CiAgent noSrc(
	    CiIdentity{}, [](uint16_t, const uint32_t*, size_t) { return true; },
	    [](const m2s::UmpPacket&) {}, [] { return std::vector<uint16_t>{}; }, 0x0101010);

	std::wstring err;
	E2E_CHECK(!noSrc.discover(&err));
	E2E_CHECK(!err.empty());
	noSrc.resetTransport();
	E2E_CHECK(noSrc.snapshot().peers.empty());

	E2E_CHECK(ciJsonString(R"({"resource":"unterminated)", "resource") == "unterminated");
	E2E_CHECK(ciJsonString(R"({"other":"x"})", "resource").empty());
	E2E_CHECK_EQ(ciJsonInt(R"({"status":"not_an_int"})", "status", -1), -1);
}

// ============================================================================
// Tier 2 — F16: E2E Harness & Stub Edge Cases (T2-F16-01..05)
// ============================================================================

E2E_TEST(2, "T2-F16-01", "EP1_OUT_Callback partial-word drop, >64B clamp, and NAK->VALID re-arm") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// Inject 7 bytes: 1 full 32-bit LE word for ST_EP_DISCOVERY (0xF0000101u) + 3 trailing partial bytes
	const uint8_t odd7[7] = {0x01, 0x01, 0x00, 0xF0, 0xAA, 0xBB, 0xCC};
	e2e_host_send_ep1_raw(odd7, sizeof(odd7));
	E2E_CHECK_EQ(GetEPRxStatus(1), static_cast<uint16_t>(EP_RX_NAK));
	__wrap_midi_parsers_poll();
	E2E_CHECK_EQ(GetEPRxStatus(1), static_cast<uint16_t>(EP_RX_VALID));

	// 1 partial MT 0xF word buffered cleanly without OOB read; send remaining 3 words to complete
	const uint32_t tail3[3] = {0x00000001u, 0u, 0u};
	fw.send_ump(tail3, 3);
	fw.poll(2);
	E2E_CHECK(!fw.drain_ump().empty());

	// >64 byte USB read is safely clamped to 64 bytes without stack/buffer overflow
	uint8_t big[80];
	std::memset(big, 0, sizeof(big));
	e2e_host_send_ep1_raw(big, sizeof(big));
	fw.poll(2);

	// NULL guard on e2e_host_send_ep1_raw and e2e_host_send_ump
	const uint16_t rxFreeBefore = ump_rx_free_words();
	e2e_host_send_ep1_raw(nullptr, 16);
	E2E_CHECK_EQ(ump_rx_free_words(), rxFreeBefore);
	fw.send_ump(nullptr, 4);

	// Pre-drain check: pre-filling CiRxQueue to 7 free packets (<8) + multi-word send_ump must not overflow
	fw.boot(true, 1);
	const uint32_t g2Empty[2] = {0x32000000u, 0u};
	for (int i = 0; i < 57; ++i) {
		E2E_CHECK_EQ(midici_rx_push(g2Empty), 0);
	}
	E2E_CHECK_EQ(midici_rx_free_packets(), 7u);
	std::vector<uint32_t> burst(16, 0u);
	for (size_t i = 0; i < 8; ++i) {
		burst[2 * i] = 0x32000000u;
		burst[2 * i + 1] = 0u;
	}
	fw.send_ump(burst.data(), static_cast<uint16_t>(burst.size()));
	E2E_CHECK_EQ(midici_rx_overflows(), 0u);
}

E2E_TEST(2, "T2-F16-02", "Flash OOB bounds and LED out-of-range index fail silently under ASan") {
	e2e_stubs_reset();
	const uint8_t payload[16] = {1, 2, 3, 4};
	uint8_t out[16] = {0};

	// Write/read crossing USER_AREA_SIZE (1024) must not write past e2e_flash[1024]
	hal_write_flash(1020, payload, sizeof(payload));
	E2E_CHECK_EQ(e2e_flash[1020], 0xFFu);
	hal_read_flash(1024, out, sizeof(out));
	E2E_CHECK_EQ(out[0], 0u);

	// Out-of-range pad LED index (>= 100) must not crash or corrupt state
	hal_plot_led(TYPEPAD, 200, 63, 63, 63);
	hal_plot_led(TYPEPAD, 99, 10, 20, 30);
	E2E_CHECK_EQ(e2e_leds[99][0], 10u);
	E2E_CHECK_EQ(e2e_leds[99][1], 20u);
	E2E_CHECK_EQ(e2e_leds[99][2], 30u);
}

E2E_TEST(2, "T2-F16-03", "USB EP1 IN TX backpressure respects EP_TX_VALID until host consumes packet") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	// USB_SIL_Write to OUT endpoint (0x01) or with NULL buf must not pollute EP1 IN TX FIFO
	uint8_t outDummy[4] = {0x11, 0x22, 0x33, 0x44};
	USB_SIL_Write(0x01, outDummy, sizeof(outDummy));
	USB_SIL_Write(0x81, nullptr, 4);
	E2E_CHECK_EQ(e2e_tx_count(), 0u);

	const uint32_t pkt[2] = {0x30037E7Fu, 0x0D010203u};
	E2E_CHECK_EQ(ump_tx_push(UMP_TX_CONTROL, pkt, 2), 0);

	// Simulate EP1 IN still busy (EP_TX_VALID)
	SetEPTxStatus(1, EP_TX_VALID);
	__wrap_usbmidi_poll();
	E2E_CHECK_EQ(e2e_tx_count(), 0u);

	// Once host reads packet (EP_TX_NAK via e2e_host_consume_in), poll transmits queued words
	e2e_host_consume_in();
	__wrap_usbmidi_poll();
	E2E_CHECK_EQ(e2e_tx_count(), 2u);
}

E2E_TEST(2, "T2-F16-04", "WirePair::settle(max_cycles) prevents infinite loop under active traffic") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0666666u);

	// With idle queues, settle(5) completes immediately returning true
	E2E_CHECK(wire.settle(5));

	// Multi-packet ST_EP_DISCOVERY (filter=0x1F -> 6 messages > 64B):
	// test both max_cycles = 0 AND genuine active multi-cycle max_cycles = 1 cap hit before settle(16) finishes
	const uint32_t disc[4] = {0xF0000101u, 0x0000001Fu, 0u, 0u};
	wire.inject_ump_to_fw(disc, 4);
	E2E_CHECK(!wire.settle(0));
	E2E_CHECK(!wire.settle(1));
	E2E_CHECK(wire.settle(16));
}

E2E_TEST(2, "T2-F16-05", "E2E_CHECK_EQ single-evaluation guarantee and rapid alt 1<->0<->1 resets") {
	int evalA = 0;
	int evalB = 0;
	auto incA = [&]() { return ++evalA; };
	auto incB = [&]() { return ++evalB; };
	E2E_CHECK_EQ(incA(), incB());
	E2E_CHECK_EQ(evalA, 1);
	E2E_CHECK_EQ(evalB, 1);

	e2e::FirmwareNode fw;
	for (int i = 0; i < 10; ++i) {
		fw.boot(true, 1);
		fw.set_alt(0);
		fw.poll(2);
		fw.set_alt(1);
		fw.poll(2);
	}
	E2E_CHECK_EQ(midi2_usb_alt(), 1u);
	E2E_CHECK_EQ(midi2_active(), 1u);
}
