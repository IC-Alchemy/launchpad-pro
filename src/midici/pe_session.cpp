#include "midici/pe_session.h"

extern "C" {
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "midici/midici.h"
}

namespace ms = WindowsMidiServicesCapabilityInquiry;

namespace lp_pe {

namespace {

using T = ms::MessageType;

constexpr uint8_t kGroup = 0;	/* PE is answered on the Standalone Function Block only */
constexpr uint8_t kHeaderMax = 48;
constexpr uint8_t kSubscribable[] = {CI_PE_SCALE_SETTINGS, CI_PE_PLAY_OPTIONS, CI_PE_SURFACE_SETTINGS};
constexpr int kSubCount = sizeof kSubscribable / sizeof kSubscribable[0];
/* PE Notify status that ends a transaction (Common Rules for PE, 144: terminate). */
constexpr int32_t kStatusTerminate = 144;

SettingsGet settings_get;
SettingsApply settings_apply;

volatile bool reset_requested;

/* The one PE message being sent, a chunk at a time. */
struct Outgoing {
	bool active;
	uint8_t type;		/* ms::MessageType */
	uint8_t request_id;
	uint32_t destination;
	char header[kHeaderMax];
	uint8_t header_len;
	uint8_t resource;	/* body, or CI_PE_NONE for an empty body */
	struct settings snapshot;	/* what the body describes, fixed when it was queued */
	uint16_t body_len;
	uint16_t first_data;	/* data bytes in chunk 1 (it also carries the header) */
	uint16_t later_data;
	uint16_t chunk_count;
	uint16_t next_chunk;
};
Outgoing out;

/* A Set arriving in chunks. */
struct SetRx {
	bool active;
	uint32_t source;
	uint8_t request_id;
	uint8_t resource;
	uint16_t status;	/* an error found in the header, or 0 */
	uint16_t len;
	uint16_t chunk_count;
	uint16_t next_chunk;
	bool overflow;
};
SetRx set_rx;
char set_body[CI_PE_SET_BODY_MAX];

struct Subscription {
	bool active;
	uint32_t muid;
	uint16_t id;		/* subscribeId is "lp" + id */
};
Subscription subs[kSubCount];	/* indexed like kSubscribable */
uint16_t next_sub_id = 1;
uint8_t notify_pending;	/* CI_PE_BIT mask */

struct settings last_seen;
bool have_last_seen;

uint8_t my_request_id;	/* for the notifies we initiate */

/* The last initiator that sent us a Discovery, and the SysEx size it can take. */
uint32_t peer_muid = ms::MuidBroadcast;
uint32_t peer_max_sysex;

int sub_index(uint8_t resource)
{
	for (int i = 0; i < kSubCount; i++)
		if (kSubscribable[i] == resource)
			return i;
	return -1;
}

void reset_now()
{
	reset_requested = false;
	out.active = false;
	set_rx.active = false;
	for (auto &s : subs)
		s.active = false;
	notify_pending = 0;
	peer_muid = ms::MuidBroadcast;
}

void check_reset()
{
	if (reset_requested)
		reset_now();
}

uint16_t message_limit(uint32_t destination)
{
	uint32_t limit = CI_PE_MESSAGE_MAX;
	/* 128 is the smallest any MIDI-CI device may declare; anything below is noise */
	if (destination == peer_muid && peer_max_sysex >= 128 && peer_max_sysex < limit)
		limit = peer_max_sysex;
	return static_cast<uint16_t>(limit);
}

/* "header" built with the json writer into out.header */
struct HeaderWriter {
	ci_json_out o;
	HeaderWriter() { ci_json_out_init(&o, out.header, 0, kHeaderMax); }
	void finish() { out.header_len = static_cast<uint8_t>(o.pos < kHeaderMax ? o.pos : kHeaderMax); }
};

void write_sub_id(ci_json_out *o, uint16_t id)
{
	ci_json_raw(o, "\"lp");
	ci_json_int(o, id);
	ci_json_raw(o, "\"");
}

/* Plans out.* for a header already in out.header and a body (or none). */
void start(uint8_t type, uint8_t request_id, uint32_t destination, uint8_t resource, const struct settings *s,
           const char *piid)
{
	const uint16_t limit = message_limit(destination);
	const uint16_t fixed = static_cast<uint16_t>(ms::PropertyExchangeFixedByteCount + 2);	/* + F0 F7 */
	out.type = type;
	out.request_id = request_id;
	out.destination = destination;
	out.resource = resource;
	if (s)
		out.snapshot = *s;
	out.body_len = resource == CI_PE_NONE ? 0 : ci_pe_body_len(resource, &out.snapshot, piid);
	out.first_data = static_cast<uint16_t>(limit - fixed - out.header_len);
	out.later_data = static_cast<uint16_t>(limit - fixed);
	if (out.body_len <= out.first_data)
		out.chunk_count = 1;
	else
		out.chunk_count = static_cast<uint16_t>(
			1 + (out.body_len - out.first_data + out.later_data - 1) / out.later_data);
	out.next_chunk = 1;
	out.active = true;
}

/* A reply with just {"status":N} and no body. */
void reply_status(uint8_t type, const ms::ParsedMessage &m, uint16_t status)
{
	HeaderWriter h;
	ci_json_raw(&h.o, "{\"status\":");
	ci_json_int(&h.o, status);
	ci_json_raw(&h.o, "}");
	h.finish();
	start(type, m.PropertyExchange.RequestId, m.SourceMuid, CI_PE_NONE, nullptr, nullptr);
}

/* Builds chunk out.next_chunk into buf; returns its length (no F0/F7). */
size_t build_chunk(uint8_t *buf, size_t cap, uint32_t my_muid, const char *piid)
{
	const uint16_t c = out.next_chunk;
	const bool first = c == 1;
	const uint16_t offset = first ? 0 : static_cast<uint16_t>(out.first_data + (c - 2) * out.later_data);
	const uint16_t budget = first ? out.first_data : out.later_data;
	const uint16_t remaining = out.body_len > offset ? static_cast<uint16_t>(out.body_len - offset) : 0;
	const uint16_t data = remaining < budget ? remaining : budget;
	const uint16_t header = first ? out.header_len : 0;
	const size_t total = ms::PropertyExchangeFixedByteCount + header + data;
	if (total > cap)
		return 0;

	size_t n = 0;
	buf[n++] = ms::UniversalSystemExclusiveId;
	buf[n++] = ms::DeviceIdFunctionBlock;
	buf[n++] = ms::SubId1CapabilityInquiry;
	buf[n++] = out.type;
	buf[n++] = ms::MessageVersionCurrent;
	ms::WriteMuid(buf + n, my_muid);
	n += 4;
	ms::WriteMuid(buf + n, out.destination);
	n += 4;
	buf[n++] = out.request_id & 0x7F;
	ms::WriteFourteenBitValue(buf + n, header);
	n += 2;
	for (uint16_t i = 0; i < header; i++)
		buf[n++] = static_cast<uint8_t>(out.header[i]) & 0x7F;
	ms::WriteFourteenBitValue(buf + n, out.chunk_count);
	n += 2;
	ms::WriteFourteenBitValue(buf + n, c);
	n += 2;
	ms::WriteFourteenBitValue(buf + n, data);
	n += 2;
	if (data) {
		ci_json_out o;
		ci_json_out_init(&o, reinterpret_cast<char *>(buf + n), offset, data);
		ci_pe_render(out.resource, &out.snapshot, piid, &o);
		for (uint16_t i = 0; i < data; i++)
			buf[n + i] &= 0x7F;
		n += data;
	}
	return n;
}

void emit(uint32_t my_muid, const char *piid)
{
	while (out.active) {
		uint16_t cap = 0;
		uint8_t *buf = midici_reply_buffer(&cap);
		/* sized from the plan before building, so a full queue costs nothing */
		const uint16_t header = out.next_chunk == 1 ? out.header_len : 0;
		const uint16_t budget = out.next_chunk == 1 ? out.first_data : out.later_data;
		const uint16_t packets = static_cast<uint16_t>(
			(ms::PropertyExchangeFixedByteCount + header + budget + 5) / 6);
		if (midici_tx_free_packets() < packets)
			return;
		const size_t n = build_chunk(buf, cap, my_muid, piid);
		if (n == 0 || midici_send_sysex(kGroup, buf, static_cast<uint16_t>(n)) != 0) {
			/* cannot happen with the sizes above; never spin on it */
			out.active = false;
			return;
		}
		if (++out.next_chunk > out.chunk_count)
			out.active = false;
	}
}

/* ---- incoming ---- */

struct Header {
	uint8_t resource = CI_PE_NONE;
	bool has_resource = false;
	bool set_partial = false;
	bool non_ascii = false;
	enum { NONE, START, END, OTHER } command = NONE;
	bool has_sub_id = false;
	int32_t sub_id = -1;
	int32_t status = 0;
	bool malformed = false;
};

Header parse_header(const uint8_t *p, uint16_t n)
{
	Header h;
	ci_json_it it;
	ci_json_member m;
	int r;
	ci_json_it_init(&it, reinterpret_cast<const char *>(p), n);
	while ((r = ci_json_next(&it, &m)) == 1) {
		if (ci_json_key_is(&m, "resource")) {
			h.has_resource = true;
			if (m.type == CI_JSON_STR)
				h.resource = ci_pe_lookup(m.str, m.str_len);
		} else if (ci_json_key_is(&m, "setPartial")) {
			h.set_partial = m.type == CI_JSON_BOOL && m.num;
		} else if (ci_json_key_is(&m, "mutualEncoding")) {
			h.non_ascii = !ci_json_str_is(&m, "ASCII");
		} else if (ci_json_key_is(&m, "command")) {
			h.command = ci_json_str_is(&m, "start") ? Header::START
			          : ci_json_str_is(&m, "end")   ? Header::END
			                                        : Header::OTHER;
		} else if (ci_json_key_is(&m, "subscribeId")) {
			h.has_sub_id = true;
			/* ours look like "lp<n>" */
			if (m.type == CI_JSON_STR && m.str_len > 2 && m.str_len < 8 && m.str[0] == 'l' && m.str[1] == 'p') {
				int32_t v = 0;
				for (uint16_t i = 2; i < m.str_len && v >= 0; i++)
					v = (m.str[i] >= '0' && m.str[i] <= '9') ? v * 10 + (m.str[i] - '0') : -1;
				h.sub_id = v;
			}
		} else if (ci_json_key_is(&m, "status")) {
			h.status = m.type == CI_JSON_INT ? m.num : 0;
		}
	}
	h.malformed = r < 0;
	return h;
}

bool settings_resource(uint8_t r)
{
	return r == CI_PE_CHANNEL_LIST || r == CI_PE_SCALE_SETTINGS || r == CI_PE_PLAY_OPTIONS;
}

/* A resource that exists and can be served now. */
uint8_t available(const Header &h)
{
	if (h.resource == CI_PE_NONE || (settings_resource(h.resource) && !settings_get))
		return CI_PE_NONE;
	return h.resource;
}

void on_get(const ms::ParsedMessage &m, const uint8_t *payload, const char *piid)
{
	const auto &pe = m.PropertyExchange;
	if (pe.ChunkNumber != 1 || pe.ChunkCount != 1) {
		reply_status(static_cast<uint8_t>(T::PropertyGetDataReply), m, CI_PE_STATUS_BAD_REQUEST);
		return;
	}
	const Header h = parse_header(payload + pe.HeaderOffset, pe.HeaderByteCount);
	const uint8_t r = available(h);
	if (h.malformed || !h.has_resource || r == CI_PE_NONE) {
		reply_status(static_cast<uint8_t>(T::PropertyGetDataReply), m,
		             h.malformed || !h.has_resource ? CI_PE_STATUS_BAD_REQUEST : CI_PE_STATUS_NOT_FOUND);
		return;
	}
	HeaderWriter w;
	ci_json_raw(&w.o, "{\"status\":200}");
	w.finish();
	start(static_cast<uint8_t>(T::PropertyGetDataReply), pe.RequestId, m.SourceMuid, r,
	      settings_get ? settings_get() : nullptr, piid);
}

/* After a Set or a menu change: queue full notifies for subscribers of the
 * changed resources, except the initiator that made the change. */
void mark_changed(uint8_t bits, uint32_t except)
{
	for (int i = 0; i < kSubCount; i++)
		if (subs[i].active && (bits & CI_PE_BIT(kSubscribable[i])) && subs[i].muid != except)
			notify_pending |= CI_PE_BIT(kSubscribable[i]);
}

void finish_set(const ms::ParsedMessage &m)
{
	uint16_t status = set_rx.status;
	set_rx.active = false;
	if (!status && set_rx.overflow)
		status = CI_PE_STATUS_TOO_LARGE;
	if (!status) {
		struct settings next = *settings_get();
		status = ci_pe_apply_set(set_rx.resource, set_body, set_rx.len, &next);
		if (status == CI_PE_STATUS_OK) {
			const struct settings before = *settings_get();
			settings_apply(&next);
			const struct settings &after = *settings_get();
			mark_changed(ci_pe_changed(&before, &after), m.SourceMuid);
			last_seen = after;
			have_last_seen = true;
		}
	}
	reply_status(static_cast<uint8_t>(T::PropertySetDataReply), m, status);
}

void on_set(const ms::ParsedMessage &m, const uint8_t *payload)
{
	const auto &pe = m.PropertyExchange;
	const uint8_t set_reply = static_cast<uint8_t>(T::PropertySetDataReply);
	if (pe.ChunkNumber == 1) {
		const Header h = parse_header(payload + pe.HeaderOffset, pe.HeaderByteCount);
		set_rx.active = true;
		set_rx.source = m.SourceMuid;
		set_rx.request_id = pe.RequestId;
		set_rx.resource = available(h);
		set_rx.len = 0;
		set_rx.overflow = false;
		set_rx.chunk_count = pe.ChunkCount;
		set_rx.next_chunk = 1;
		if (h.malformed || !h.has_resource || h.set_partial || h.non_ascii || pe.ChunkCount == 0)
			set_rx.status = CI_PE_STATUS_BAD_REQUEST;
		else if (set_rx.resource == CI_PE_NONE)
			set_rx.status = CI_PE_STATUS_NOT_FOUND;
		else if (!ci_pe_can_set(set_rx.resource) || !settings_apply)
			set_rx.status = CI_PE_STATUS_NOT_ALLOWED;
		else
			set_rx.status = 0;
	} else if (!set_rx.active || set_rx.source != m.SourceMuid || set_rx.request_id != pe.RequestId ||
	           pe.ChunkNumber != set_rx.next_chunk || pe.ChunkCount != set_rx.chunk_count) {
		/* a chunk out of order, or of a Set we never saw start */
		set_rx.active = false;
		reply_status(set_reply, m, CI_PE_STATUS_BAD_REQUEST);
		return;
	}
	if (!set_rx.status) {
		for (uint16_t i = 0; i < pe.DataByteCount; i++) {
			if (set_rx.len >= CI_PE_SET_BODY_MAX) {
				set_rx.overflow = true;
				break;
			}
			set_body[set_rx.len++] = static_cast<char>(payload[pe.DataOffset + i] & 0x7F);
		}
	}
	set_rx.next_chunk++;
	if (pe.ChunkNumber >= set_rx.chunk_count)
		finish_set(m);
}

void on_subscription(const ms::ParsedMessage &m, const uint8_t *payload)
{
	const auto &pe = m.PropertyExchange;
	const uint8_t reply = static_cast<uint8_t>(T::PropertySubscriptionReply);
	const Header h = parse_header(payload + pe.HeaderOffset, pe.HeaderByteCount);
	if (h.malformed || pe.ChunkNumber != 1 || pe.ChunkCount != 1) {
		reply_status(reply, m, CI_PE_STATUS_BAD_REQUEST);
		return;
	}
	if (h.command == Header::START) {
		const uint8_t r = available(h);
		const int i = sub_index(r);
		if (r == CI_PE_NONE || i < 0) {
			reply_status(reply, m, r == CI_PE_NONE ? CI_PE_STATUS_NOT_FOUND : CI_PE_STATUS_NOT_ALLOWED);
			return;
		}
		/* one subscriber per resource: the latest wins */
		subs[i].active = true;
		subs[i].muid = m.SourceMuid;
		subs[i].id = next_sub_id;
		next_sub_id = static_cast<uint16_t>(next_sub_id % 9999 + 1);
		notify_pending &= static_cast<uint8_t>(~CI_PE_BIT(r));
		HeaderWriter w;
		ci_json_raw(&w.o, "{\"status\":200,\"subscribeId\":");
		write_sub_id(&w.o, subs[i].id);
		ci_json_raw(&w.o, "}");
		w.finish();
		start(reply, pe.RequestId, m.SourceMuid, CI_PE_NONE, nullptr, nullptr);
		return;
	}
	if (h.command == Header::END) {
		for (auto &s : subs)
			if (s.active && s.muid == m.SourceMuid &&
			    ((h.has_sub_id && s.id == h.sub_id) || (!h.has_sub_id && h.has_resource &&
			                                             kSubscribable[&s - subs] == h.resource)))
				s.active = false;
		reply_status(reply, m, CI_PE_STATUS_OK);
		return;
	}
	/* full / partial / notify go from a responder to its subscriber, not to us */
	reply_status(reply, m, CI_PE_STATUS_BAD_REQUEST);
}

void on_notify(const ms::ParsedMessage &m, const uint8_t *payload)
{
	const auto &pe = m.PropertyExchange;
	const Header h = parse_header(payload + pe.HeaderOffset, pe.HeaderByteCount);
	/* the initiator gives up on the reply we are sending it */
	if (h.status == kStatusTerminate && out.active && out.destination == m.SourceMuid &&
	    out.request_id == pe.RequestId)
		out.active = false;
}

void requeue_interrupted_notify()
{
	/* a notify cut short by a reply: send it again, in full, afterwards */
	if (out.active && out.type == static_cast<uint8_t>(T::PropertySubscriptionInquiry) && out.next_chunk > 1)
		notify_pending |= CI_PE_BIT(out.resource);
}

} // namespace

void bind(SettingsGet get, SettingsApply apply)
{
	settings_get = get;
	settings_apply = apply;
	have_last_seen = false;
}

void request_reset()
{
	reset_requested = true;
}

void note_initiator(uint32_t muid, uint32_t max_sysex)
{
	check_reset();
	peer_muid = muid;
	peer_max_sysex = max_sysex;
}

void forget_initiator(uint32_t muid)
{
	check_reset();
	for (auto &s : subs)
		if (s.active && s.muid == muid)
			s.active = false;
	if (out.active && out.destination == muid)
		out.active = false;
	if (set_rx.active && set_rx.source == muid)
		set_rx.active = false;
	if (peer_muid == muid)
		peer_muid = ms::MuidBroadcast;
}

void handle(const ms::ParsedMessage &m, const uint8_t *payload, size_t len, uint32_t my_muid, const char *piid)
{
	(void)len;
	check_reset();
	const T type = m.Type;
	const bool inquiry = type == T::PropertyGetDataInquiry || type == T::PropertySetDataInquiry ||
	                     type == T::PropertySubscriptionInquiry;
	if (inquiry)
		requeue_interrupted_notify();
	switch (type) {
	case T::PropertyGetDataInquiry:
		on_get(m, payload, piid);
		break;
	case T::PropertySetDataInquiry:
		on_set(m, payload);
		break;
	case T::PropertySubscriptionInquiry:
		on_subscription(m, payload);
		break;
	case T::PropertyNotify:
		on_notify(m, payload);
		break;
	default:
		/* replies to our notifies (0x39) and stray replies: nothing to do */
		break;
	}
	emit(my_muid, piid);
}

void poll(uint32_t my_muid, const char *piid)
{
	check_reset();
	if (settings_get) {
		const struct settings &now = *settings_get();
		if (have_last_seen) {
			const uint8_t bits = ci_pe_changed(&last_seen, &now);
			if (bits)
				mark_changed(bits, ms::MuidBroadcast);
		}
		last_seen = now;
		have_last_seen = true;
	}
	if (!out.active && notify_pending && settings_get) {
		for (int i = 0; i < kSubCount; i++) {
			const uint8_t r = kSubscribable[i];
			if (!(notify_pending & CI_PE_BIT(r)))
				continue;
			notify_pending &= static_cast<uint8_t>(~CI_PE_BIT(r));
			if (!subs[i].active)
				continue;
			HeaderWriter w;
			ci_json_raw(&w.o, "{\"command\":\"full\",\"subscribeId\":");
			write_sub_id(&w.o, subs[i].id);
			ci_json_raw(&w.o, "}");
			w.finish();
			my_request_id = static_cast<uint8_t>(my_request_id % 127 + 1);
			start(static_cast<uint8_t>(T::PropertySubscriptionInquiry), my_request_id, subs[i].muid, r,
			      settings_get(), piid);
			break;
		}
	}
	emit(my_muid, piid);
}

bool busy()
{
	return out.active;
}

uint8_t subscriptions()
{
	uint8_t bits = 0;
	for (int i = 0; i < kSubCount; i++)
		if (subs[i].active)
			bits |= CI_PE_BIT(kSubscribable[i]);
	return bits;
}

} // namespace lp_pe
