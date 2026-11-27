#include "midici/initiator.h"
#include "midici/ci_identity.h"
#include "vendor/MidiCiMessage.h"

#include <string.h>

extern "C" {
#include "midici/ci_json.h"
#include "midici/midici.h"
}

namespace ms = WindowsMidiServicesCapabilityInquiry;

namespace {

using T = ms::MessageType;

constexpr uint8_t kStandaloneGroup = 0;
constexpr uint32_t kProfileReplyTimeoutMs = 3000u;

constexpr uint8_t TX_ENDPOINT = 0x01u;
constexpr uint8_t TX_PROFILE  = 0x02u;
constexpr uint8_t TX_PE_CAPS  = 0x04u;
constexpr uint8_t TX_PE_GET   = 0x08u;

constexpr uint8_t RX_ENDPOINT = 0x01u;
constexpr uint8_t RX_PROFILE  = 0x02u;
constexpr uint8_t RX_PE_CAPS  = 0x04u;
constexpr uint8_t RX_PE_GET   = 0x08u;
constexpr uint8_t RX_MASK     = 0x0Fu;

constexpr char kDeviceInfoHeader[] = "{\"resource\":\"DeviceInfo\"}";
constexpr uint16_t kDeviceInfoHeaderLen = sizeof(kDeviceInfoHeader) - 1u;

struct PendingGet {
	uint32_t peer_muid;
	uint32_t start_ms;
	uint16_t expect_chunk;
	uint16_t rx_len;
	uint16_t status;
	uint8_t req_id;
	bool active;
	bool overflow;
	uint8_t first_char;
	uint8_t last_char;
};

struct ProfileInquiryProgress {
	uint32_t last_reply_ms;
	uint32_t seen_scopes; /* bits 0..15: channels, bit 16: group, bit 17: Function Block */
	uint16_t enabled;
	uint16_t disabled;
};

ci_peer_info_t peers[CI_INITIATOR_MAX_PEERS];
uint8_t tx_pending[CI_INITIATOR_MAX_PEERS];
uint8_t rx_pending[CI_INITIATOR_MAX_PEERS];
ProfileInquiryProgress profile_progress[CI_INITIATOR_MAX_PEERS];
char pe_rx_buf[256];
PendingGet pe_get;

uint32_t window_start_ms;
uint32_t last_now_ms;
uint8_t peer_count;
uint8_t next_req_id;
bool discovering;
bool discovery_tx_pending;
bool auto_discovered;
bool echo_armed;
volatile bool reset_requested;

uint16_t packets_for(size_t len)
{
	return len ? static_cast<uint16_t>((len + 5u) / 6u) : 1u;
}

void reset_now()
{
	reset_requested = false;
	memset(peers, 0, sizeof(peers));
	memset(tx_pending, 0, sizeof(tx_pending));
	memset(rx_pending, 0, sizeof(rx_pending));
	memset(profile_progress, 0, sizeof(profile_progress));
	pe_get = {};
	window_start_ms = 0;
	peer_count = 0;
	next_req_id = 0;
	discovering = false;
	discovery_tx_pending = false;
	auto_discovered = false;
	echo_armed = false;
}

void check_reset()
{
	if (reset_requested)
		reset_now();
}

int find_peer(uint32_t muid)
{
	for (uint8_t i = 0; i < peer_count; i++) {
		if (peers[i].muid == muid)
			return static_cast<int>(i);
	}
	return -1;
}

uint8_t in_flight_mask(uint8_t idx)
{
	const uint8_t rx = static_cast<uint8_t>(rx_pending[idx] & RX_MASK);
	const uint8_t tx = tx_pending[idx];
	uint8_t mask = 0u;
	if ((rx & RX_ENDPOINT) && !(tx & TX_ENDPOINT))
		mask |= RX_ENDPOINT;
	if ((rx & RX_PROFILE) && !(tx & TX_PROFILE))
		mask |= RX_PROFILE;
	if ((rx & RX_PE_CAPS) && !(tx & TX_PE_CAPS))
		mask |= RX_PE_CAPS;
	if ((rx & RX_PE_GET) && !(rx & RX_PE_CAPS) && !(tx & TX_PE_GET))
		mask |= RX_PE_GET;
	return mask;
}

uint8_t popcount4(uint8_t m)
{
	return static_cast<uint8_t>(
		((m & 0x01u) ? 1u : 0u) +
		((m & 0x02u) ? 1u : 0u) +
		((m & 0x04u) ? 1u : 0u) +
		((m & 0x08u) ? 1u : 0u));
}

void resolve_bare_naks(uint8_t idx)
{
	uint8_t rx = static_cast<uint8_t>(rx_pending[idx] & RX_MASK);
	if (rx == 0u) {
		rx_pending[idx] = 0u;
		return;
	}
	uint8_t bare = static_cast<uint8_t>((rx_pending[idx] >> 4u) & 0x0Fu);
	if (bare == 0u)
		return;
	const uint8_t inflight = in_flight_mask(idx);
	const uint8_t u = popcount4(inflight);
	if (u > 0u && bare >= u) {
		rx = static_cast<uint8_t>(rx & ~inflight);
		if (inflight & RX_PE_CAPS)
			rx = static_cast<uint8_t>(rx & ~RX_PE_GET);
		if ((inflight & RX_PE_GET) && pe_get.active && pe_get.peer_muid == peers[idx].muid)
			pe_get.active = false;
		bare = (rx == 0u) ? 0u : static_cast<uint8_t>(bare - u);
		rx_pending[idx] = static_cast<uint8_t>(rx | (bare << 4u));
	}
}

void update_complete(uint8_t idx)
{
	if ((rx_pending[idx] & RX_MASK) == 0u) {
		rx_pending[idx] = 0u;
		peers[idx].flags |= CI_PEER_FLAG_COMPLETE;
	}
}

void remove_peer(uint8_t idx)
{
	if (idx >= peer_count)
		return;
	if (pe_get.active && pe_get.peer_muid == peers[idx].muid)
		pe_get.active = false;
	for (uint8_t i = idx; static_cast<uint8_t>(i + 1u) < peer_count; i++) {
		peers[i] = peers[i + 1u];
		tx_pending[i] = tx_pending[i + 1u];
		rx_pending[i] = rx_pending[i + 1u];
		profile_progress[i] = profile_progress[i + 1u];
	}
	peer_count--;
	peers[peer_count] = {};
	tx_pending[peer_count] = 0;
	rx_pending[peer_count] = 0;
	profile_progress[peer_count] = {};
}

size_t build_discovery_msg(uint8_t *buf, size_t cap, uint32_t my_muid)
{
	ms::DiscoveryReplyFields f{};
	f.SourceMuid = my_muid;
	f.DestinationMuid = ms::MuidBroadcast;
	f.ManufacturerSysExId[0] = CI_MANUFACTURER_0;
	f.ManufacturerSysExId[1] = CI_MANUFACTURER_1;
	f.ManufacturerSysExId[2] = CI_MANUFACTURER_2;
	f.DeviceFamily = CI_FAMILY;
	f.DeviceFamilyModelNumber = CI_MODEL;
	f.SoftwareRevisionLevel[0] = CI_SW_REV_0;
	f.SoftwareRevisionLevel[1] = CI_SW_REV_1;
	f.SoftwareRevisionLevel[2] = CI_SW_REV_2;
	f.SoftwareRevisionLevel[3] = CI_SW_REV_3;
	f.CapabilityCategories = ms::CategoryPropertyExchange;
	f.ReceivableMaximumSysExSize = CI_REASSEMBLY_BYTES;
	f.OutputPathId = 0;
	f.MessageVersion = ms::MessageVersionCurrent;
	return ms::BuildDiscovery(f, buf, cap);
}

bool try_send_discovery()
{
	uint16_t cap = 0;
	uint8_t *buf = midici_reply_buffer(&cap);
	const size_t n = build_discovery_msg(buf, cap, midici_muid());
	if (!n)
		return false;
	if (midici_tx_free_packets() < packets_for(n))
		return false;
	if (midici_send_sysex(kStandaloneGroup, buf, static_cast<uint16_t>(n)) != 0)
		return false;
	discovery_tx_pending = false;
	echo_armed = true;
	return true;
}

bool try_send_followup(uint8_t idx, uint8_t bit)
{
	uint16_t cap = 0;
	uint8_t *buf = midici_reply_buffer(&cap);
	const uint32_t my_muid = midici_muid();
	const uint32_t dst = peers[idx].muid;
	size_t n = 0;

	if (bit == TX_ENDPOINT) {
		n = ms::BuildEndpointInquiry(my_muid, dst, ms::EndpointStatusProductInstanceId, buf, cap);
	} else if (bit == TX_PROFILE) {
		ms::ProfileMessageFields pf{};
		pf.Type = T::ProfileInquiry;
		pf.DeviceId = ms::DeviceIdFunctionBlock;
		pf.SourceMuid = my_muid;
		pf.DestinationMuid = dst;
		pf.MessageVersion = ms::MessageVersionCurrent;
		n = ms::BuildProfileMessage(pf, buf, cap);
	} else if (bit == TX_PE_CAPS) {
		n = ms::WriteCommonHeader(buf, cap, ms::DeviceIdFunctionBlock,
		                          T::PropertyExchangeCapabilitiesInquiry,
		                          my_muid, dst, ms::MessageVersionCurrent);
		if (n && cap >= n + 3u) {
			buf[n++] = 1u; /* simultaneousRequests */
			buf[n++] = ms::PropertyExchangeMajorVersion;
			buf[n++] = ms::PropertyExchangeMinorVersion;
		} else {
			n = 0;
		}
	}

	if (!n)
		return false;
	if (midici_tx_free_packets() < packets_for(n))
		return false;
	if (midici_send_sysex(kStandaloneGroup, buf, static_cast<uint16_t>(n)) != 0)
		return false;
	if (bit == TX_PROFILE)
		profile_progress[idx].last_reply_ms = last_now_ms;
	tx_pending[idx] &= static_cast<uint8_t>(~bit);
	return true;
}

bool try_send_pe_get(uint8_t idx, uint32_t now_ms)
{
	uint16_t cap = 0;
	uint8_t *buf = midici_reply_buffer(&cap);
	const uint32_t my_muid = midici_muid();
	const uint8_t rid = static_cast<uint8_t>((next_req_id % 127u) + 1u);

	ms::PropertyExchangeMessageFields f{};
	f.Type = T::PropertyGetDataInquiry;
	f.SourceMuid = my_muid;
	f.DestinationMuid = peers[idx].muid;
	f.RequestId = rid;
	f.Header = reinterpret_cast<const uint8_t *>(kDeviceInfoHeader);
	f.HeaderByteCount = kDeviceInfoHeaderLen;
	f.ChunkCount = 1u;
	f.ChunkNumber = 1u;
	f.Data = nullptr;
	f.DataByteCount = 0u;

	const size_t n = ms::BuildPropertyExchangeMessage(f, buf, cap);
	if (!n)
		return false;
	if (midici_tx_free_packets() < packets_for(n))
		return false;
	if (midici_send_sysex(kStandaloneGroup, buf, static_cast<uint16_t>(n)) != 0)
		return false;

	next_req_id = rid;
	tx_pending[idx] &= static_cast<uint8_t>(~TX_PE_GET);
	pe_get = {};
	pe_get.peer_muid = peers[idx].muid;
	pe_get.start_ms = now_ms;
	pe_get.expect_chunk = 1u;
	pe_get.req_id = rid;
	pe_get.active = true;
	return true;
}

void close_window_if_expired(uint32_t now_ms)
{
	if (discovering && static_cast<uint32_t>(now_ms - window_start_ms) >= CI_DISCOVERY_WINDOW_MS) {
		discovering = false;
		discovery_tx_pending = false;
	}
}

void handle_discovery_reply(const ms::ParsedMessage &m, const uint8_t *payload, uint16_t len)
{
	if (!discovering || m.DeviceId != ms::DeviceIdFunctionBlock)
		return;
	const uint8_t ver = m.VersionFormat;
	if (ver == 0u || (ver & 0x70u) != 0u)
		return;
	const size_t min_len = (ver == ms::MessageVersion11)
		? ms::DiscoveryReplyByteCountVersion11
		: ms::DiscoveryReplyByteCount;
	if (len < min_len)
		return;

	int idx_i = find_peer(m.SourceMuid);
	bool is_new = false;
	if (idx_i < 0) {
		if (peer_count >= CI_INITIATOR_MAX_PEERS)
			return;
		idx_i = peer_count++;
		peers[idx_i] = {};
		tx_pending[idx_i] = 0;
		rx_pending[idx_i] = 0;
		is_new = true;
	}

	const uint8_t idx = static_cast<uint8_t>(idx_i);
	ci_peer_info_t &p = peers[idx];
	const size_t o = ms::CommonHeaderByteCount;

	p.muid = m.SourceMuid;
	p.manufacturer[0] = payload[o + 0] & 0x7Fu;
	p.manufacturer[1] = payload[o + 1] & 0x7Fu;
	p.manufacturer[2] = payload[o + 2] & 0x7Fu;
	p.family = ms::ReadFourteenBitValue(payload + o + 3);
	p.model = ms::ReadFourteenBitValue(payload + o + 5);
	p.sw_rev[0] = payload[o + 7] & 0x7Fu;
	p.sw_rev[1] = payload[o + 8] & 0x7Fu;
	p.sw_rev[2] = payload[o + 9] & 0x7Fu;
	p.sw_rev[3] = payload[o + 10] & 0x7Fu;
	p.categories = payload[o + 11] & 0x7Fu;
	p.max_sysex = ms::ReadMuid(payload + o + 12);
	p.version = ver;
	p.function_block = (ver >= ms::MessageVersion12 && len >= ms::DiscoveryReplyByteCount)
		? static_cast<uint8_t>(payload[30] & 0x7Fu)
		: 0x7Fu;

	if (is_new || !(p.flags & CI_PEER_FLAG_DISCOVERED)) {
		p.flags |= CI_PEER_FLAG_DISCOVERED;
		tx_pending[idx] |= TX_ENDPOINT;
		rx_pending[idx] |= RX_ENDPOINT;
		if (p.categories & ms::CategoryProfileConfiguration) {
			tx_pending[idx] |= TX_PROFILE;
			rx_pending[idx] |= RX_PROFILE;
		}
		if (p.categories & ms::CategoryPropertyExchange) {
			tx_pending[idx] |= TX_PE_CAPS;
			rx_pending[idx] |= static_cast<uint8_t>(RX_PE_CAPS | RX_PE_GET);
		}
	}
}

void handle_endpoint_reply(const ms::ParsedMessage &m, const uint8_t *payload)
{
	if (m.DeviceId != ms::DeviceIdFunctionBlock || !m.HasEndpointFields)
		return;
	if (m.Endpoint.Status != ms::EndpointStatusProductInstanceId)
		return;
	const int idx_i = find_peer(m.SourceMuid);
	if (idx_i < 0)
		return;
	const uint8_t idx = static_cast<uint8_t>(idx_i);
	ci_peer_info_t &p = peers[idx];

	uint16_t n = m.Endpoint.InformationByteCount;
	if (n >= CI_PEER_PIID_BUF_BYTES)
		n = CI_PEER_PIID_BUF_BYTES - 1u;
	for (uint16_t k = 0; k < n; k++)
		p.product_instance_id[k] = static_cast<char>(payload[m.Endpoint.InformationOffset + k] & 0x7Fu);
	p.product_instance_id[n] = '\0';

	p.flags |= CI_PEER_FLAG_ENDPOINT;
	rx_pending[idx] &= static_cast<uint8_t>(~RX_ENDPOINT);
	resolve_bare_naks(idx);
	update_complete(idx);
}

void finish_profile_inquiry(uint8_t idx)
{
	rx_pending[idx] &= static_cast<uint8_t>(~RX_PROFILE);
	resolve_bare_naks(idx);
	update_complete(idx);
}

void handle_profile_reply(const ms::ParsedMessage &m, uint32_t now_ms)
{
	if (!m.HasProfileFields || m.VersionFormat == 0u || (m.VersionFormat & 0x70u))
		return;
	const int idx_i = find_peer(m.SourceMuid);
	if (idx_i < 0)
		return;
	const uint8_t idx = static_cast<uint8_t>(idx_i);
	if (!(in_flight_mask(idx) & RX_PROFILE))
		return;

	/* M2-102-U §2.4: a Function Block inquiry gets one reply per reported
	 * channel and group, then a final 0x7F reply, even with empty lists. */
	const bool final_reply = m.DeviceId == ms::DeviceIdFunctionBlock;
	const uint8_t scope = m.DeviceId < 16u ? m.DeviceId : (final_reply ? 17u : 16u);
	const uint32_t scope_bit = uint32_t{1} << scope;
	ProfileInquiryProgress &progress = profile_progress[idx];
	if (progress.seen_scopes & scope_bit)
		return;
	progress.seen_scopes |= scope_bit;
	progress.last_reply_ms = now_ms;
	progress.enabled += m.Profile.EnabledProfileCount;
	progress.disabled += m.Profile.DisabledProfileCount;

	if (final_reply) {
		ci_peer_info_t &p = peers[idx];
		p.profiles_enabled = progress.enabled;
		p.profiles_disabled = progress.disabled;
		p.flags |= CI_PEER_FLAG_PROFILES;
		finish_profile_inquiry(idx);
	}
}

void handle_pe_caps_reply(const ms::ParsedMessage &m, const uint8_t *payload, uint16_t len)
{
	if (len < ms::PropertyExchangeCapabilitiesByteCountVersion11)
		return;
	const int idx_i = find_peer(m.SourceMuid);
	if (idx_i < 0)
		return;
	const uint8_t idx = static_cast<uint8_t>(idx_i);
	ci_peer_info_t &p = peers[idx];

	p.pe_simultaneous = payload[ms::CommonHeaderByteCount] & 0x7Fu;
	p.flags |= CI_PEER_FLAG_PE_CAPS;
	rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_CAPS);

