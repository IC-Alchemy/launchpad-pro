#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump.h"

extern "C" {
#include "midici/ci_identity.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "midici/midici.h"
#include "midici/responder.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ms = m2s::ci::ms;

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
	if (payload && len > 0) {
		m.payload.assign(payload, payload + len);
	}
	spy_log().push_back(std::move(m));
}

void install_spy() {
	spy_log().clear();
	midici_set_handler(spy_handler);
}

std::vector<uint8_t> make_valid_discovery_v2(uint32_t srcMuid, uint32_t maxSysEx) {
	ms::DiscoveryReplyFields f{};
	f.SourceMuid = srcMuid;
	f.DestinationMuid = ms::MuidBroadcast;
	f.ManufacturerSysExId[0] = 0x7Du;
	f.DeviceFamily = 0x0001u;
	f.DeviceFamilyModelNumber = 0x0002u;
	f.SoftwareRevisionLevel[0] = 0x01u;
	f.CapabilityCategories = 0x08u;
	f.ReceivableMaximumSysExSize = maxSysEx;
	f.OutputPathId = 0x00u;
	f.MessageVersion = ms::MessageVersion12;
	std::vector<uint8_t> buf(ms::DiscoveryByteCount, 0u);
	buf.resize(ms::BuildDiscovery(f, buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_valid_endpoint_reply(uint32_t srcMuid, uint32_t dstMuid,
                                               const std::string& piid) {
	std::vector<uint8_t> buf(ms::EndpointReplyFixedByteCount + piid.size(), 0u);
	buf.resize(ms::BuildEndpointReply(srcMuid, dstMuid, ms::EndpointStatusProductInstanceId,
	                                  reinterpret_cast<const uint8_t*>(piid.data()),
	                                  static_cast<uint16_t>(piid.size()), buf.data(), buf.size()));
	return buf;
}

std::vector<uint8_t> make_valid_pe_get(uint32_t srcMuid, uint32_t dstMuid, uint8_t reqId,
                                       const std::string& hdr, const std::string& body = "") {
	ms::PropertyExchangeMessageFields f{};
	f.Type = ms::MessageType::PropertyGetDataInquiry;
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

void run_fuzz_step(const uint8_t* data, size_t size, uint8_t mode) {
	switch (mode & 0x03u) {
		case 0: {
			uint32_t words[32] = {};
			const size_t nbytes = std::min<size_t>(size, sizeof(words));
			size_t nwords = 0;
			for (size_t i = 0; i + 7u < nbytes && nwords + 1u < 32u; i += 8u) {
				words[nwords] = static_cast<uint32_t>(data[i]) |
				                (static_cast<uint32_t>(data[i + 1u]) << 8) |
				                (static_cast<uint32_t>(data[i + 2u]) << 16) |
				                (static_cast<uint32_t>(data[i + 3u]) << 24);
				words[nwords] = (words[nwords] & 0x0FFFFFFFu) | 0x30000000u;
				words[nwords + 1u] = static_cast<uint32_t>(data[i + 4u]) |
				                     (static_cast<uint32_t>(data[i + 5u]) << 8) |
				                     (static_cast<uint32_t>(data[i + 6u]) << 16) |
				                     (static_cast<uint32_t>(data[i + 7u]) << 24);
				nwords += 2u;
			}
			if (nwords > 0) {
				ump_rx_push_words(words, static_cast<uint16_t>(nwords));
			}
			midici_poll();
			midici_responder_poll();
			__wrap_usbmidi_poll();
			uint32_t tmp[64];
			while (e2e_tx_count() > 0) {
				(void)e2e_tx_pop_ump(tmp, 64);
				if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
				__wrap_usbmidi_poll();
			}
			break;
		}
		case 1: {
			const uint16_t rawLen = static_cast<uint16_t>(std::min<size_t>(size, 64u));
			e2e_host_send_ep1_raw(rawLen > 0 ? data : nullptr, rawLen);
			__wrap_midi_parsers_poll();
			__wrap_usbmidi_poll();
			if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
			uint32_t tmp[64];
			(void)e2e_tx_pop_ump(tmp, 64);
			break;
		}
		case 2: {
			const size_t parseSize = std::min<size_t>(size, 65535u);
			ms::ParsedMessage msg{};
			(void)ms::Parse(data, parseSize, msg);

			const uint16_t jsonLen = static_cast<uint16_t>(std::min<size_t>(size, 1024u));
			struct ci_json_it it {};
			struct ci_json_member mem {};
			ci_json_it_init(&it, reinterpret_cast<const char*>(data), jsonLen);
			int steps = 0;
			int rc = 0;
			while ((rc = ci_json_next(&it, &mem)) == 1) {
				++steps;
				E2E_CHECK(steps <= static_cast<int>(jsonLen) + 1);
				if (steps > static_cast<int>(jsonLen) + 1) break;
			}
			E2E_CHECK(rc == 0 || rc == -1);
			if (jsonLen > 0) {
				(void)m2s::ci::ciJsonString(
				    std::string(reinterpret_cast<const char*>(data), jsonLen), "resource");
			}
			break;
		}
		case 3:
		default: {
			if (size >= 8u) {
				const uint32_t w[2] = {
				    0x30160000u | (static_cast<uint32_t>(data[0] & 0x7Fu) << 8) | (data[1] & 0x7Fu),
				    (static_cast<uint32_t>(data[2]) << 24) |
				        (static_cast<uint32_t>(data[3]) << 16) |
				        (static_cast<uint32_t>(data[4]) << 8) | static_cast<uint32_t>(data[5]),
				};
				ump_rx_push_words(w, 2u);
			}
			midici_reset();
			midici_poll();
			midici_responder_poll();
			__wrap_usbmidi_poll();
			if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
			uint32_t tmp[64];
			(void)e2e_tx_pop_ump(tmp, 64);
			break;
		}
	}
}

}  // namespace

// ============================================================================
// Tier 1 — F18: SysEx7 & Parse Fuzzing (T1-F18-01..05)
// ============================================================================

E2E_TEST(1, "T1-F18-01", "Curated seed corpus replay across SysEx7 reassembly, ms::Parse, and ci_json") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	const uint32_t drops0 = midici_reassembly_drops();
	const uint32_t rx0 = midici_messages_received();

	const std::vector<uint8_t> disc = make_valid_discovery_v2(0x0111222u, 512u);
	std::vector<uint8_t> epInq(ms::EndpointInquiryByteCount, 0u);
	epInq.resize(ms::BuildEndpointInquiry(0x0111222u, 0x0222333u,
	                                      ms::EndpointStatusProductInstanceId, epInq.data(),
	                                      epInq.size()));
	const std::vector<uint8_t> peGet =
	    make_valid_pe_get(0x0111222u, 0x0222333u, 1u, R"({"resource":"DeviceInfo"})");

	const std::vector<uint8_t> ci5 = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x01u};
	const std::vector<uint8_t> ci6 = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x01u};
	const std::vector<uint8_t> ci13(13u, 0x00u);
	std::vector<uint8_t> ci13Copy = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 1, 2, 3, 4, 0x7F, 0x7F, 0x7F, 0x7F};
	(void)ci13;
	std::vector<uint8_t> ci512(512u, 0x2Au);
	ci512[0] = 0x7Eu;
	ci512[1] = 0x7Fu;
	ci512[2] = 0x0Du;
	ci512[3] = 0x70u;
	ci512[4] = 0x02u;

	const std::vector<std::vector<uint8_t>> ciSeeds = {disc, epInq, peGet, ci5, ci6, ci13Copy, ci512};
	for (const auto& seed : ciSeeds) {
		const auto words = e2e::pack_sysex7_words(0, seed);
		fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
		fw.poll(4);
	}
	// Also feed a 0-byte SysEx7 packet (valid non-CI empty payload -> 0 drops, 0 CI count)
	const std::vector<uint32_t> emptyWords = e2e::pack_sysex7_words(0, {});
	fw.send_ump(emptyWords.data(), static_cast<uint16_t>(emptyWords.size()));
	fw.poll(2);

	E2E_CHECK_EQ(midici_reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(midici_messages_received() - rx0, static_cast<uint32_t>(ciSeeds.size()));
	E2E_CHECK_EQ(spy_log().size(), ciSeeds.size());
	for (size_t i = 0; i < ciSeeds.size() && i < spy_log().size(); ++i) {
		E2E_CHECK(spy_log()[i].payload == ciSeeds[i]);
	}
	E2E_CHECK(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS);

	for (const auto& validCi : {disc, epInq, peGet}) {
		ms::ParsedMessage parsed{};
		E2E_CHECK(ms::Parse(validCi.data(), validCi.size(), parsed) == ms::ParseStatus::Ok);
	}

	const std::string hdrs[] = {
	    R"({"resource":"DeviceInfo"})",
	    R"( { "offset" : 0 , "resource" : "DeviceInfo" , "note" : "a\"b" } )",
	};
	for (const auto& h : hdrs) {
		struct ci_json_it it {};
		struct ci_json_member mem {};
		ci_json_it_init(&it, h.data(), static_cast<uint16_t>(h.size()));
		bool sawResource = false;
		int rc = 0;
		while ((rc = ci_json_next(&it, &mem)) == 1) {
			if (ci_json_key_is(&mem, "resource")) {
				sawResource = (ci_json_str_is(&mem, "DeviceInfo") != 0u);
			}
		}
		E2E_CHECK_EQ(rc, 0);
		E2E_CHECK(sawResource);
	}

	midici_responder_init();
}

