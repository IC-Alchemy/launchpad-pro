#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump.h"

extern "C" {
#include "app.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "midici/initiator.h"
#include "midici/midici.h"
#include "midici/responder.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ms = m2s::ci::ms;

bool g_inited = false;
#if E2E_HAS_M3
uint32_t g_fuzz_ms = 100u;
#endif

void init_fuzz_once() {
	if (g_inited) return;
	e2e_stubs_reset();
	ump_rx_reset();
	ump_tx_reset();
	ump_stream_reset();
	midici_init();
	__wrap_midi_parsers_init();
	app_init(e2e_adc);
	e2e_set_device_state(USB_STATE_CONFIGURED);
	usb_midi2_set_alt(1);
	usb_midi2_service();
	g_inited = true;
}

void drain_ep1_tx_fast() {
	uint32_t tmp[64];
	for (int i = 0; i < 16; ++i) {
		if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
		__wrap_usbmidi_poll();
		while (e2e_tx_count() > 0u) {
			(void)e2e_tx_pop_ump(tmp, 64u);
		}
		if (midici_tx_pending() == 0u && !e2e_ep1_tx_is_valid()) break;
	}
}

void check_invariant(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[fuzz_ci] INVARIANT FAILED: %s\n", msg);
		std::abort();
	}
}

