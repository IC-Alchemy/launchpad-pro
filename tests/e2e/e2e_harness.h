#pragma once

#ifndef E2E_HAS_M1
#define E2E_HAS_M1 0
#endif
#ifndef E2E_HAS_M2
#define E2E_HAS_M2 0
#endif
#ifndef E2E_HAS_M3
#define E2E_HAS_M3 0
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ci/ci_agent.h"
#include "ci/sysex7.h"
#include "common/ci_types.h"
#include "common/ump.h"
#include "common/ump_text.h"

extern "C" {
#include "e2e_stubs.h"
#include "app.h"
#include "midi2.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
#include "ump/ump.h"
#include "platform/platform.h"
#include "instrument/ui.h"
#include "midici/ci_identity.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "midici/responder.h"
#if E2E_HAS_M1 || E2E_HAS_M3
#include "midici/midici.h"
#endif
#if E2E_HAS_M3
#include "midici/initiator.h"
#endif
}

namespace e2e {

struct TestCase {
	int tier;
	const char* id;
	const char* name;
	void (*fn)();
};

struct TestRegistrar {
	TestRegistrar(int tier, const char* id, const char* name, void (*fn)());
};

const std::vector<TestCase>& registry();
int test_failure_count();
void reset_failure_count();

namespace detail {

void record_failure(const char* expr, const char* file, int line, const std::string& msg);

inline void check_bool(bool cond, const char* expr, const char* file, int line) {
	if (!cond) record_failure(expr, file, line, {});
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 5, 6)))
#endif
void check_bool_fmt(bool cond, const char* expr, const char* file, int line, const char* fmt, ...);

template <typename T>
struct is_nonbool_integral
    : std::bool_constant<std::is_integral_v<T> && !std::is_same_v<std::decay_t<T>, bool>> {};

template <typename A, typename B>
bool values_equal(const A& a, const B& b) {
	using DA = std::decay_t<A>;
	using DB = std::decay_t<B>;
	if constexpr (is_nonbool_integral<DA>::value && is_nonbool_integral<DB>::value) {
		return std::cmp_equal(a, b);
	} else if constexpr (std::is_enum_v<DA> && is_nonbool_integral<DB>::value) {
		return std::cmp_equal(static_cast<std::underlying_type_t<DA>>(a), b);
	} else if constexpr (is_nonbool_integral<DA>::value && std::is_enum_v<DB>) {
		return std::cmp_equal(a, static_cast<std::underlying_type_t<DB>>(b));
	} else {
		return a == b;
	}
}

template <typename T>
std::string format_val(const T& v) {
	using DT = std::decay_t<T>;
	std::ostringstream oss;
	if constexpr (std::is_same_v<DT, bool>) {
		oss << (v ? "true" : "false");
	} else if constexpr (std::is_same_v<DT, char> || std::is_same_v<DT, uint8_t> ||
	                     std::is_same_v<DT, int8_t> || std::is_same_v<DT, signed char> ||
	                     std::is_same_v<DT, unsigned char>) {
		oss << static_cast<int>(v);
	} else if constexpr (std::is_enum_v<DT>) {
		using U = std::underlying_type_t<DT>;
		if constexpr (std::is_same_v<U, char> || std::is_same_v<U, uint8_t> ||
		              std::is_same_v<U, int8_t> || std::is_same_v<U, signed char> ||
		              std::is_same_v<U, unsigned char>) {
			oss << static_cast<int>(static_cast<U>(v));
		} else {
			oss << static_cast<U>(v);
		}
	} else {
		oss << v;
	}
	return oss.str();
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file,
              int line) {
	if (!values_equal(a, b)) {
		record_failure((std::string(exprA) + " == " + exprB).c_str(), file, line,
		               "got " + format_val(a) + ", expected " + format_val(b));
	}
}

}  // namespace detail

class VirtualClock {
public:
	uint32_t now_ms() const noexcept { return now_ms_; }
	void advance_ms(uint32_t ms, const std::function<void()>& on_tick = {});
	void advance_and_poll(uint32_t ms);

private:
	uint32_t now_ms_ = 0;
};

VirtualClock& global_clock();

class FirmwareNode {
public:
	VirtualClock& clock() noexcept { return global_clock(); }
	const VirtualClock& clock() const noexcept { return global_clock(); }

	bool host_in_auto_ack = true;
	std::vector<m2s::UmpPacket> tx_history;

	void set_serial(const char* serial);
	void set_entropy(const uint32_t* seq, int count);

	void boot(bool usb_configured = true, uint8_t alt = 1);
	void set_usb_configured(bool configured);
	void set_alt(uint8_t alt);

	void poll_once();
	void poll(int passes = 1);
	void advance_ms(uint32_t ms);

	void press_pad(uint8_t index, uint8_t value = 127);
	void release_pad(uint8_t index);
	void tap_pad(uint8_t index, uint32_t hold_ms = 20);
	void press_setup(uint8_t value = 127);
	void release_setup();
	void tap_setup(uint32_t hold_ms = 50);