E2E_TEST(1, "T1-F18-02", "5000-iteration deterministic PRNG over MT 0x3 words preserves invariants and 7-bit masking") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	const uint32_t drops0 = midici_reassembly_drops();
	SplitMix64 rng(0xC1FA1802ULL);

	for (int iter = 0; iter < 5000; ++iter) {
		const uint8_t group = rng.next_u8() & 0x0Fu;
		const uint8_t st = rng.next_u8() & 0x0Fu;
		const uint8_t cnt = rng.next_u8() & 0x0Fu;
		const uint32_t w0 = 0x30000000u | (static_cast<uint32_t>(group) << 24) |
		                    (static_cast<uint32_t>(st) << 20) |
		                    (static_cast<uint32_t>(cnt) << 16) |
		                    (static_cast<uint32_t>(rng.next_u8()) << 8) | rng.next_u8();
		const uint32_t w1 = rng.next_u32();
		const uint32_t pkt[2] = {w0, w1};
		(void)ump_rx_push_words(pkt, 2u);
		if ((iter & 3) == 0) {
			midici_poll();
			__wrap_usbmidi_poll();
			if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
			uint32_t tmp[32];
			(void)e2e_tx_pop_ump(tmp, 32);
		}
	}
	midici_poll();

	const uint32_t dropsAfter = midici_reassembly_drops();
	E2E_CHECK(dropsAfter > drops0);
	E2E_CHECK(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS);
	E2E_CHECK(midici_tx_pending() <= CI_TX_QUEUE_PACKETS);

	for (const auto& m : spy_log()) {
		E2E_CHECK(m.payload.size() <= CI_REASSEMBLY_BYTES);
		for (uint8_t b : m.payload) {
			E2E_CHECK((b & 0x80u) == 0u);
		}
	}

	midici_responder_init();
}