	if (p.pe_simultaneous > 0u) {
		if ((rx_pending[idx] & RX_PE_GET) &&
		    !(p.flags & CI_PEER_FLAG_DEVICE_INFO) &&
		    !(tx_pending[idx] & TX_PE_GET) &&
		    !(pe_get.active && pe_get.peer_muid == p.muid)) {
			tx_pending[idx] |= TX_PE_GET;
		} else if (p.flags & CI_PEER_FLAG_DEVICE_INFO) {
			rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_GET);
		}
	} else {
		tx_pending[idx] &= static_cast<uint8_t>(~TX_PE_GET);
		rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_GET);
		if (pe_get.active && pe_get.peer_muid == p.muid)
			pe_get.active = false;
	}
	resolve_bare_naks(idx);
	update_complete(idx);
}

void fail_pe_get(uint8_t idx)
{
	peers[idx].flags |= CI_PEER_FLAG_NAK;
	rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_GET);
	pe_get.active = false;
	resolve_bare_naks(idx);
	update_complete(idx);
}

void handle_pe_get_reply(const ms::ParsedMessage &m, const uint8_t *payload)
{
	if (!m.HasPropertyExchangeFields || !pe_get.active)
		return;
	const auto &pe = m.PropertyExchange;
	if (m.SourceMuid != pe_get.peer_muid || pe.RequestId != pe_get.req_id)
		return;

	const int idx_i = find_peer(pe_get.peer_muid);
	if (idx_i < 0) {
		pe_get.active = false;
		return;
	}
	const uint8_t idx = static_cast<uint8_t>(idx_i);

	if (pe.ChunkCount == 0u || pe.ChunkNumber == 0u ||
	    pe.ChunkNumber != pe_get.expect_chunk || pe.ChunkNumber > pe.ChunkCount) {
		fail_pe_get(idx);
		return;
	}

	if (pe.ChunkNumber == 1u) {
		struct ci_json_it it;
		struct ci_json_member mem;
		int32_t status = -1;
		ci_json_it_init(&it, reinterpret_cast<const char *>(payload + pe.HeaderOffset),
		                pe.HeaderByteCount);
		int r;
		while ((r = ci_json_next(&it, &mem)) > 0) {
			if (ci_json_key_is(&mem, "status") && mem.type == CI_JSON_INT)
				status = mem.num;
		}
		if (r < 0 || status < 0 || status > 0xFFFF) {
			fail_pe_get(idx);
			return;
		}
		pe_get.status = static_cast<uint16_t>(status);
	}

	for (uint16_t k = 0; k < pe.DataByteCount; k++) {
		const uint8_t ch = payload[pe.DataOffset + k] & 0x7Fu;
		if (!pe_get.overflow) {
			if (pe_get.rx_len < sizeof(pe_rx_buf))
				pe_rx_buf[pe_get.rx_len++] = static_cast<char>(ch);
			else
				pe_get.overflow = true;
		}
		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
			if (pe_get.first_char == 0u)
				pe_get.first_char = ch;
			pe_get.last_char = ch;
		}
	}

	if (pe.ChunkNumber >= pe.ChunkCount) {
		peers[idx].device_info_status = pe_get.status;
		bool json_ok = false;
		if (!pe_get.overflow) {
			struct ci_json_it bit;
			struct ci_json_member bmem;
			ci_json_it_init(&bit, pe_rx_buf, pe_get.rx_len);
			int br;
			while ((br = ci_json_next(&bit, &bmem)) > 0) {}
			json_ok = (br == 0);
		} else {
			json_ok = (pe_get.first_char == '{' && pe_get.last_char == '}');
		}
		if (pe_get.status == 200u && json_ok)
			peers[idx].flags |= CI_PEER_FLAG_DEVICE_INFO;
		else
			peers[idx].flags |= CI_PEER_FLAG_NAK;

		rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_GET);
		pe_get.active = false;
		resolve_bare_naks(idx);
		update_complete(idx);
	} else {
		pe_get.expect_chunk++;
	}
}

