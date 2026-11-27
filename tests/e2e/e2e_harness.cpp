#include "e2e_harness.h"

#include <cstdarg>
#include <cstdio>

namespace e2e {

namespace {

std::vector<TestCase>& mutable_registry() {
	static std::vector<TestCase> tests;
	return tests;
}

int g_failures = 0;

}  // namespace

TestRegistrar::TestRegistrar(int tier, const char* id, const char* name, void (*fn)()) {
	mutable_registry().push_back(TestCase{tier, id, name, fn});
}

const std::vector<TestCase>& registry() {
	return mutable_registry();
}

int test_failure_count() {
	return g_failures;
}

void reset_failure_count() {
	g_failures = 0;
}

namespace detail {

void record_failure(const char* expr, const char* file, int line, const std::string& msg) {
	++g_failures;
	if (msg.empty()) {
		std::fprintf(stderr, "  ASSERTION FAILED (%s:%d): %s\n", file, line, expr);
	} else {
		std::fprintf(stderr, "  ASSERTION FAILED (%s:%d): %s [%s]\n", file, line, expr,
		             msg.c_str());
	}
}

void check_bool_fmt(bool cond, const char* expr, const char* file, int line, const char* fmt, ...) {
	if (cond) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	record_failure(expr, file, line, buf);
}

}  // namespace detail

VirtualClock& global_clock() {
	static VirtualClock clk;
	return clk;
}

void VirtualClock::advance_ms(uint32_t ms, const std::function<void()>& on_tick) {
	for (uint32_t i = 0; i < ms; ++i) {
		app_timer_event();
		++now_ms_;
		if (on_tick) on_tick();
	}
}

void VirtualClock::advance_and_poll(uint32_t ms) {
	advance_ms(ms, []() {
		if (e2e_ep1_tx_is_valid()) e2e_ep1_tx_ack();
		__wrap_midi_parsers_poll();
		__wrap_usbmidi_poll();
	});
}

void FirmwareNode::set_serial(const char* serial) {
	if (serial) {
		serial_ = std::string(serial);
	} else {
		serial_.reset();
	}
	e2e_set_serial(serial);
}

void FirmwareNode::set_entropy(const uint32_t* seq, int count) {
	if (seq && count > 0) {
		entropy_.assign(seq, seq + count);
	} else {
		entropy_.clear();
	}
	e2e_set_entropy(seq, count);
}

void FirmwareNode::boot(bool usb_configured, uint8_t alt) {
	e2e_stubs_reset();
	if (serial_.has_value()) {
		e2e_set_serial(serial_->c_str());
	}
	if (!entropy_.empty()) {
		e2e_set_entropy(entropy_.data(), static_cast<int>(entropy_.size()));
	}
	ump_rx_reset();
	ump_tx_reset();
	ump_stream_reset();
	__wrap_midi_parsers_init();
	app_init(e2e_adc);
	set_usb_configured(usb_configured);
	usb_midi2_set_alt(alt);
	usb_midi2_service();
	tx_history.clear();
}

void FirmwareNode::set_usb_configured(bool configured) {
	e2e_set_device_state(configured ? USB_STATE_CONFIGURED : USB_STATE_UNCONNECTED);
}

void FirmwareNode::set_alt(uint8_t alt) {
	usb_midi2_set_alt(alt);
}

void FirmwareNode::poll_once() {
	if (host_in_auto_ack && e2e_ep1_tx_is_valid()) {
		e2e_ep1_tx_ack();
	}
	__wrap_midi_parsers_poll();
	__wrap_usbmidi_poll();
}

void FirmwareNode::poll(int passes) {
	for (int i = 0; i < passes; ++i) {
		poll_once();
	}
}

void FirmwareNode::advance_ms(uint32_t ms) {
	clock().advance_ms(ms, [this]() { poll_once(); });
}

void FirmwareNode::press_pad(uint8_t index, uint8_t value) {
	app_surface_event(TYPEPAD, index, value);
}

void FirmwareNode::release_pad(uint8_t index) {
	app_surface_event(TYPEPAD, index, 0);
}

void FirmwareNode::tap_pad(uint8_t index, uint32_t hold_ms) {
	press_pad(index, 127);
	advance_ms(hold_ms);
	release_pad(index);
}

void FirmwareNode::press_setup(uint8_t value) {
	app_surface_event(TYPESETUP, 0, value);
}

void FirmwareNode::release_setup() {
	app_surface_event(TYPESETUP, 0, 0);
}

void FirmwareNode::tap_setup(uint32_t hold_ms) {
	press_setup(127);
	advance_ms(hold_ms);
	release_setup();
}

void FirmwareNode::send_ump(const uint32_t* words, uint16_t nwords) {
	e2e_host_send_ump(words, nwords);
}

std::vector<uint32_t> FirmwareNode::drain_ump() {
	std::vector<uint32_t> out;
	const uint16_t n = e2e_tx_count();
	if (n > 0) {
		out.resize(n);
		const uint16_t popped = e2e_tx_pop_ump(out.data(), n);
		out.resize(popped);
	}
	return out;
}

std::vector<m2s::UmpPacket> FirmwareNode::drain_tx_packets(uint16_t source_id) {
	std::vector<m2s::UmpPacket> pkts;
	const std::vector<uint32_t> words = drain_ump();
	size_t pos = 0;
	while (pos < words.size()) {
		const uint8_t wc = m2s::umpWordCountForType(static_cast<uint8_t>(words[pos] >> 28));
		if (wc == 0 || pos + wc > words.size()) break;
		m2s::UmpPacket p{};
		for (uint8_t i = 0; i < wc; ++i) {
			p.words[i] = words[pos + i];
		}
		p.wordCount = wc;
		p.source = source_id;
		pkts.push_back(p);
		tx_history.push_back(p);
		pos += wc;
	}
	return pkts;
}

void FirmwareNode::negotiate_ump_stream(uint8_t protocol) {
	const auto ep = make_ep_discovery(0x1Fu);
	const auto fb = make_fb_discovery(0xFFu, 0x03u);
	const auto cfg = make_stream_config_req(protocol, 0u);
	send_ump(ep.data(), static_cast<uint16_t>(ep.size()));
	poll(4);
	send_ump(fb.data(), static_cast<uint16_t>(fb.size()));
	poll(4);
	send_ump(cfg.data(), static_cast<uint16_t>(cfg.size()));
	poll(4);
}

#if E2E_HAS_M1
int FirmwareNode::send_sysex(uint8_t group, const uint8_t* data, uint16_t len) {
	return midici_send_sysex(group, data, len);
}

std::vector<std::array<uint32_t, 2>> FirmwareNode::packetize_sysex7(uint8_t group,
                                                                     const uint8_t* payload,
                                                                     uint16_t len) {
	const uint16_t packets = len ? static_cast<uint16_t>((len + 5u) / 6u) : 1u;
	std::vector<std::array<uint32_t, 2>> out(packets);
	for (uint16_t p = 0; p < packets; ++p) {
		ump_sysex7_packet(out[p].data(), group, payload, len, p, packets);
	}
	return out;
}

uint32_t FirmwareNode::reassembly_drops() const {
	return midici_reassembly_drops();
}

uint32_t FirmwareNode::muid() const {
	return midici_muid();
}
#endif

#if E2E_HAS_M3
void FirmwareNode::trigger_negotiated_discovery() {
	midici_on_ump_negotiated(clock().now_ms());
}

void FirmwareNode::start_discovery() {
	midici_initiator_start_discovery(clock().now_ms());
}

uint8_t FirmwareNode::peer_count() const {
	return midici_initiator_peer_count();
}

bool FirmwareNode::get_peer(uint8_t idx, ci_peer_info_t* out) const {
	return midici_initiator_get_peer(idx, out);
}
#endif

WirePair::WirePair(FirmwareNode& fw, const WireConfig& cfg)
    : fw_(fw),
      fw_source_id_(cfg.fw_source_id),
      connected_(cfg.connected),
      self_echo_to_fw_(cfg.self_echo_to_fw),
      self_echo_to_agent_(cfg.self_echo_to_agent),
      agent_(
          cfg.agent_identity,
          [this](uint16_t source, const uint32_t* words, size_t count) -> bool {
            if (!connected_ || source != fw_source_id_ || !words || count == 0) return false;
            std::vector<uint32_t> vec(words, words + count);
            if (tx_agent_to_fw_filter) tx_agent_to_fw_filter(vec);
            for (uint32_t w : vec) {
              to_fw_words_.push_back(w);
            }
            if (self_echo_to_agent_) {
              for (size_t pos = 0; pos + 1 < vec.size(); pos += 2) {
                m2s::UmpPacket echoPkt{};
                echoPkt.words[0] = vec[pos];
                echoPkt.words[1] = vec[pos + 1];
                echoPkt.wordCount = 2;
                echoPkt.source = fw_source_id_;
                agent_.receive(echoPkt);
              }
            }
            return true;
          },
          [this](const m2s::UmpPacket& p) { agent_echoes.push_back(p); },
          [this]() -> std::vector<uint16_t> {
            if (!connected_) return {};
            return {fw_source_id_};
          },
          cfg.agent_muid) {}

WirePair::WirePair(FirmwareNode& fw, const m2s::ci::CiIdentity& host_id, uint32_t host_muid,
                   uint16_t fw_source_id)
    : WirePair(fw, WireConfig{host_id, host_muid, fw_source_id, true, false, false}) {}

bool WirePair::pump() {
	bool did_work = false;

	if (!to_fw_words_.empty() && e2e_ep1_rx_is_valid()) {
		std::vector<uint32_t> chunk;
		chunk.reserve(16);
		while (!to_fw_words_.empty() && chunk.size() < 16) {
			const uint8_t wc = ump_word_count(to_fw_words_.front());
			const size_t msg_words = (wc >= 1 && wc <= 4 && to_fw_words_.size() >= wc) ? wc : 1;
			if (!chunk.empty() && chunk.size() + msg_words > 16) break;
			for (size_t i = 0; i < msg_words && !to_fw_words_.empty() && chunk.size() < 16; ++i) {
				chunk.push_back(to_fw_words_.front());
				to_fw_words_.pop_front();
			}
		}
		if (!chunk.empty()) {
			e2e_host_send_ump(chunk.data(), static_cast<uint16_t>(chunk.size()));
			did_work = true;
		}
	}

	const bool tx_pending_before = e2e_ep1_tx_is_valid() || e2e_tx_count() > 0;
	fw_.poll_once();
	if (tx_pending_before) did_work = true;

	std::vector<m2s::UmpPacket> pkts = fw_.drain_tx_packets(fw_source_id_);
	if (!pkts.empty()) {
		did_work = true;
		if (tx_fw_to_agent_filter) tx_fw_to_agent_filter(pkts);
		for (const auto& p : pkts) {
			agent_.receive(p);
			if (self_echo_to_fw_ && m2s::umpType(p) == 0x3u) {
				for (uint8_t i = 0; i < p.wordCount; ++i) {
					to_fw_words_.push_back(p.words[i]);
				}
			}
		}
	}

	if (agent_.pump() > 0) {
		did_work = true;
	}

	return did_work;
}

bool WirePair::settle(uint32_t max_cycles) {
	if (max_cycles == 0) return false;
	uint32_t idle_streak = 0;
	for (uint32_t cycle = 0; cycle < max_cycles; ++cycle) {
		const bool active = pump();
		if (!active && to_fw_words_.empty() && e2e_tx_count() == 0 && !e2e_ep1_tx_is_valid()) {
			++idle_streak;
			if (idle_streak >= 2) return true;
		} else {
			idle_streak = 0;
		}
	}
	return false;
}

bool WirePair::discover_from_agent(std::wstring* err) {
	if (!agent_.discover(err)) return false;
	return settle();
}

void WirePair::advance_ms_and_settle(uint32_t ms) {
	fw_.clock().advance_ms(ms, [this]() { pump(); });
	settle();
}

void WirePair::inject_ump_to_fw(const uint32_t* words, size_t word_count) {
	if (!words || word_count == 0) return;
	for (size_t i = 0; i < word_count; ++i) {
		to_fw_words_.push_back(words[i]);
	}
}

void WirePair::inject_sysex7_to_fw(uint8_t group, const uint8_t* payload, uint16_t len) {
	const std::vector<uint8_t> vec =
	    (payload && len > 0) ? std::vector<uint8_t>(payload, payload + len) : std::vector<uint8_t>{};
	const std::vector<uint32_t> words = pack_sysex7_words(group, vec);
	inject_ump_to_fw(words.data(), words.size());
}

void WirePair::inject_sysex7_to_agent(uint8_t group, const uint8_t* payload, uint16_t len) {
	const std::vector<uint8_t> vec =
	    (payload && len > 0) ? std::vector<uint8_t>(payload, payload + len) : std::vector<uint8_t>{};
	for (const auto& pkt : pack_sysex7_umps(group, fw_source_id_, vec)) {
		agent_.receive(pkt);
	}
}

void WirePair::echo_to_fw(uint8_t group, const uint8_t* payload, uint16_t len) {
	inject_sysex7_to_fw(group, payload, len);
}

std::optional<m2s::CiPeer> WirePair::find_agent_peer(uint32_t muid) const {
	const m2s::CiSnapshot snap = agent_.snapshot();
	for (const auto& peer : snap.peers) {
		if (peer.muid == muid) return peer;
	}
	return std::nullopt;
}

std::array<uint32_t, 4> make_ep_discovery(uint8_t filter, uint8_t ver_major, uint8_t ver_minor) {
	const uint32_t w0 = 0xF0000000u | (static_cast<uint32_t>(ver_major) << 8) | ver_minor;
	return {w0, static_cast<uint32_t>(filter), 0u, 0u};
}

std::array<uint32_t, 4> make_fb_discovery(uint8_t fb, uint8_t filter) {
	const uint32_t w0 = 0xF0100000u | (static_cast<uint32_t>(fb) << 8) | filter;
	return {w0, 0u, 0u, 0u};
}

std::array<uint32_t, 4> make_stream_config_req(uint8_t protocol, uint8_t jr_flags) {
	const uint32_t w0 = 0xF0050000u | (static_cast<uint32_t>(protocol) << 8) | jr_flags;
	return {w0, 0u, 0u, 0u};
}

std::vector<uint32_t> pack_sysex7_words(uint8_t group, const std::vector<uint8_t>& payload) {
	const auto pkts = m2s::ci::packetizeSysEx7(group, payload.data(), payload.size());
	std::vector<uint32_t> words;
	words.reserve(pkts.size() * 2);
	for (const auto& p : pkts) {
		words.push_back(p[0]);
		words.push_back(p[1]);
	}
	return words;
}

std::vector<m2s::UmpPacket> pack_sysex7_umps(uint8_t group, uint16_t source,
                                             const std::vector<uint8_t>& payload) {
	const auto pkts = m2s::ci::packetizeSysEx7(group, payload.data(), payload.size());
	std::vector<m2s::UmpPacket> out;
	out.reserve(pkts.size());
	for (const auto& p : pkts) {
		m2s::UmpPacket u{};
		u.words[0] = p[0];
		u.words[1] = p[1];
		u.wordCount = 2;
		u.source = source;
		out.push_back(u);
	}
	return out;
}

bool reassemble_sysex7(const std::vector<m2s::UmpPacket>& pkts, uint8_t group,
                       std::vector<uint8_t>& out, uint64_t* drops) {
	m2s::ci::SysEx7Assembler asmblr;
	for (const auto& p : pkts) {
		if (m2s::umpType(p) != 0x3u || m2s::umpGroup(p) != (group & 0xFu)) continue;
		if (asmblr.feed(p, out)) {
			if (drops) *drops = asmblr.dropped();
			return true;
		}
	}
	if (drops) *drops = asmblr.dropped();
	return false;
}

}  // namespace e2e