E2E_TEST(1, "T1-F18-03", "Byte-by-byte truncation sweep over all MIDI-CI message types in ms::Parse and firmware") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const std::vector<uint8_t> disc = make_valid_discovery_v2(0x0111111u, 512u);
	const std::vector<uint8_t> ep = make_valid_endpoint_reply(0x0111111u, 0x0222222u, "TEST0001");

	std::vector<uint8_t> prof(64u, 0u);
	const uint8_t enProf[5] = {0x7Eu, 0x00u, 0x01u, 0x00u, 0x01u};
	const uint8_t disProf[5] = {0x7Eu, 0x01u, 0x02u, 0x00u, 0x01u};
	prof.resize(ms::BuildProfileInquiryReply(ms::DeviceIdFunctionBlock, 0x0111111u, 0x0222222u,
	                                         enProf, 1u, disProf, 1u, prof.data(), prof.size()));

	const std::vector<uint8_t> pe =
	    make_valid_pe_get(0x0111111u, 0x0222222u, 7u, R"({"resource":"DeviceInfo"})", R"({"a":1})");

	std::vector<uint8_t> inv(ms::InvalidateMuidByteCount, 0u);
	inv.resize(ms::BuildInvalidateMuid(0x0111111u, 0x0222222u, inv.data(), inv.size()));

	std::vector<uint8_t> nak(64u, 0u);
	ms::AcknowledgmentFields af{};
	af.OriginalMessageType = static_cast<uint8_t>(ms::MessageType::PropertyGetDataInquiry);
	af.StatusCode = ms::NakStatusMessageMalformed;
	const char* nakText = "bad";
	nak.resize(ms::BuildAcknowledgment(ms::MessageType::Nak, ms::DeviceIdFunctionBlock,
	                                   0x0111111u, 0x0222222u, af,
	                                   reinterpret_cast<const uint8_t*>(nakText), 3u, nak.data(),
	                                   nak.size()));

	// Exact ParseStatus sweep for each message type across every prefix [0 .. full_len - 1]
	for (size_t k = 0; k < disc.size(); ++k) {
		ms::ParsedMessage m{};
		const auto st = ms::Parse(disc.data(), k, m);
		E2E_CHECK(st == (k < ms::CommonHeaderByteCount ? ms::ParseStatus::TooShort
		                                               : ms::ParseStatus::Ok));
	}
	for (size_t k = 0; k < ep.size(); ++k) {
		ms::ParsedMessage m{};
		const auto st = ms::Parse(ep.data(), k, m);
		E2E_CHECK(st == (k < ms::EndpointReplyFixedByteCount
		                     ? ms::ParseStatus::TooShort
		                     : ms::ParseStatus::LengthFieldExceedsBuffer));
	}
	for (size_t k = 0; k < prof.size(); ++k) {
		ms::ParsedMessage m{};
		const auto st = ms::Parse(prof.data(), k, m);
		ms::ParseStatus expected = ms::ParseStatus::Ok;
		if (k < 15u || (k >= 20u && k < 22u)) {
			expected = ms::ParseStatus::TooShort;
		} else {
			expected = ms::ParseStatus::LengthFieldExceedsBuffer;
		}
		E2E_CHECK(st == expected);
	}
	for (size_t k = 0; k < pe.size(); ++k) {
		ms::ParsedMessage m{};
		const auto st = ms::Parse(pe.data(), k, m);
		const size_t hdrEnd = 16u + 25u;  // 16 + len("{\"resource\":\"DeviceInfo\"}") (25)
		ms::ParseStatus expected = ms::ParseStatus::TooShort;
		if (k < 16u) {
			expected = ms::ParseStatus::TooShort;
		} else if (k < hdrEnd) {
			expected = ms::ParseStatus::LengthFieldExceedsBuffer;
		} else if (k < hdrEnd + 6u) {
			expected = ms::ParseStatus::TooShort;
		} else {
			expected = ms::ParseStatus::LengthFieldExceedsBuffer;
		}
		E2E_CHECK(st == expected);
	}
	for (size_t k = 0; k < inv.size(); ++k) {
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(inv.data(), k, m) == ms::ParseStatus::TooShort);
	}
	for (size_t k = 0; k < nak.size(); ++k) {
		ms::ParsedMessage m{};
		const auto st = ms::Parse(nak.data(), k, m);
		ms::ParseStatus expected = ms::ParseStatus::TooShort;
		if (k < ms::CommonHeaderByteCount) {
			expected = ms::ParseStatus::TooShort;
		} else if (k < ms::CommonHeaderByteCount + 10u) {
			expected = ms::ParseStatus::Ok;  // bare v1 acknowledgment
		} else {
			expected = ms::ParseStatus::LengthFieldExceedsBuffer;
		}
		E2E_CHECK(st == expected);
	}

	// Feed truncated prefixes into firmware over EP1 OUT -> zero faults, queue stays bounded
	const std::array<const std::vector<uint8_t>*, 6> truncTargets = {&disc, &ep, &prof, &pe, &inv, &nak};
	for (const auto* vec : truncTargets) {
		for (size_t k = 1; k < vec->size(); k += 3u) {
			std::vector<uint8_t> sub(vec->begin(), vec->begin() + static_cast<ptrdiff_t>(k));
			const auto words = e2e::pack_sysex7_words(0, sub);
			fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
			fw.poll(2);
			(void)fw.drain_ump();
		}
	}
	E2E_CHECK(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS);
	E2E_CHECK(midici_tx_pending() <= CI_TX_QUEUE_PACKETS);
}