void fuzz_one_input(const uint8_t* data, size_t size) {
	if (!g_inited) init_fuzz_once();

	if (!data || size == 0u) {
		ms::ParsedMessage m{};
		check_invariant(ms::Parse(nullptr, 0u, m) == ms::ParseStatus::TooShort,
		                "Parse(nullptr, 0) != TooShort");
		struct ci_json_it it {};
		struct ci_json_member mem {};
		ci_json_it_init(&it, "", 0u);
		check_invariant(ci_json_next(&it, &mem) == -1, "ci_json_next empty != -1");
		midici_poll();
		return;
	}

	const uint8_t selector = data[0];
	const size_t parseSize = std::min<size_t>(size, 65535u);
	const uint16_t len16 = static_cast<uint16_t>(parseSize);
	const uint16_t ciLen = static_cast<uint16_t>(std::min<size_t>(size, 520u));

	switch (selector & 0x03u) {
		case 0: {
			// Branch 0: Raw MT 0x3 words + odd EP1 OUT packets into reassembler & USB RX
			uint32_t words[32] = {};
			const size_t nbytes = std::min<size_t>(size, sizeof(words));
			size_t nwords = 0u;
			for (size_t i = 0; i + 7u < nbytes && nwords + 1u < 32u; i += 8u) {
				uint32_t w0 = static_cast<uint32_t>(data[i]) |
				              (static_cast<uint32_t>(data[i + 1u]) << 8) |
				              (static_cast<uint32_t>(data[i + 2u]) << 16) |
				              (static_cast<uint32_t>(data[i + 3u]) << 24);
				w0 = (w0 & 0x0FFFFFFFu) | 0x30000000u;
				const uint32_t w1 = static_cast<uint32_t>(data[i + 4u]) |
				                    (static_cast<uint32_t>(data[i + 5u]) << 8) |
				                    (static_cast<uint32_t>(data[i + 6u]) << 16) |
				                    (static_cast<uint32_t>(data[i + 7u]) << 24);
				words[nwords++] = w0;
				words[nwords++] = w1;
			}
			if (nwords > 0u) {
				ump_rx_push_words(words, static_cast<uint16_t>(nwords));
			}
			const uint16_t rawLen = static_cast<uint16_t>(std::min<size_t>(size, 64u));
			e2e_host_send_ep1_raw(data, rawLen);
			__wrap_midi_parsers_poll();
			midici_responder_poll();
			drain_ep1_tx_fast();
			check_invariant(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS,
			                "rx_free_packets > CI_RX_QUEUE_PACKETS");
			check_invariant(midici_tx_pending() <= CI_TX_QUEUE_PACKETS,
			                "tx_pending > CI_TX_QUEUE_PACKETS");
			break;
		}
		case 1: {
			// Branch 1: SysEx7-wrapped payload (including >512B overlong) -> Responder/Initiator
			const uint8_t group = static_cast<uint8_t>((selector >> 2) & 0x0Fu);
			const auto pkts = m2s::ci::packetizeSysEx7(group, data, ciLen);
			std::vector<uint32_t> words;
			words.reserve(pkts.size() * 2u);
			for (const auto& p : pkts) {
				words.push_back(p[0]);
				words.push_back(p[1]);
			}
			if (!words.empty()) {
				const size_t pushWords = std::min<size_t>(words.size(), UMP_RX_WORDS);
				ump_rx_push_words(words.data(), static_cast<uint16_t>(pushWords));
			}
			ump_rx_poll();
			midici_poll();
			midici_responder_poll();
#if E2E_HAS_M3
			(void)midici_initiator_on_message(group, data, ciLen, ++g_fuzz_ms);
			midici_tick(g_fuzz_ms);
#endif
			drain_ep1_tx_fast();
			check_invariant(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS,
			                "rx_free_packets > CI_RX_QUEUE_PACKETS (branch 1)");
			break;
		}
		case 2: {
			// Branch 2: ms::Parse (raw + synthesized 7E <dev> 0D <type> header) & JSON parsers
			ms::ParsedMessage m{};
			(void)ms::Parse(data, parseSize, m);

			if (size >= 2u) {
				std::array<uint8_t, 520> synth{};
				const size_t n = std::min<size_t>(size, synth.size());
				std::memcpy(synth.data(), data, n);
				synth[0] = ms::UniversalSystemExclusiveId;
				synth[1] = data[1] & 0x7Fu;
				if (n >= 3u) synth[2] = ms::SubId1CapabilityInquiry;
				ms::ParsedMessage m2{};
				(void)ms::Parse(synth.data(), n, m2);
			}

			struct ci_json_it it {};
			struct ci_json_member mem {};
			ci_json_it_init(&it, reinterpret_cast<const char*>(data), len16);
			int steps = 0;
			int rc = 0;
			while ((rc = ci_json_next(&it, &mem)) == 1) {
				++steps;
				check_invariant(steps <= static_cast<int>(len16) + 1, "ci_json_next runaway");
			}
			check_invariant(rc == 0 || rc == -1, "ci_json_next invalid return");
			(void)m2s::ci::ciJsonString(
			    std::string(reinterpret_cast<const char*>(data), ciLen), "resource");
			break;
		}
		case 3:
		default: {
			// Branch 3: Reset & reassembly state-machine stress + TX packetizing
			if (size >= 8u) {
				const uint32_t pkt[2] = {
				    0x30100000u | (static_cast<uint32_t>(data[0] & 0x7Fu) << 8) | (data[1] & 0x7Fu),
				    (static_cast<uint32_t>(data[2]) << 24) |
				        (static_cast<uint32_t>(data[3]) << 16) |
				        (static_cast<uint32_t>(data[4]) << 8) | static_cast<uint32_t>(data[5]),
				};
				(void)midici_rx_push(pkt);
			}
			midici_reset();
			midici_responder_reset();
#if E2E_HAS_M3
			midici_initiator_reset();
#endif
			midici_poll();
			if (ciLen <= CI_REASSEMBLY_BYTES) {
				(void)midici_send_sysex(static_cast<uint8_t>(selector & 0x0Fu), data, ciLen);
			}
			(void)midici_tx_drain(CI_TX_DRAIN_MAX);
			drain_ep1_tx_fast();
			check_invariant(midici_rx_free_packets() <= CI_RX_QUEUE_PACKETS,
			                "rx_free_packets > CI_RX_QUEUE_PACKETS (branch 3)");
			break;
		}
	}
}

#ifndef FUZZ_LIBFUZZER

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