void handle_nak(const ms::ParsedMessage &m)
{
	const int idx_i = find_peer(m.SourceMuid);
	if (idx_i < 0)
		return;
	const uint8_t idx = static_cast<uint8_t>(idx_i);
	if (m.HasAcknowledgmentFields &&
	    m.Acknowledgment.OriginalMessageType == static_cast<uint8_t>(T::ProfileInquiry) &&
	    (m.DeviceId != ms::DeviceIdFunctionBlock || !(in_flight_mask(idx) & RX_PROFILE)))
		return;
	peers[idx].flags |= CI_PEER_FLAG_NAK;

	if (m.HasAcknowledgmentFields) {
		switch (static_cast<T>(m.Acknowledgment.OriginalMessageType)) {
		case T::EndpointInquiry:
			rx_pending[idx] &= static_cast<uint8_t>(~RX_ENDPOINT);
			break;
		case T::ProfileInquiry:
			rx_pending[idx] &= static_cast<uint8_t>(~RX_PROFILE);
			break;
		case T::PropertyExchangeCapabilitiesInquiry:
			tx_pending[idx] &= static_cast<uint8_t>(~TX_PE_GET);
			rx_pending[idx] &= static_cast<uint8_t>(~(RX_PE_CAPS | RX_PE_GET));
			break;
		case T::PropertyGetDataInquiry:
			rx_pending[idx] &= static_cast<uint8_t>(~RX_PE_GET);
			if (pe_get.active && pe_get.peer_muid == peers[idx].muid)
				pe_get.active = false;
			break;
		default:
			break;
		}
	} else {
		const uint8_t u = popcount4(in_flight_mask(idx));
		const uint8_t rx = static_cast<uint8_t>(rx_pending[idx] & RX_MASK);
		uint8_t bare = static_cast<uint8_t>((rx_pending[idx] >> 4u) & 0x0Fu);
		if (u > 0u && bare < 15u) {
			bare++;
			rx_pending[idx] = static_cast<uint8_t>(rx | (bare << 4u));
		}
	}
	resolve_bare_naks(idx);
	update_complete(idx);
}

} // namespace