E2E_TEST(1, "T1-F18-04", "Multi-mode fuzz step parity across raw MT3, EP1 raw, ms::Parse/ci_json, and resets") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	SplitMix64 rng(0xC1FA1804ULL);
	std::vector<uint8_t> buf;
	for (int iter = 0; iter < 2000; ++iter) {
		const size_t len = static_cast<size_t>(rng.next_u32() % 601u);
		buf.resize(len);
		for (size_t i = 0; i < len; ++i) {
			buf[i] = rng.next_u8();
		}
		run_fuzz_step(buf.data(), buf.size(), static_cast<uint8_t>(iter & 0x03));
	}
	E2E_CHECK(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS);
	E2E_CHECK(midici_tx_pending() <= CI_TX_QUEUE_PACKETS);
}

E2E_TEST(1, "T1-F18-05", "Post-fuzz recovery restores UMP Stream, SysEx7 loopback, and CiAgent Discovery/PE 200") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	SplitMix64 rng(0xC1FA1805ULL);
	for (int i = 0; i < 1000; ++i) {
		const uint32_t pkt[2] = {0x30000000u | (rng.next_u32() & 0x0FFFFFFFu), rng.next_u32()};
		(void)ump_rx_push_words(pkt, 2u);
		if ((i & 7) == 0) midici_poll();
	}
	for (int i = 0; i < 50; ++i) {
		uint8_t raw[64];
		const uint16_t rlen = static_cast<uint16_t>((rng.next_u32() % 64u) + 1u);
		for (uint16_t j = 0; j < rlen; ++j) raw[j] = rng.next_u8();
		e2e_host_send_ep1_raw(raw, rlen);
		if ((i & 3) == 0) midici_reset();
		fw.poll_once();
	}

	// Clean post-fuzz transport drain/reset (without rebooting app_init)
	ump_rx_reset();
	ump_tx_reset();
	midici_reset();
	fw.poll(4);
	(void)fw.drain_ump();

	// Unconditional UMP Stream EP Discovery recovery
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	fw.poll(4);
	const std::vector<uint32_t> epWords = fw.drain_ump();
	bool sawEpInfo = false;
	for (size_t i = 0; i + 3u < epWords.size(); i += 4u) {
		if (epWords[i] == 0xF0010101u) sawEpInfo = true;
	}
	E2E_CHECK(sawEpInfo);

	// Unconditional midici_send_sysex TX recovery
	const uint8_t ci6[6] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x55u};
	E2E_CHECK_EQ(midici_send_sysex(0, ci6, sizeof(ci6)), 0);
	fw.poll(4);
	const auto txPkts = fw.drain_tx_packets();
	std::vector<uint8_t> reassembled;
	E2E_CHECK(e2e::reassemble_sysex7(txPkts, 0, reassembled));
	E2E_CHECK(reassembled == std::vector<uint8_t>(ci6, ci6 + sizeof(ci6)));