	void send_ump(const uint32_t* words, uint16_t nwords);
	std::vector<uint32_t> drain_ump();
	std::vector<m2s::UmpPacket> drain_tx_packets(uint16_t source_id = 10);

	void negotiate_ump_stream(uint8_t protocol = UMP_PROTO_MIDI2);

#if E2E_HAS_M1
	int send_sysex(uint8_t group, const uint8_t* data, uint16_t len);
	std::vector<std::array<uint32_t, 2>> packetize_sysex7(uint8_t group, const uint8_t* payload,
	                                                      uint16_t len);
	uint32_t reassembly_drops() const;
	uint32_t muid() const;
#endif

#if E2E_HAS_M3
	void trigger_negotiated_discovery();
	void start_discovery();
	uint8_t peer_count() const;
	bool get_peer(uint8_t idx, ci_peer_info_t* out) const;
#endif

private:
	std::optional<std::string> serial_;
	std::vector<uint32_t> entropy_;
};

struct WireConfig {
	m2s::ci::CiIdentity agent_identity{};
	uint32_t agent_muid = 0x0123456u;
	uint16_t fw_source_id = 10;
	bool connected = true;
	bool self_echo_to_fw = false;
	bool self_echo_to_agent = false;
};

class WirePair {
public:
	explicit WirePair(FirmwareNode& fw, const WireConfig& cfg = {});
	WirePair(FirmwareNode& fw, const m2s::ci::CiIdentity& host_id,
	         uint32_t host_muid = 0x0123456u, uint16_t fw_source_id = 10);

	m2s::ci::CiAgent& agent() noexcept { return agent_; }
	const m2s::ci::CiAgent& agent() const noexcept { return agent_; }

	void set_connected(bool connected) noexcept { connected_ = connected; }
	bool connected() const noexcept { return connected_; }

	bool pump();
	bool settle(uint32_t max_cycles = 256);

	bool discover_from_agent(std::wstring* err = nullptr);
	void advance_ms_and_settle(uint32_t ms);

	void inject_ump_to_fw(const uint32_t* words, size_t word_count);
	void inject_sysex7_to_fw(uint8_t group, const uint8_t* payload, uint16_t len);
	void inject_sysex7_to_agent(uint8_t group, const uint8_t* payload, uint16_t len);
	void echo_to_fw(uint8_t group, const uint8_t* payload, uint16_t len);

	std::optional<m2s::CiPeer> find_agent_peer(uint32_t muid) const;

	std::vector<m2s::UmpPacket> agent_echoes;
	std::function<void(std::vector<uint32_t>&)> tx_agent_to_fw_filter;
	std::function<void(std::vector<m2s::UmpPacket>&)> tx_fw_to_agent_filter;

private:
	FirmwareNode& fw_;
	uint16_t fw_source_id_;
	bool connected_;
	bool self_echo_to_fw_;
	bool self_echo_to_agent_;
	std::deque<uint32_t> to_fw_words_;
	m2s::ci::CiAgent agent_;
};

std::array<uint32_t, 4> make_ep_discovery(uint8_t filter = 0x1F, uint8_t ver_major = 1,
                                          uint8_t ver_minor = 1);
std::array<uint32_t, 4> make_fb_discovery(uint8_t fb = 0xFF, uint8_t filter = 0x03);
std::array<uint32_t, 4> make_stream_config_req(uint8_t protocol = UMP_PROTO_MIDI2,
                                               uint8_t jr_flags = 0);
std::vector<uint32_t> pack_sysex7_words(uint8_t group, const std::vector<uint8_t>& payload);
std::vector<m2s::UmpPacket> pack_sysex7_umps(uint8_t group, uint16_t source,
                                             const std::vector<uint8_t>& payload);
bool reassemble_sysex7(const std::vector<m2s::UmpPacket>& pkts, uint8_t group,
                       std::vector<uint8_t>& out, uint64_t* drops = nullptr);

}  // namespace e2e

#define E2E_CAT_IMPL(a, b) a##b
#define E2E_CAT(a, b) E2E_CAT_IMPL(a, b)

#define E2E_TEST(tier, id, name)                                                           \
	static void E2E_CAT(e2e_test_fn_, __LINE__)();                                         \
	static const ::e2e::TestRegistrar E2E_CAT(e2e_test_reg_, __LINE__)(                    \
	    (tier), (id), (name), &E2E_CAT(e2e_test_fn_, __LINE__));                           \
	static void E2E_CAT(e2e_test_fn_, __LINE__)()

#define E2E_CHECK(cond, ...)                                                               \
	do {                                                                                   \
		__VA_OPT__(::e2e::detail::check_bool_fmt(static_cast<bool>(cond), #cond, __FILE__, \
		                                         __LINE__, __VA_ARGS__);)                  \
		if constexpr (sizeof(#__VA_ARGS__) == 1) {                                         \
			::e2e::detail::check_bool(static_cast<bool>(cond), #cond, __FILE__, __LINE__); \
		}                                                                                  \
	} while (0)

#define E2E_CHECK_EQ(a, b) \
	::e2e::detail::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