void run_curated_seed_corpus() {
	std::vector<std::vector<uint8_t>> corpus;

	// Boundary payload lengths: 0B, 1B, 12B, 13B, 512B, 513B
	corpus.push_back({});
	corpus.push_back({0x7Eu});
	corpus.push_back(std::vector<uint8_t>(12u, 0x7Eu));
	corpus.push_back(std::vector<uint8_t>(13u, 0x00u));
	corpus.push_back(std::vector<uint8_t>(512u, 0x41u));
	corpus.push_back(std::vector<uint8_t>(513u, 0x42u));

	// Discovery v1 (29B) & v2 (30B)
	{
		ms::DiscoveryReplyFields df{};
		df.SourceMuid = 0x0111111u;
		df.DestinationMuid = ms::MuidBroadcast;
		df.CapabilityCategories = 0x08u;
		df.ReceivableMaximumSysExSize = 512u;
		df.MessageVersion = ms::MessageVersion11;
		std::vector<uint8_t> d1(ms::DiscoveryByteCountVersion11, 0u);
		d1.resize(ms::BuildDiscovery(df, d1.data(), d1.size()));
		corpus.push_back(d1);

		df.MessageVersion = ms::MessageVersion12;
		std::vector<uint8_t> d2(ms::DiscoveryByteCount, 0u);
		d2.resize(ms::BuildDiscovery(df, d2.data(), d2.size()));
		corpus.push_back(d2);
	}

	// EndpointInquiry & adversarial 0x3FFF / 0x0FFFFFFF length fields
	{
		std::vector<uint8_t> ep(ms::EndpointInquiryByteCount, 0u);
		ep.resize(ms::BuildEndpointInquiry(0x0111111u, 0x0222222u, 0u, ep.data(), ep.size()));
		corpus.push_back(ep);

		std::vector<uint8_t> badEp(ms::EndpointReplyFixedByteCount + 4u, 0u);
		ms::WriteCommonHeader(badEp.data(), badEp.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::EndpointReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&badEp[14], 0x3FFFu);
		corpus.push_back(badEp);

		std::vector<uint8_t> badProf1(ms::CommonHeaderByteCount + 6u, 0u);
		ms::WriteCommonHeader(badProf1.data(), badProf1.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileInquiryReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&badProf1[13], 0x3FFFu);
		corpus.push_back(badProf1);

		std::vector<uint8_t> badProf2(ms::CommonHeaderByteCount + 6u, 0u);
		ms::WriteCommonHeader(badProf2.data(), badProf2.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileInquiryReply, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&badProf2[15], 0x3FFFu);
		corpus.push_back(badProf2);

		std::vector<uint8_t> badPsd(ms::CommonHeaderByteCount + 12u, 0u);
		ms::WriteCommonHeader(badPsd.data(), badPsd.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::ProfileSpecificData, 0x0111111u, 0x0222222u);
		ms::WriteTwentyEightBitValue(&badPsd[18], 0x0FFFFFFFu);
		corpus.push_back(badPsd);

		std::vector<uint8_t> badPe(ms::PropertyExchangeFixedByteCount + 4u, 0u);
		ms::WriteCommonHeader(badPe.data(), badPe.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::PropertyGetDataInquiry, 0x0111111u, 0x0222222u);
		badPe[13] = 1u;
		ms::WriteFourteenBitValue(&badPe[14], 0x3FFFu);
		ms::WriteFourteenBitValue(&badPe[20], 0x3FFFu);
		corpus.push_back(badPe);

		std::vector<uint8_t> inv(ms::InvalidateMuidByteCount, 0u);
		inv.resize(ms::BuildInvalidateMuid(0x0111111u, 0x0222222u, inv.data(), inv.size()));
		corpus.push_back(inv);

		std::vector<uint8_t> nak(ms::CommonHeaderByteCount + 12u, 0u);
		ms::WriteCommonHeader(nak.data(), nak.size(), ms::DeviceIdFunctionBlock,
		                      ms::MessageType::Nak, 0x0111111u, 0x0222222u);
		ms::WriteFourteenBitValue(&nak[21], 0x3FFFu);
		corpus.push_back(nak);
	}

	// Malformed JSON headers
	const char* badJsons[] = {
	    "",
	    "{}",
	    "{\"resource\":\"DeviceInf",
	    "{\"resource\":\"a\\",
	    "{\"resource\":\"a\\\"b\"}",
	    "{\"a\":[[[[[[[[[1]]]]]]]]]}",
	    "{\"resource\":\"DeviceInfo\"}xyz",
	};
	for (const char* s : badJsons) {
		const size_t n = std::strlen(s);
		corpus.emplace_back(reinterpret_cast<const uint8_t*>(s),
		                    reinterpret_cast<const uint8_t*>(s) + n);
	}

	// Sweep all MessageType 0x00..0x7F at 13B, 14B, 29B, 31B
	for (uint16_t mt = 0u; mt <= 0x7Fu; ++mt) {
		for (size_t L : {size_t{13}, size_t{14}, size_t{29}, size_t{31}}) {
			std::vector<uint8_t> m(L, 0u);
			ms::WriteCommonHeader(m.data(), m.size(), ms::DeviceIdFunctionBlock,
			                      static_cast<ms::MessageType>(mt), 0x0111111u, 0x0222222u);
			corpus.push_back(std::move(m));
		}
	}

	// Replay every seed across all 4 fuzz branches
	for (const auto& seed : corpus) {
		for (uint8_t branch = 0; branch < 4u; ++branch) {
			if (seed.empty()) {
				fuzz_one_input(nullptr, 0u);
			} else {
				std::vector<uint8_t> tagged = seed;
				tagged[0] = static_cast<uint8_t>((seed[0] & 0xFCu) | branch);
				fuzz_one_input(tagged.data(), tagged.size());
				// Also run exact untagged seed through Branch 2 (Parse/JSON)
				if (branch == 2u) {
					ms::ParsedMessage parsed{};
					(void)ms::Parse(seed.data(), std::min<size_t>(seed.size(), 65535u), parsed);
					struct ci_json_it it {};
					struct ci_json_member mem {};
					ci_json_it_init(&it, reinterpret_cast<const char*>(seed.data()),
					                static_cast<uint16_t>(std::min<size_t>(seed.size(), 65535u)));
					while (ci_json_next(&it, &mem) == 1) {
					}
				}
			}
		}
	}
}