#if E2E_HAS_M2
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0F18050u);
	E2E_CHECK(wire.discover_from_agent());
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	E2E_CHECK_EQ(snap.peers.size(), 1u);
	if (snap.peers.size() == 1u) {
		E2E_CHECK_EQ(snap.peers[0].resourceListStatus, 200);
		E2E_CHECK_EQ(snap.peers[0].deviceInfoStatus, 200);
	}
#endif
}

// ============================================================================
// Tier 2 — F18: Adversarial Edge Cases (T2-F18-01..05)
// ============================================================================

E2E_TEST(2, "T2-F18-01", "Extreme byte patterns (0B/1B/7B/64B 0x00/0xFF/0x7F/>64B) and 513B overlong drop") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	// Boundary EP1 OUT raw lengths including nullptr/0 and >64B clamp
	e2e_host_send_ep1_raw(nullptr, 0u);
	const uint8_t one = 0x30u;
	e2e_host_send_ep1_raw(&one, 1u);
	const uint8_t odd7[7] = {0x01, 0x02, 0x03, 0x30, 0x7E, 0x7F, 0x0D};
	e2e_host_send_ep1_raw(odd7, sizeof(odd7));
	fw.poll(2);

	const std::array<uint8_t, 64> zeros{};
	std::array<uint8_t, 64> ffs{};
	ffs.fill(0xFFu);
	std::array<uint8_t, 64> sevens{};
	sevens.fill(0x7Fu);
	std::array<uint8_t, 80> over80{};
	over80.fill(0xAAu);

	e2e_host_send_ep1_raw(zeros.data(), static_cast<uint16_t>(zeros.size()));
	fw.poll(1);
	e2e_host_send_ep1_raw(ffs.data(), static_cast<uint16_t>(ffs.size()));
	fw.poll(1);
	e2e_host_send_ep1_raw(sevens.data(), static_cast<uint16_t>(sevens.size()));
	fw.poll(1);
	e2e_host_send_ep1_raw(over80.data(), static_cast<uint16_t>(over80.size()));
	fw.poll(2);

	ms::ParsedMessage m{};
	E2E_CHECK(ms::Parse(nullptr, 0u, m) == ms::ParseStatus::TooShort);
	E2E_CHECK(ms::Parse(ffs.data(), ffs.size(), m) == ms::ParseStatus::NotUniversalSystemExclusive);

	// Reset partial UMP RX state cleanly and test 0B, 512B (accepted), 513B (dropped)
	ump_rx_reset();
	midici_reset();
	fw.poll(2);
	spy_log().clear();
	const uint32_t drops0 = midici_reassembly_drops();

	std::vector<uint8_t> ok512(512u, 0x11u);
	ok512[0] = 0x7Eu;
	ok512[1] = 0x7Fu;
	ok512[2] = 0x0Du;
	ok512[3] = 0x70u;
	ok512[4] = 0x02u;
	const auto w512 = e2e::pack_sysex7_words(0, ok512);
	fw.send_ump(w512.data(), static_cast<uint16_t>(w512.size()));
	fw.poll(4);
	E2E_CHECK_EQ(midici_reassembly_drops() - drops0, 0u);
	E2E_CHECK_EQ(spy_log().size(), 1u);

	std::vector<uint8_t> bad513(513u, 0x22u);
	bad513[0] = 0x7Eu;
	bad513[1] = 0x7Fu;
	bad513[2] = 0x0Du;
	bad513[3] = 0x70u;
	bad513[4] = 0x02u;
	const auto w513 = e2e::pack_sysex7_words(0, bad513);
	fw.send_ump(w513.data(), static_cast<uint16_t>(w513.size()));
	fw.poll(4);
	E2E_CHECK(midici_reassembly_drops() - drops0 >= 1u);
	E2E_CHECK_EQ(spy_log().size(), 1u);

	midici_responder_init();
}