extern "C" {

void midici_initiator_init(void)
{
	reset_now();
	last_now_ms = 0;
}

void midici_initiator_reset(void)
{
	reset_requested = true;
}

void midici_initiator_tick(uint32_t now_ms)
{
	check_reset();
	last_now_ms = now_ms;
	close_window_if_expired(now_ms);

	for (uint8_t i = 0; i < peer_count; i++) {
		if ((in_flight_mask(i) & RX_PROFILE) &&
		    static_cast<uint32_t>(now_ms - profile_progress[i].last_reply_ms) >= kProfileReplyTimeoutMs) {
			peers[i].flags |= CI_PEER_FLAG_NAK;
			finish_profile_inquiry(i);
		}
	}

	if (pe_get.active && static_cast<uint32_t>(now_ms - pe_get.start_ms) >= CI_PE_GET_TIMEOUT_MS) {
		const int idx_i = find_peer(pe_get.peer_muid);
		if (idx_i >= 0)
			fail_pe_get(static_cast<uint8_t>(idx_i));
		else
			pe_get.active = false;
	}

	if (discovery_tx_pending) {
		try_send_discovery();
		return;
	}

	for (uint8_t i = 0; i < peer_count; i++) {
		const uint8_t mask = static_cast<uint8_t>(tx_pending[i] & (TX_ENDPOINT | TX_PROFILE | TX_PE_CAPS));
		if (!mask)
			continue;
		const uint8_t bit = (mask & TX_ENDPOINT) ? TX_ENDPOINT
			: (mask & TX_PROFILE) ? TX_PROFILE
			: TX_PE_CAPS;
		try_send_followup(i, bit);
		return;
	}

	if (!pe_get.active) {
		for (uint8_t i = 0; i < peer_count; i++) {
			if (tx_pending[i] & TX_PE_GET) {
				try_send_pe_get(i, now_ms);
				return;
			}
		}
	}
}

void midici_initiator_start_discovery(uint32_t now_ms)
{
	check_reset();
	last_now_ms = now_ms;
	memset(peers, 0, sizeof(peers));
	memset(tx_pending, 0, sizeof(tx_pending));
	memset(rx_pending, 0, sizeof(rx_pending));
	memset(profile_progress, 0, sizeof(profile_progress));
	pe_get = {};
	peer_count = 0;
	window_start_ms = now_ms;
	discovering = true;
	discovery_tx_pending = true;
	echo_armed = false;
	try_send_discovery();
}

void midici_initiator_on_ump_negotiated(uint32_t now_ms)
{
	check_reset();
	last_now_ms = now_ms;
	if (auto_discovered)
		return;
	auto_discovered = true;
	midici_initiator_start_discovery(now_ms);
}

bool midici_initiator_on_message(uint8_t group, const uint8_t *payload, uint16_t len,
                                 uint32_t now_ms)
{
	check_reset();
	last_now_ms = now_ms;
	close_window_if_expired(now_ms);

	if (group != kStandaloneGroup || !payload || len < ms::CommonHeaderByteCount)
		return false;

	ms::ParsedMessage m{};
	if (ms::Parse(payload, len, m) != ms::ParseStatus::Ok)
		return false;
	if (!ms::MuidIsUsable(m.SourceMuid))
		return false;
	if (m.DeviceId > 0x0Fu && m.DeviceId < 0x7Eu)
		return false;

	const uint32_t my_muid = midici_muid();
	if (m.SourceMuid == my_muid)
		return false;

	if (m.Type == T::InvalidateMuid) {
		if (m.DestinationMuid != ms::MuidBroadcast || m.DeviceId != ms::DeviceIdFunctionBlock)
			return false;
		if (m.TargetMuid == my_muid) {
			midici_initiator_on_muid_changed();
			return true;
		}
		const int idx = find_peer(m.TargetMuid);
		if (idx >= 0)
			remove_peer(static_cast<uint8_t>(idx));
		return true;
	}

	if (m.DestinationMuid != my_muid)
		return false;

	switch (m.Type) {
	case T::DiscoveryReply:
		handle_discovery_reply(m, payload, len);
		return true;
	case T::EndpointReply:
		handle_endpoint_reply(m, payload);
		return true;
	case T::ProfileInquiryReply:
		handle_profile_reply(m, now_ms);
		return true;
	case T::PropertyExchangeCapabilitiesReply:
		handle_pe_caps_reply(m, payload, len);
		return true;
	case T::PropertyGetDataReply:
		handle_pe_get_reply(m, payload);
		return true;
	case T::Nak:
		handle_nak(m);
		return true;
	default:
		return false;
	}
}

void midici_initiator_on_muid_changed(void)
{
	check_reset();
	echo_armed = false;
	for (uint8_t i = 0; i < peer_count; i++) {
		if (in_flight_mask(i) & RX_PROFILE) {
			peers[i].flags |= CI_PEER_FLAG_NAK;
			finish_profile_inquiry(i);
		}
	}
	if (pe_get.active) {
		const int idx = find_peer(pe_get.peer_muid);
		pe_get.active = false;
		if (idx >= 0) {
			const uint8_t i = static_cast<uint8_t>(idx);
			rx_pending[i] &= static_cast<uint8_t>(~RX_PE_GET);
			resolve_bare_naks(i);
			update_complete(i);
		}
	}
}

uint8_t midici_initiator_peer_count(void)
{
	if (reset_requested)
		return 0u;
	return peer_count;
}

bool midici_initiator_get_peer(uint8_t idx, ci_peer_info_t *out)
{
	if (reset_requested || !out || idx >= peer_count)
		return false;
	*out = peers[idx];
	return true;
}

bool midici_initiator_is_discovering(void)
{
	if (reset_requested)
		return false;
	return discovering;
}

uint32_t midici_initiator_now_ms(void)
{
	return last_now_ms;
}

bool midici_initiator_is_own_discovery_echo(const uint8_t *payload, uint16_t len)
{
	if (reset_requested || !echo_armed || !payload || len != ms::DiscoveryByteCount)
		return false;
	uint8_t expected[ms::DiscoveryByteCount];
	if (build_discovery_msg(expected, sizeof(expected), midici_muid()) != ms::DiscoveryByteCount)
		return false;
	if (memcmp(payload, expected, ms::DiscoveryByteCount) != 0)
		return false;
	echo_armed = false;
	return true;
}

void midici_tick(uint32_t now_ms)
{
	midici_initiator_tick(now_ms);
}

void midici_on_ump_negotiated(uint32_t now_ms)
{
	midici_initiator_on_ump_negotiated(now_ms);
}

} /* extern "C" */