bool verify_post_fuzz_recovery() {
	e2e::FirmwareNode fw;
	midici_init();
	fw.boot(true, 1);

	// 1. Unconditional UMP Stream EP Discovery
	const auto epDisc = e2e::make_ep_discovery(0x01u);
	fw.send_ump(epDisc.data(), static_cast<uint16_t>(epDisc.size()));
	fw.poll(4);
	const std::vector<uint32_t> epWords = fw.drain_ump();
	bool sawEpInfo = false;
	for (size_t i = 0; i + 3u < epWords.size(); i += 4u) {
		if (epWords[i] == 0xF0010101u) sawEpInfo = true;
	}
	if (!sawEpInfo) {
		std::fprintf(stderr, "[fuzz_ci] Recovery failed: missing ST_EP_INFO\n");
		return false;
	}

	// 2. Unconditional SysEx7 TX->RX 6-byte loopback
	const uint8_t ci6[6] = {0x7Eu, 0x7Fu, 0x0Du, 0x70u, 0x02u, 0x33u};
	if (midici_send_sysex(0, ci6, sizeof(ci6)) != 0) {
		std::fprintf(stderr, "[fuzz_ci] Recovery failed: midici_send_sysex != 0\n");
		return false;
	}
	fw.poll(4);
	const auto txPkts = fw.drain_tx_packets();
	std::vector<uint8_t> reassembled;
	if (!e2e::reassemble_sysex7(txPkts, 0, reassembled) ||
	    reassembled != std::vector<uint8_t>(ci6, ci6 + sizeof(ci6))) {
		std::fprintf(stderr, "[fuzz_ci] Recovery failed: SysEx7 6B loopback mismatch\n");
		return false;
	}

#if E2E_HAS_M2
	// 3. M2 Responder recovery against live CiAgent (Discovery + GET DeviceInfo 200)
	e2e::WirePair wire(fw, m2s::ci::CiIdentity{}, 0x0F022C1u);
	if (!wire.discover_from_agent()) {
		std::fprintf(stderr, "[fuzz_ci] Recovery failed: CiAgent discover_from_agent()\n");
		return false;
	}
	const m2s::CiSnapshot snap = wire.agent().snapshot();
	if (snap.peers.size() != 1u || snap.peers[0].resourceListStatus != 200 ||
	    snap.peers[0].deviceInfoStatus != 200) {
		std::fprintf(stderr, "[fuzz_ci] Recovery failed: peer=%zu rl=%d dev=%d\n",
		             snap.peers.size(),
		             snap.peers.empty() ? -1 : snap.peers[0].resourceListStatus,
		             snap.peers.empty() ? -1 : snap.peers[0].deviceInfoStatus);
		return false;
	}
#endif

	return true;
}

}  // namespace