E2E_TEST(2, "T2-F18-02", "Exhaustive MessageType 0x00..0x7F sweep across boundary lengths in ms::Parse and firmware") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);

	const size_t lengths[] = {0u, 12u, 13u, 14u, 15u, 17u, 23u, 29u, 31u, 512u};
	std::vector<uint8_t> msg(512u, 0u);
	msg[0] = ms::UniversalSystemExclusiveId;
	msg[1] = ms::DeviceIdFunctionBlock;
	msg[2] = ms::SubId1CapabilityInquiry;
	msg[4] = ms::MessageVersion12;
	ms::WriteMuid(&msg[5], 0x0111222u);
	ms::WriteMuid(&msg[9], 0x0333444u);

	for (uint16_t mt = 0; mt <= 0x7Fu; ++mt) {
		msg[3] = static_cast<uint8_t>(mt);
		for (size_t L : lengths) {
			ms::ParsedMessage parsed{};
			const auto st = ms::Parse(L > 0 ? msg.data() : nullptr, L, parsed);
			if (L < ms::CommonHeaderByteCount) {
				E2E_CHECK(st == ms::ParseStatus::TooShort);
			}
		}
		std::vector<uint8_t> sub(msg.begin(), msg.begin() + 31);
		const auto words = e2e::pack_sysex7_words(0, sub);
		fw.send_ump(words.data(), static_cast<uint16_t>(words.size()));
		fw.poll(2);
		(void)fw.drain_ump();
	}
	E2E_CHECK(midici_tx_pending() <= CI_TX_QUEUE_PACKETS);
	E2E_CHECK(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS);
}

