#include "e2e_harness.h"
#include "e2e_stubs.h"

#include "ci/ci_agent.h"
#include "ci/ms_midici.h"
#include "ci/sysex7.h"
#include "common/ump_text.h"

extern "C" {
#include "midici/ci_identity.h"
#include "ump/ump.h"
#include "usb/usb_compat.h"
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

enum class RunMode {
	All,
	Smoke,
	Tier,
	List,
};

void print_usage(const char* argv0) {
	std::printf(
	    "Usage: %s [--smoke | --tier=<1|2|3|4> | --all | --list] [--filter=<substr>] [--verbose]\n",
	    argv0);
}

// SMOKE-01: CiAgent self-pair Discovery + Endpoint + Property Exchange loopback
void run_smoke_ci_agent_pair() {
	using namespace m2s;
	using namespace m2s::ci;

	std::deque<UmpPacket> toA, toB;
	auto toPackets = [](const uint32_t* w, size_t n, uint16_t src) {
		std::vector<UmpPacket> out;
		for (size_t i = 0; i + 1 < n; i += 2) {
			UmpPacket p{};
			p.words[0] = w[i];
			p.words[1] = w[i + 1];
			p.wordCount = 2;
			p.source = src;
			out.push_back(p);
		}
		return out;
	};

	CiIdentity idA;
	idA.maxSysEx = 128;  // Exercise chunked PE reply from B
	idA.productInstanceId = "LPP-SMOKE-A";

	CiIdentity idB;
	idB.maxSysEx = 512;
	idB.productInstanceId = "LPP-SMOKE-B";
	idB.modelName = "Launchpad Pro Smoke Responder with chunked DeviceInfo payload";

	CiAgent agentA(
	    idA,
	    [&](uint16_t s, const uint32_t* w, size_t n) {
		    if (s != 20) return false;
		    for (const auto& p : toPackets(w, n, 10)) toB.push_back(p);
		    return true;
	    },
	    [](const UmpPacket&) {}, [] { return std::vector<uint16_t>{20}; }, 0x0123456);

	CiAgent agentB(
	    idB,
	    [&](uint16_t s, const uint32_t* w, size_t n) {
		    if (s != 10) return false;
		    for (const auto& p : toPackets(w, n, 20)) toA.push_back(p);
		    return true;
	    },
	    [](const UmpPacket&) {}, [] { return std::vector<uint16_t>{10}; }, 0x0765432);

	std::wstring err;
	E2E_CHECK(agentA.discover(&err));
	for (int round = 0; round < 64; ++round) {
		while (!toA.empty()) {
			agentA.receive(toA.front());
			toA.pop_front();
		}
		while (!toB.empty()) {
			agentB.receive(toB.front());
			toB.pop_front();
		}
		if (agentA.pump() + agentB.pump() == 0 && toA.empty() && toB.empty()) break;
	}

	const CiSnapshot snapA = agentA.snapshot();
	E2E_CHECK_EQ(snapA.muid, 0x0123456u);
	E2E_CHECK_EQ(snapA.peers.size(), 1u);
	if (snapA.peers.size() == 1) {
		const CiPeer& peer = snapA.peers[0];
		E2E_CHECK_EQ(peer.muid, 0x0765432u);
		E2E_CHECK(peer.answeredDiscovery);
		E2E_CHECK(peer.hasEndpointInfo);
		E2E_CHECK(peer.productInstanceId == "LPP-SMOKE-B");
		E2E_CHECK(peer.hasPeCaps);
		E2E_CHECK_EQ(peer.resourceListStatus, 200);
		E2E_CHECK_EQ(peer.deviceInfoStatus, 200);
		E2E_CHECK(ciJsonString(peer.deviceInfo, "model") == idB.modelName);
		E2E_CHECK(peer.notes.empty());
	}

	const CiSnapshot snapB = agentB.snapshot();
	E2E_CHECK_EQ(snapB.peers.size(), 1u);
	if (snapB.peers.size() == 1) {
		E2E_CHECK(snapB.peers[0].discoveredUs);
		E2E_CHECK_EQ(snapB.peers[0].muid, 0x0123456u);
	}
}

// SMOKE-02: FirmwareNode UMP Stream EP Discovery & Product ID loopback via EP1_OUT -> EP1_IN
void run_smoke_firmware_ump_stream() {
	e2e::FirmwareNode fw;
	fw.set_serial("SMOKE1234");
	fw.boot(true, 1);  // configured + USB alt 1

	const uint32_t disc[4] = {0xF0000101u, 0x0000001Fu, 0u, 0u};
	fw.send_ump(disc, 4);
	fw.poll(4);

	const std::vector<uint32_t> rxWords = fw.drain_ump();
	E2E_CHECK(!rxWords.empty());

	bool sawEpInfo = false;
	bool sawDevIdentity = false;
	bool sawConfigNotify = false;
	std::string epName;
	std::string productId;

	size_t pos = 0;
	while (pos < rxWords.size()) {
		const uint32_t w0 = rxWords[pos];
		const uint8_t wc = ump_word_count(w0);
		E2E_CHECK(wc >= 1 && pos + wc <= rxWords.size());
		if (wc == 0 || pos + wc > rxWords.size()) break;

		if ((w0 >> 28) == 0xFu && wc == 4) {
			const uint16_t status = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
			if (status == 0x001u) {
				sawEpInfo = true;
				E2E_CHECK_EQ(w0, 0xF0010101u);
				E2E_CHECK_EQ(rxWords[pos + 1], 0x82000300u);
			} else if (status == 0x002u) {
				sawDevIdentity = true;
				E2E_CHECK_EQ(rxWords[pos + 1], CI_UMP_IDENTITY_WORD1);
				E2E_CHECK_EQ(rxWords[pos + 2], CI_UMP_IDENTITY_WORD2);
				E2E_CHECK_EQ(rxWords[pos + 3], CI_UMP_IDENTITY_WORD3);
			} else if (status == 0x003u || status == 0x004u) {
				std::string* dst = (status == 0x003u) ? &epName : &productId;
				const uint8_t b0 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
				const uint8_t b1 = static_cast<uint8_t>(w0 & 0xFFu);
				if (b0 != 0) dst->push_back(static_cast<char>(b0));
				if (b1 != 0) dst->push_back(static_cast<char>(b1));
				for (size_t w = 1; w < 4; ++w) {
					for (int shift = 24; shift >= 0; shift -= 8) {
						const char c = static_cast<char>((rxWords[pos + w] >> shift) & 0xFFu);
						if (c != 0) dst->push_back(c);
					}
				}
			} else if (status == 0x006u) {
				sawConfigNotify = true;
				E2E_CHECK_EQ(w0, 0xF0060200u);
			}
		}
		pos += wc;
	}

	E2E_CHECK(sawEpInfo);
	E2E_CHECK(sawDevIdentity);
	E2E_CHECK(sawConfigNotify);
	E2E_CHECK(epName == "Launchpad Pro MIDI 2.0");
	E2E_CHECK(productId == "SMOKE1234");
}

int run_smoke_suite() {
	struct SmokeCase {
		const char* id;
		const char* name;
		void (*fn)();
	};
	const SmokeCase cases[] = {
	    {"SMOKE-01", "CiAgent self-pair Discovery/PE loopback", run_smoke_ci_agent_pair},
	    {"SMOKE-02", "FirmwareNode UMP Stream EP Discovery / Product ID loopback via EP1_OUT->EP1_IN",
	     run_smoke_firmware_ump_stream},
	};

	int failed = 0;
	std::printf("[e2e] Running smoke suite (%zu checks) [M1=%d M2=%d M3=%d]\n",
	            sizeof(cases) / sizeof(cases[0]), E2E_HAS_M1, E2E_HAS_M2, E2E_HAS_M3);
	for (const auto& c : cases) {
		e2e_stubs_reset();
		const int before = e2e::test_failure_count();
		c.fn();
		const int delta = e2e::test_failure_count() - before;
		if (delta == 0) {
			std::printf("[PASS] %s: %s\n", c.id, c.name);
		} else {
			std::printf("[FAIL] %s: %s (%d assertion failures)\n", c.id, c.name, delta);
			++failed;
		}
	}
	std::printf("[e2e] Smoke summary: %zu run, %d failed\n", sizeof(cases) / sizeof(cases[0]), failed);
	return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
	const char* prog = (argc > 0 && argv && argv[0]) ? argv[0] : "e2e_tests";
	RunMode mode = RunMode::All;
	int targetTier = 0;
	std::string filter;
	bool verbose = false;

	for (int i = 1; i < argc; ++i) {
		const char* arg = argv[i];
		if (std::strcmp(arg, "--smoke") == 0) {
			mode = RunMode::Smoke;
		} else if (std::strcmp(arg, "--all") == 0) {
			mode = RunMode::All;
		} else if (std::strcmp(arg, "--list") == 0) {
			mode = RunMode::List;
		} else if (std::strcmp(arg, "--verbose") == 0 || std::strcmp(arg, "-v") == 0) {
			verbose = true;
		} else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
			print_usage(prog);
			return 0;
		} else if (std::strncmp(arg, "--tier=", 7) == 0) {
			mode = RunMode::Tier;
			const char* val = arg + 7;
			char* end = nullptr;
			errno = 0;
			const long parsed = std::strtol(val, &end, 10);
			if (val[0] == '\0' || end == nullptr || *end != '\0' || errno == ERANGE ||
			    parsed < 1 || parsed > 4) {
				std::fprintf(stderr, "Invalid tier '%s' (expected 1..4)\n", val);
				return 2;
			}
			targetTier = static_cast<int>(parsed);
		} else if (std::strncmp(arg, "--filter=", 9) == 0) {
			filter = arg + 9;
		} else {
			std::fprintf(stderr, "Unknown option: %s\n", arg);
			print_usage(prog);
			return 2;
		}
	}

	if (mode == RunMode::Smoke) {
		return run_smoke_suite();
	}

	const auto& tests = e2e::registry();
	if (mode == RunMode::List) {
		for (const auto& t : tests) {
			if (!filter.empty() && std::string(t.id).find(filter) == std::string::npos &&
			    std::string(t.name).find(filter) == std::string::npos) {
				continue;
			}
			std::printf("[T%d] %s: %s\n", t.tier, t.id, t.name);
		}
		return 0;
	}

	int matched = 0;
	int passed = 0;
	int failed = 0;

	std::printf("[e2e] Executing %s%s [M1=%d M2=%d M3=%d]\n",
	            mode == RunMode::All ? "all tiers" : "tier ",
	            mode == RunMode::All ? "" : std::to_string(targetTier).c_str(),
	            E2E_HAS_M1, E2E_HAS_M2, E2E_HAS_M3);

	for (const auto& t : tests) {
		if (mode == RunMode::Tier && t.tier != targetTier) continue;
		if (!filter.empty() && std::string(t.id).find(filter) == std::string::npos &&
		    std::string(t.name).find(filter) == std::string::npos) {
			continue;
		}

		++matched;
		e2e_stubs_reset();
		const int before = e2e::test_failure_count();
		t.fn();
		const int delta = e2e::test_failure_count() - before;
		if (delta == 0) {
			++passed;
			if (verbose || mode == RunMode::Tier || mode == RunMode::All) {
				std::printf("[PASS] [T%d] %s: %s\n", t.tier, t.id, t.name);
			}
		} else {
			++failed;
			std::printf("[FAIL] [T%d] %s: %s (%d assertions failed)\n", t.tier, t.id, t.name, delta);
		}
	}

	if (matched == 0) {
		if (!filter.empty()) {
			std::fprintf(stderr, "[e2e] Filter '%s' matched 0 tests.\n", filter.c_str());
			return 1;
		}
		std::printf("[e2e] Tier %d: 0 tests registered (suite pending E2E-M2/M3).\n", targetTier);
		return 0;
	}

	std::printf("[e2e] Summary: %d run, %d passed, %d failed\n", matched, passed, failed);
	return failed ? 1 : 0;
}