int main(int argc, char** argv) {
	uint64_t seed = 0xC1FA2201ULL;
	uint64_t iters = 100000ULL;

	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		if (std::strncmp(arg, "--seed=", 7) == 0) {
			char* end = nullptr;
			errno = 0;
			const unsigned long long v = std::strtoull(arg + 7, &end, 0);
			if (arg[7] == '\0' || end == nullptr || *end != '\0' || errno == ERANGE) {
				std::fprintf(stderr, "Invalid --seed value: %s\n", arg);
				return 2;
			}
			seed = static_cast<uint64_t>(v);
		} else if (std::strncmp(arg, "--iters=", 8) == 0) {
			char* end = nullptr;
			errno = 0;
			const unsigned long long v = std::strtoull(arg + 8, &end, 0);
			if (arg[8] == '\0' || end == nullptr || *end != '\0' || errno == ERANGE || v == 0ULL) {
				std::fprintf(stderr, "Invalid --iters value: %s\n", arg);
				return 2;
			}
			iters = static_cast<uint64_t>(v);
		} else {
			std::fprintf(stderr, "Usage: %s [--seed=<u64>] [--iters=<N>]\n", argv[0]);
			return 2;
		}
	}

	init_fuzz_once();
	run_curated_seed_corpus();

	SplitMix64 rng(seed);
	const size_t corners[] = {0u, 1u, 7u, 12u, 13u, 14u, 15u, 29u, 31u, 64u, 512u, 513u};
	std::vector<uint8_t> buf;
	buf.reserve(608u);

	for (uint64_t iter = 0; iter < iters; ++iter) {
		size_t sz = 0u;
		if ((iter & 7u) == 0u) {
			sz = corners[rng.next_u32() % (sizeof(corners) / sizeof(corners[0]))];
		} else {
			sz = static_cast<size_t>(rng.next_u32() % 601u);
		}
		buf.resize(sz);
		for (size_t j = 0; j < sz; ++j) {
			buf[j] = rng.next_u8();
		}
		fuzz_one_input(buf.empty() ? nullptr : buf.data(), buf.size());
	}

	if (!verify_post_fuzz_recovery()) {
		return 1;
	}

	std::printf("[fuzz_ci] PASS: seed=0x%llx iters=%llu corpus+recovery OK [M1=%d M2=%d M3=%d]\n",
	            static_cast<unsigned long long>(seed), static_cast<unsigned long long>(iters),
	            E2E_HAS_M1, E2E_HAS_M2, E2E_HAS_M3);
	return 0;
}

#else  // FUZZ_LIBFUZZER

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
	(void)argc;
	(void)argv;
	init_fuzz_once();
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	fuzz_one_input(data, size);
	return 0;
}

#endif