E2E_TEST(2, "T2-F18-03", "High-bit 0x80..0xFF masking and adversarial JSON escapes, controls, and depth > 8") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	// Construct raw MT 0x3 kComplete packet carrying 0x80..0xFF bytes in payload
	const uint32_t pkt[2] = {0x3006FEFFu, 0x8D80C5FFu};
	fw.send_ump(pkt, 2u);
	fw.poll(2);
	E2E_CHECK_EQ(spy_log().size(), 1u);
	if (spy_log().size() == 1u) {
		const std::vector<uint8_t> expected = {0x7Eu, 0x7Fu, 0x0Du, 0x00u, 0x45u, 0x7Fu};
		E2E_CHECK(spy_log()[0].payload == expected);
	}

	// Adversarial JSON checks on ci_json_it_init / ci_json_next and ciJsonString
	const std::string trailingSlash = R"({"resource":"a\)";
	struct ci_json_it it {};
	struct ci_json_member mem {};
	ci_json_it_init(&it, trailingSlash.data(), static_cast<uint16_t>(trailingSlash.size()));
	E2E_CHECK_EQ(ci_json_next(&it, &mem), -1);
	E2E_CHECK(m2s::ci::ciJsonString(trailingSlash, "resource") == "a\\");

	const char ctrlJson[] = {'{', '"', 'r', 'e', 's', 'o', 'u', 'r', 'c', 'e', '"', ':', '"', 'a', 0x01, 'b', '"', '}'};
	ci_json_it_init(&it, ctrlJson, static_cast<uint16_t>(sizeof(ctrlJson)));
	E2E_CHECK_EQ(ci_json_next(&it, &mem), -1);

	const std::string deep9 = R"({"a":[[[[[[[[[1]]]]]]]]]})";
	ci_json_it_init(&it, deep9.data(), static_cast<uint16_t>(deep9.size()));
	E2E_CHECK_EQ(ci_json_next(&it, &mem), -1);

	const std::string trailingGarbage = R"({"resource":"DeviceInfo"}xyz)";
	ci_json_it_init(&it, trailingGarbage.data(), static_cast<uint16_t>(trailingGarbage.size()));
	E2E_CHECK_EQ(ci_json_next(&it, &mem), 1);
	E2E_CHECK(ci_json_key_is(&mem, "resource") != 0u);
	E2E_CHECK(ci_json_str_is(&mem, "DeviceInfo") != 0u);
	E2E_CHECK_EQ(ci_json_next(&it, &mem), -1);

	midici_responder_init();
}

E2E_TEST(2, "T2-F18-04", "Interleaved midici_reset(), RX queue overflow, and no pre/post-reset partial stitching") {
	e2e::FirmwareNode fw;
	fw.boot(true, 1);
	install_spy();

	// 1. Overflow CiRxQueue (capacity = CI_RX_QUEUE_PACKETS = 64) then midici_reset() recovers all 64 slots
	const uint32_t ovf0 = midici_rx_overflows();
	const uint32_t filler[2] = {0x32000000u, 0u};
	for (uint16_t i = 0; i < CI_RX_QUEUE_PACKETS; ++i) {
		E2E_CHECK_EQ(midici_rx_push(filler), 0);
	}
	E2E_CHECK_EQ(midici_rx_free_packets(), 0u);
	E2E_CHECK_EQ(midici_rx_push(filler), -1);
	E2E_CHECK_EQ(midici_rx_overflows() - ovf0, 1u);

	midici_reset();
	midici_poll();
	E2E_CHECK_EQ(midici_rx_free_packets(), CI_RX_QUEUE_PACKETS);

	// 2. Pre-reset kStart followed by post-reset kEnd must NEVER stitch old + new
	const uint32_t drops0 = midici_reassembly_drops();
	const uint32_t startPkt[2] = {0x30167E7Fu, 0x0D700201u};
	const uint32_t endPkt[2] = {0x30320102u, 0u};
	E2E_CHECK_EQ(midici_rx_push(startPkt), 0);
	midici_poll();
	midici_reset();
	midici_poll();
	E2E_CHECK_EQ(midici_rx_push(endPkt), 0);
	midici_poll();
	E2E_CHECK(midici_reassembly_drops() > drops0);
	E2E_CHECK(spy_log().empty());

	// 3. 2000 rapid PRNG reset + partial MT 0x3 interleavings
	SplitMix64 rng(0xC1FA1804BEEFULL);
	for (int i = 0; i < 2000; ++i) {
		const uint32_t p[2] = {0x30000000u | (rng.next_u32() & 0x0FFFFFFFu), rng.next_u32()};
		(void)midici_rx_push(p);
		if ((i % 5) == 0) midici_poll();
		if ((i % 7) == 0) midici_reset();
		if ((i % 19) == 0) ump_rx_reset();
	}
	midici_reset();
	midici_poll();
	E2E_CHECK_EQ(midici_rx_free_packets(), CI_RX_QUEUE_PACKETS);

	midici_responder_init();
}

E2E_TEST(2, "T2-F18-05", "Max 14-bit (0x3FFF) and 28-bit (0x0FFFFFFF) length fields and >65535 ProfileInquiryReply safety") {
	// 1. EndpointReply InformationByteCount = 0x3FFF
	{
		std::vector<uint8_t> b(ms::EndpointReplyFixedByteCount + 4u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::EndpointReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&b[14], 0x3FFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 2. ProfileInquiryReply EnabledProfileCount = 0x3FFF, and DisabledProfileCount = 0x3FFF
	{
		std::vector<uint8_t> b(ms::CommonHeaderByteCount + 6u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileInquiryReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&b[13], 0x3FFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);

		ms::WriteFourteenBitValue(&b[13], 0u);
		ms::WriteFourteenBitValue(&b[15], 0x3FFFu);
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 3. ProfileDetailsInquiryReply TargetDataByteCount = 0x3FFF
	{
		std::vector<uint8_t> b(ms::CommonHeaderByteCount + 10u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileDetailsInquiryReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&b[19], 0x3FFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 4. ProfileSpecificData TargetDataByteCount = 0x0FFFFFFF
	{
		std::vector<uint8_t> b(ms::CommonHeaderByteCount + 12u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileSpecificData, 0x0111111u, 0x0222222u);
		ms::WriteTwentyEightBitValue(&b[18], 0x0FFFFFFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 5. Nak MessageTextByteCount = 0x3FFF
	{
		std::vector<uint8_t> b(ms::CommonHeaderByteCount + 12u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock, ms::MessageType::Nak,
		                      0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&b[21], 0x3FFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 6. PropertyGetDataInquiry HeaderByteCount = 0x3FFF and DataByteCount = 0x3FFF
	{
		std::vector<uint8_t> b(ms::PropertyExchangeFixedByteCount + 4u, 0u);
		ms::WriteCommonHeader(b.data(), b.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::PropertyGetDataInquiry, 0x0111111u, 0x0222222u);
		b[13] = 1u;
		ms::WriteFourteenBitValue(&b[14], 0x3FFFu);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);

		ms::WriteFourteenBitValue(&b[14], 0u);
		ms::WriteFourteenBitValue(&b[16], 1u);
		ms::WriteFourteenBitValue(&b[18], 1u);
		ms::WriteFourteenBitValue(&b[20], 0x3FFFu);
		E2E_CHECK(ms::Parse(b.data(), b.size(), m) == ms::ParseStatus::LengthFieldExceedsBuffer);
	}

	// 7. >65535-byte ProfileInquiryReply (EnabledProfileCount=13999 -> 69995B + header/disabled=0)
	{
		std::vector<uint8_t> big(70020u, 0x01u);
		ms::WriteCommonHeader(big.data(), big.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileInquiryReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&big[13], 13999u);
		const size_t disOff = 15u + 13999u * 5u;
		ms::WriteFourteenBitValue(&big[disOff], 0u);
		ms::ParsedMessage m{};
		E2E_CHECK(ms::Parse(big.data(), big.size(), m) == ms::ParseStatus::Ok);
		E2E_CHECK_EQ(m.Profile.EnabledProfileCount, 13999u);
		E2E_CHECK_EQ(m.Profile.DisabledProfileCount, 0u);
	}
}
