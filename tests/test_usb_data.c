#include "test.h"
#include "stubs/stubs.h"
#include "usb/usb_compat.h"
#include "usb/usb_midi2.h"
#include "midici/midici.h"
#include "midici/responder.h"
#include "midici/ci_pe.h"
#include "instrument/settings.h"
#include "instrument/palette.h"
#include "platform/lib_private.h"
#include "ump/ump.h"
#include "midi2.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

void EP1_OUT_Callback(void);
void __wrap_midi_parsers_init(void);
void __wrap_midi_parsers_poll(void);
void __wrap_usbmidi_poll(void);
void __wrap_midi_flush_windows_buffer(void);
void __wrap_hal_send_midi(uint8_t port, uint8_t status, uint8_t d1, uint8_t d2);
void __wrap_hal_send_sysex(uint8_t port, const uint8_t *data, uint16_t len);

/* ---- helpers ---- */

/* configured, alt 0, main loop ran once */
static void enter_alt0(void)
{
	stub_irq_hook = 0;
	stubs_reset();
	usb_midi2_usb_reset();
	usb_midi2_service();
	stubs_reset();
	bDeviceState = USB_STATE_CONFIGURED;
	midi2_debug_pause_rx(0);
	ump_stream_set_protocol_hook(midi2_protocol_changing);
}

/* configured, alt 1 (MIDI 2.0 protocol), main loop ran once; EP1 RX armed */
static void enter_alt1(void)
{
	stub_irq_hook = 0;
	stubs_reset();
	usb_midi2_usb_reset();
	usb_midi2_set_alt(1);
	usb_midi2_service();
	stub.ep_rx_valid_calls = 0;
	stub.fifo_flush_calls = 0;
	stub.fifo1k_flush_calls = 0;
	bDeviceState = USB_STATE_CONFIGURED;
	midi2_debug_pause_rx(0);
	ump_stream_set_protocol_hook(midi2_protocol_changing);
}

/* host → device: one EP1 OUT packet of n words, little-endian */
static void host_out(const uint32_t *w, uint16_t n)
{
	uint8_t p[64];
	uint16_t i;
	for (i = 0; i < n && i < 16; i++) {
		p[4 * i] = (uint8_t)w[i];
		p[4 * i + 1] = (uint8_t)(w[i] >> 8);
		p[4 * i + 2] = (uint8_t)(w[i] >> 16);
		p[4 * i + 3] = (uint8_t)(w[i] >> 24);
	}
	stub_rx_packet(p, (uint16_t)(4 * i));
	EP1_OUT_Callback();
}

/* the host took the last IN packet; the main loop sends the next one.
 * Returns the words of the packet written (0 if none). */
static uint16_t host_in(uint32_t out[16])
{
	uint16_t i;
	int writes = stub.tx_writes;
	stub.ep_tx_status[1] = EP_TX_NAK;
	__wrap_usbmidi_poll();
	if (stub.tx_writes == writes)
		return 0;
	for (i = 0; i < stub.last_tx_len / 4; i++)
		out[i] = (uint32_t)stub.last_tx[4 * i] | ((uint32_t)stub.last_tx[4 * i + 1] << 8) |
		         ((uint32_t)stub.last_tx[4 * i + 2] << 16) |
		         ((uint32_t)stub.last_tx[4 * i + 3] << 24);
	return (uint16_t)(stub.last_tx_len / 4);
}

/* MIDI-CI frames include F0/F7 here, but the UMP codec strips them on the wire. */
static void host_out_sysex(uint8_t group, const uint8_t *data, uint16_t len)
{
	uint32_t words[UMP_RX_WORDS];
	uint16_t count = ump_sysex7_pack(words, UMP_RX_WORDS, group, data, len);
	CHECK(count != 0);
	for (uint16_t i = 0; i < count; i += 16) {
		uint16_t remaining = (uint16_t)(count - i);
		host_out(words + i, remaining < 16 ? remaining : 16);
		__wrap_midi_parsers_poll();
	}
}

/* reply bytes already complete when the Function Block alert arrived */
static uint16_t alert_after_len;

static void host_in_ci(struct ump_sysex7_rx *reply, int expected_alerts)
{
	uint32_t words[16];
	int alerts = 0;
	ump_sysex7_rx_reset(reply);
	for (uint16_t count;;) {
		if ((count = host_in(words)) == 0) {
			__wrap_midi_parsers_poll();	/* one more main-loop pass, as on hardware */
			if ((count = host_in(words)) == 0)
				break;
		}
		for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
			if (words[i] >> 28 == 3) {
				CHECK_EQ((words[i] >> 24) & 0xF, 0);
				CHECK(ump_sysex7_rx_feed(reply, words + i) >= 0);
			} else if (words[i] == 0xF0118033u) {
				CHECK_EQ(words[i + 1], 0x00010200u);
				CHECK_EQ(reply->active, 0);
				alert_after_len = reply->len;
				alerts++;
			}
		}
	}
	CHECK_EQ(alerts, expected_alerts);
	CHECK_EQ(reply->active, 0);
}

static const uint8_t ci_discovery[] = {
	0xF0, 0x7E, 0x7F, 0x0D, 0x70, 0x02, 1, 2, 3, 4, 0x7F, 0x7F, 0x7F, 0x7F,
	0x7D, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 8, 0, 32, 0, 0, 7, 0xF7
};

static void ci_discover(struct ump_sysex7_rx *reply)
{
	host_out_sysex(0, ci_discovery, sizeof(ci_discovery));
	host_in_ci(reply, 0);
	CHECK_EQ(reply->len, 33);
	CHECK_EQ(reply->buf[4], 0x71);
}

static void test_ci_discovery_endpoint_and_version(void)
{
	struct ump_sysex7_rx reply;
	uint8_t inquiry[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x72, 2, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0xF7};
	uint8_t legacy[sizeof(ci_discovery) - 1];
	enter_alt1();
	ci_discover(&reply);
	CHECK_MEM(reply.buf + 10, ci_discovery + 6, 4);
	const uint8_t identity[] = {0, 0x20, 0x29, 0x51, 0, 0, 0, 0, 1, 0, 0};
	CHECK_MEM(reply.buf + 14, identity, sizeof(identity));
	CHECK_EQ(reply.buf[25], 0x08); /* Property Exchange only: no Profiles or Process Inquiry */
	CHECK_EQ(reply.buf[26] + (reply.buf[27] << 7), CI_REASSEMBLY_BYTES);
	CHECK_EQ(reply.buf[30], 7); /* initiator Output Path ID */
	CHECK_EQ(reply.buf[31], 0); /* Standalone Function Block */
	memcpy(inquiry + 10, reply.buf + 6, 4);
	host_out_sysex(0, inquiry, sizeof(inquiry));
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.len, 26);
	CHECK_EQ(reply.buf[4], 0x73);
	CHECK_EQ(reply.buf[15], 8);
	CHECK_EQ(reply.buf[16], 0);
	CHECK_MEM(reply.buf + 17, "TEST0001", 8);
	CHECK_EQ(stub_sysex_calls, 0);

	memcpy(legacy, ci_discovery, sizeof(legacy) - 1);
	legacy[5] = 1;
	legacy[sizeof(legacy) - 1] = 0xF7;
	host_out_sysex(0, legacy, sizeof(legacy));
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.len, 33);
	CHECK_EQ(reply.buf[5], 2); /* responder keeps its declared format */
	CHECK_EQ(reply.buf[30], 0);
	CHECK_MEM(reply.buf + 6, inquiry + 10, 4);

	host_out_sysex(1, ci_discovery, sizeof(ci_discovery));
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.len, 0);
	CHECK_EQ(stub_sysex_port, USBMIDI);
	CHECK_EQ(stub_sysex_calls, 1);
}

static void test_ci_nak_address_and_sysex_limit(void)
{
	struct ump_sysex7_rx reply;
	uint8_t inquiry[128] = {0xF0, 0x7E, 0x7F, 0x0D, 0x20, 2, 1, 2, 3, 4}; /* Profile Inquiry: not supported */
	enter_alt1();
	ci_discover(&reply);
	memcpy(inquiry + 10, reply.buf + 6, 4);
	inquiry[sizeof(inquiry) - 1] = 0xF7;
	host_out_sysex(0, inquiry, sizeof(inquiry));
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.len, 25);
	CHECK_EQ(reply.buf[4], 0x7F);
	CHECK_EQ(reply.buf[14], 0x20);
	CHECK_EQ(reply.buf[15], 0x01);

	inquiry[5] = 0x22; /* reserved version bits */
	host_out_sysex(0, inquiry, sizeof(inquiry));
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.buf[5], 2);
	CHECK_EQ(reply.buf[15], 0x02);

	inquiry[5] = 2;
	inquiry[4] = 0x72;
	inquiry[14] = 0xF7; /* endpoint inquiry without a status byte */
	host_out_sysex(0, inquiry, 15);
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.buf[15], 0x41);

	inquiry[10] ^= 1; /* for a different MUID */
	host_out_sysex(0, inquiry, 15);
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.len, 0);
}

static void test_ci_unsupported_profile_request_nak_details(void)
{
	struct ump_sysex7_rx reply;
	uint8_t request[] = {0xF0, 0x7E, 0x00, 0x0D, 0x22, 0x02, 1, 2, 3, 4,
		0, 0, 0, 0, 0x7E, 1, 2, 3, 4, 1, 0, 0xF7};
	const uint8_t types[] = {0x22, 0x23, 0x28};
	const uint8_t scopes[] = {0x00, 0x7E, 0x7F};
	enter_alt1();
	ci_discover(&reply);
	memcpy(request + 10, reply.buf + 6, 4);
	for (unsigned i = 0; i < sizeof(types); i++) {
		request[2] = scopes[i];
		request[4] = types[i];
		request[19] = (i == 0) ? 1 : 0;
		if (types[i] == 0x28)
			request[20] = 0xF7; /* Profile Details Inquiry has one target byte */
		host_out_sysex(0, request, sizeof(request) - (types[i] == 0x28));
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 25);
		CHECK_EQ(reply.buf[2], scopes[i]);
		CHECK_EQ(reply.buf[4], 0x7F);
		CHECK_EQ(reply.buf[5], 2);
		CHECK_MEM(reply.buf + 6, request + 10, 4);
		CHECK_MEM(reply.buf + 10, request + 6, 4);
		CHECK_EQ(reply.buf[14], types[i]);
		CHECK_EQ(reply.buf[15], 0x01);
		CHECK_MEM(reply.buf + 17, request + 14, 5);
	}

	/* Format 2 Set Profile On requires the two channel-count bytes. */
	request[4] = 0x22;
	request[19] = 0xF7;
	host_out_sysex(0, request, 20);
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.buf[15], 0x41);
	CHECK_MEM(reply.buf + 17, request + 14, 5);
}

static void test_ci_muid_replacement_and_collision(void)
{
	struct ump_sysex7_rx reply;
	uint8_t invalidate[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 1, 2, 3, 4,
		0x7F, 0x7F, 0x7F, 0x7F, 0, 0, 0, 0, 0xF7};
	uint8_t collision[sizeof(ci_discovery)];
	uint8_t replacement[4];
	enter_alt1();
	ci_discover(&reply);
	memcpy(invalidate + 14, reply.buf + 6, 4);
	host_out_sysex(0, invalidate, sizeof(invalidate));
	host_in_ci(&reply, 1);
	CHECK_EQ(reply.len, 0);
	ci_discover(&reply);
	CHECK(memcmp(reply.buf + 6, invalidate + 14, 4) != 0);

	memcpy(collision, ci_discovery, sizeof(collision));
	memcpy(collision + 6, reply.buf + 6, 4);
	host_out_sysex(0, collision, sizeof(collision));
	alert_after_len = 0;
	host_in_ci(&reply, 1);
	CHECK_EQ(reply.len, sizeof(invalidate));
	CHECK_EQ(alert_after_len, sizeof(invalidate));	/* Invalidate first, then the alert */
	CHECK_EQ(reply.buf[4], 0x7E);
	CHECK_MEM(reply.buf + 14, collision + 6, 4);
	CHECK(memcmp(reply.buf + 6, collision + 6, 4) != 0);
	memcpy(replacement, reply.buf + 6, 4);
	ci_discover(&reply);
	CHECK_MEM(reply.buf + 6, replacement, 4);
}

static void test_ci_reply_retries_and_is_discarded_on_alt_change(void)
{
	struct ump_sysex7_rx reply;
	const uint32_t filler[4] = {0xF0010000u, 0, 0, 0};
	uint32_t packet[16];
	enter_alt1();
	while (ump_tx_push(UMP_TX_CONTROL, filler, 4) == 0) {}
	host_out_sysex(0, ci_discovery, sizeof(ci_discovery));
	/* the reply waits in CiTxQueue until USB IN frees the control ring, then arrives */
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.buf[4], 0x71);
	CHECK_EQ(reply.len, 33);

	while (ump_tx_push(UMP_TX_CONTROL, filler, 4) == 0) {}
	host_out_sysex(0, ci_discovery, sizeof(ci_discovery));
	enter_alt1();
	__wrap_midi_parsers_poll();
	CHECK_EQ(host_in(packet), 0);
}

/* ---- MIDI-CI Property Exchange, end to end over USB ---- */

#define PE_MSGS 16
static struct ump_sysex7_rx pe_rx[PE_MSGS];
static uint8_t pe_count;
static uint8_t pe_muid[4];	/* the device's MUID, from its Discovery Reply */
static const uint8_t PE_ME[4] = {1, 2, 3, 4};	/* ci_discovery's initiator */
static const uint8_t PE_OTHER[4] = {5, 6, 7, 8};

static struct settings fake_s;
static int fake_applies;
static const struct settings *fake_get(void) { return &fake_s; }
static void fake_apply(const struct settings *s) { fake_s = *s; fake_applies++; }

/* every complete SysEx message the device sends until it goes quiet */
static void host_in_pe(void)
{
	struct ump_sysex7_rx cur;
	uint32_t words[16];
	pe_count = 0;
	ump_sysex7_rx_reset(&cur);
	for (uint16_t count;;) {
		if ((count = host_in(words)) == 0) {
			__wrap_midi_parsers_poll();
			if ((count = host_in(words)) == 0)
				break;
		}
		for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
			if (words[i] >> 28 != 3)
				continue;
			CHECK_EQ((words[i] >> 24) & 0xF, 0);
			int r = ump_sysex7_rx_feed(&cur, words + i);
			CHECK(r >= 0);
			if (r == 1) {
				CHECK(pe_count < PE_MSGS);
				if (pe_count < PE_MSGS)
					pe_rx[pe_count++] = cur;
				ump_sysex7_rx_reset(&cur);
			}
		}
	}
	CHECK_EQ(cur.active, 0);
}

struct pe_view {
	uint8_t type, req;
	const uint8_t *src, *dst;
	const char *hdr, *data;
	uint16_t hdr_len, count, num, data_len;
};

static void pe_view(const struct ump_sysex7_rx *m, struct pe_view *v)
{
	const uint8_t *b = m->buf;
	uint16_t p;
	memset(v, 0, sizeof *v);
	CHECK(m->len >= 24);
	if (m->len < 24)
		return;
	CHECK_EQ(b[1], 0x7E);
	CHECK_EQ(b[2], 0x7F);
	v->type = b[4];
	v->src = b + 6;
	v->dst = b + 10;
	v->req = b[14];
	v->hdr_len = (uint16_t)(b[15] | b[16] << 7);
	v->hdr = (const char *)b + 17;
	p = (uint16_t)(17 + v->hdr_len);
	v->count = (uint16_t)(b[p] | b[p + 1] << 7);
	v->num = (uint16_t)(b[p + 2] | b[p + 3] << 7);
	v->data_len = (uint16_t)(b[p + 4] | b[p + 5] << 7);
	v->data = (const char *)b + p + 6;
	CHECK_EQ(p + 6 + v->data_len + 1, m->len);
}

static void pe_send_from(const uint8_t src[4], uint8_t type, uint8_t req, const char *hdr, uint16_t count,
                         uint16_t num, const char *data, uint16_t dlen)
{
	static uint8_t b[400];
	uint16_t n = 0, hl = (uint16_t)strlen(hdr);
	b[n++] = 0xF0; b[n++] = 0x7E; b[n++] = 0x7F; b[n++] = 0x0D; b[n++] = type; b[n++] = 2;
	memcpy(b + n, src, 4); n += 4;
	memcpy(b + n, pe_muid, 4); n += 4;
	b[n++] = req;
	b[n++] = hl & 0x7F; b[n++] = (uint8_t)(hl >> 7);
	memcpy(b + n, hdr, hl); n += hl;
	b[n++] = count & 0x7F; b[n++] = (uint8_t)(count >> 7);
	b[n++] = num & 0x7F; b[n++] = (uint8_t)(num >> 7);
	b[n++] = dlen & 0x7F; b[n++] = (uint8_t)(dlen >> 7);
	memcpy(b + n, data, dlen); n += dlen;
	b[n++] = 0xF7;
	host_out_sysex(0, b, n);
}

static void pe_send(uint8_t type, uint8_t req, const char *hdr, const char *body)
{
	pe_send_from(PE_ME, type, req, hdr, 1, 1, body, (uint16_t)strlen(body));
}

/* Reassembles a reply of `type` to `req` from pe_rx; header to hdr (NUL-ended),
 * body to body; returns the body length. Every message <= limit bytes. */
static uint16_t pe_reply(uint8_t type, uint8_t req, const uint8_t to[4], uint16_t limit, char *hdr, char *body,
                         uint16_t cap)
{
	struct pe_view v;
	uint16_t len = 0, count = 0;
	CHECK(pe_count > 0);
	for (uint8_t i = 0; i < pe_count; i++) {
		pe_view(&pe_rx[i], &v);
		CHECK(pe_rx[i].len <= limit);
		CHECK_EQ(v.type, type);
		CHECK_EQ(v.req, req);
		CHECK_MEM(v.src, pe_muid, 4);
		CHECK_MEM(v.dst, to, 4);
		CHECK_EQ(v.num, i + 1);
		if (i == 0) {
			count = v.count;
			memcpy(hdr, v.hdr, v.hdr_len);
			hdr[v.hdr_len] = 0;
		} else {
			CHECK_EQ(v.hdr_len, 0);
			CHECK_EQ(v.count, count);
		}
		if (len + v.data_len <= cap) {
			memcpy(body + len, v.data, v.data_len);
			len = (uint16_t)(len + v.data_len);
		}
	}
	CHECK_EQ(pe_count, count);
	if (len < cap)
		body[len] = 0;
	return len;
}

static uint16_t pe_render(uint8_t res, const struct settings *s, const char *piid, char *buf, uint16_t cap)
{
	struct ci_json_out o;
	ci_json_out_init(&o, buf, 0, cap);
	ci_pe_render(res, s, piid, &o);
	if (o.pos < cap)
		buf[o.pos] = 0;
	return o.pos;
}

/* alt 1, discovered by PE_ME with the given receivable SysEx size; settings bound or not */
static void pe_setup(uint16_t max_sysex, int bound)
{
	struct ump_sysex7_rx reply;
	uint8_t discovery[sizeof(ci_discovery)];
	enter_alt1();
	settings_defaults(&fake_s);
	fake_applies = 0;
	if (bound)
		midici_pe_bind(fake_get, fake_apply);
	else
		midici_pe_bind(0, 0);
	memcpy(discovery, ci_discovery, sizeof discovery);
	discovery[26] = max_sysex & 0x7F;
	discovery[27] = (max_sysex >> 7) & 0x7F;
	discovery[28] = (max_sysex >> 14) & 0x7F;
	host_out_sysex(0, discovery, sizeof discovery);
	host_in_ci(&reply, 0);
	CHECK_EQ(reply.buf[4], 0x71);
	CHECK_EQ(reply.buf[25], 0x08);
	memcpy(pe_muid, reply.buf + 6, 4);
}

static void test_ci_pe_capabilities_and_get(void)
{
	static char hdr[64], body[4096], want[4096];
	uint16_t n;

	pe_setup(4096, 0);
	/* PE Capabilities, as MIDI2Sound sends it (version 2: simultaneous, major, minor) */
	{
		uint8_t caps[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x30, 2, 1, 2, 3, 4, 0, 0, 0, 0, 1, 0, 0, 0xF7};
		memcpy(caps + 10, pe_muid, 4);
		host_out_sysex(0, caps, sizeof caps);
	}
	host_in_pe();
	CHECK(pe_count >= 1);
	CHECK_EQ(pe_rx[pe_count - 1].buf[4], 0x31);
	CHECK_EQ(pe_rx[pe_count - 1].buf[14], 1);	/* one request at a time */

	/* ResourceList: several chunks, each <= 320 bytes, reassembling to the resource */
	pe_send(0x34, 5, "{\"resource\":\"ResourceList\"}", "");
	host_in_pe();
	CHECK(pe_count > 1);
	n = pe_reply(0x35, 5, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":200}") == 0);
	CHECK_EQ(n, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	/* DeviceInfo: one chunk, serialNumber is the Product Instance ID */
	pe_send(0x34, 6, "{\"resource\":\"DeviceInfo\"}", "");
	host_in_pe();
	CHECK_EQ(pe_count, 1);
	n = pe_reply(0x35, 6, PE_ME, 320, hdr, body, sizeof body);
	CHECK_EQ(n, pe_render(CI_PE_DEVICE_INFO, &fake_s, "TEST0001", want, sizeof want));
	CHECK_MEM(body, want, n);
	CHECK(strstr(body, "\"familyId\":[81,0]") != 0);
	CHECK(strstr(body, "\"modelId\":[0,0]") != 0);
	CHECK(strstr(body, "\"versionId\":[0,1,0,0]") != 0);
	CHECK(strstr(body, "\"serialNumber\":\"TEST0001\"") != 0);

	/* errors */
	pe_send(0x34, 7, "{\"resource\":\"ProgramList\"}", "");
	host_in_pe();
	pe_reply(0x35, 7, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":404}") == 0);
	pe_send(0x34, 8, "{\"resource\":\"X-ScaleSettings\"}", "");	/* nothing bound */
	host_in_pe();
	pe_reply(0x35, 8, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":404}") == 0);
	pe_send(0x34, 9, "{\"resourceId\":\"x\"}", "");
	host_in_pe();
	pe_reply(0x35, 9, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x34, 10, "{\"resource\":", "");
	host_in_pe();
	pe_reply(0x35, 10, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x34, 12, "{\"resource\":123}", "");
	host_in_pe();
	pe_reply(0x35, 12, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x34, 13, "{\"resource\":null}", "");
	host_in_pe();
	pe_reply(0x35, 13, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x34, 14, "{\"resource\":[\"DeviceInfo\"]}", "");
	host_in_pe();
	pe_reply(0x35, 14, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send_from(PE_ME, 0x34, 11, "{\"resource\":\"DeviceInfo\"}", 2, 1, "", 0);
	host_in_pe();
	pe_reply(0x35, 11, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
}

static void test_ci_pe_small_initiator_and_terminate(void)
{
	static char hdr[64], body[4096], want[4096];
	uint16_t n;

	/* an initiator that takes 128-byte SysEx gets 128-byte chunks */
	pe_setup(128, 1);
	pe_send(0x34, 1, "{\"resource\":\"X-ScaleList\"}", "");
	host_in_pe();
	n = pe_reply(0x35, 1, PE_ME, 128, hdr, body, sizeof body);
	CHECK(pe_count > 2);
	CHECK_EQ(n, pe_render(CI_PE_SCALE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	/* Notify 144 (terminate) stops a reply part way */
	pe_setup(4096, 1);
	pe_send(0x34, 2, "{\"resource\":\"ResourceList\"}", "");
	pe_send(0x3F, 2, "{\"status\":144}", "");
	host_in_pe();
	struct pe_view v;
	pe_view(&pe_rx[0], &v);
	CHECK(pe_count >= 1);
	CHECK(pe_count < v.count);
	for (int i = 0; i < 3; i++)
		__wrap_midi_parsers_poll();
	host_in_pe();
	CHECK_EQ(pe_count, 0);
}

static void test_ci_pe_set(void)
{
	static char hdr[64], body[512], want[512];
	struct settings before;

	pe_setup(4096, 1);
	fake_s.octave = 2;
	/* Get the bound settings */
	pe_send(0x34, 1, "{\"resource\":\"X-PlayOptions\"}", "");
	host_in_pe();
	uint16_t n = pe_reply(0x35, 1, PE_ME, 320, hdr, body, sizeof body);
	CHECK_EQ(n, pe_render(CI_PE_PLAY_OPTIONS, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	/* a full Set: given fields change, the rest stay */
	pe_send(0x36, 2, "{\"resource\":\"X-ScaleSettings\"}", "{\"root\":5,\"layout\":\"inKey3rds\"}");
	host_in_pe();
	CHECK_EQ(pe_count, 1);
	pe_reply(0x37, 2, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":200}") == 0);
	CHECK_EQ(fake_applies, 1);
	CHECK_EQ(fake_s.root, 5);
	CHECK_EQ(fake_s.layout, 1);
	CHECK_EQ(fake_s.octave, 2);

	/* rejected Sets change nothing */
	before = fake_s;
	pe_send(0x36, 3, "{\"resource\":\"X-ScaleSettings\"}", "{\"root\":4,\"octave\":9}");
	host_in_pe();
	pe_reply(0x37, 3, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x36, 4, "{\"resource\":\"X-ScaleSettings\",\"setPartial\":true}", "{\"root\":4}");
	host_in_pe();
	pe_reply(0x37, 4, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x36, 5, "{\"resource\":\"DeviceInfo\"}", "{}");
	host_in_pe();
	pe_reply(0x37, 5, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":405}") == 0);
	pe_send(0x36, 6, "{\"resource\":\"X-Nope\"}", "{}");
	host_in_pe();
	pe_reply(0x37, 6, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":404}") == 0);
	CHECK_EQ(fake_applies, 1);
	CHECK(memcmp(&fake_s, &before, sizeof before) == 0);

	/* a Set in two chunks */
	const char *a = "{\"channel\":12,\"velocity";
	const char *b = "Mode\":\"fixed\"}";
	pe_send_from(PE_ME, 0x36, 7, "{\"resource\":\"X-PlayOptions\"}", 2, 1, a, (uint16_t)strlen(a));
	host_in_pe();
	CHECK_EQ(pe_count, 0);
	pe_send_from(PE_ME, 0x36, 7, "", 2, 2, b, (uint16_t)strlen(b));
	host_in_pe();
	pe_reply(0x37, 7, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":200}") == 0);
	CHECK_EQ(fake_s.channel, 11);
	CHECK_EQ(fake_s.velocity_mode, VEL_FIXED);

	/* a body over 256 bytes: 413, nothing applied */
	static char big[160];
	memset(big, ' ', sizeof big);
	big[0] = '{';
	pe_send_from(PE_ME, 0x36, 8, "{\"resource\":\"X-PlayOptions\"}", 2, 1, big, sizeof big);
	big[sizeof big - 1] = '}';
	pe_send_from(PE_ME, 0x36, 8, "", 2, 2, big + 1, sizeof big - 1);
	host_in_pe();
	pe_reply(0x37, 8, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":413}") == 0);

	/* non-string "resource" in Set returns 400 and changes nothing */
	before = fake_s;
	pe_send(0x36, 10, "{\"resource\":123}", "{\"root\":4}");
	host_in_pe();
	pe_reply(0x37, 10, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x36, 11, "{\"resource\":null}", "{\"root\":4}");
	host_in_pe();
	pe_reply(0x37, 11, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x36, 12, "{\"resource\":[\"DeviceInfo\"]}", "{\"root\":4}");
	host_in_pe();
	pe_reply(0x37, 12, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	CHECK(memcmp(&fake_s, &before, sizeof before) == 0);

	/* a chunk out of order */
	pe_send_from(PE_ME, 0x36, 9, "", 2, 2, b, (uint16_t)strlen(b));
	host_in_pe();
	pe_reply(0x37, 9, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	CHECK_EQ(fake_applies, 2);

	midici_pe_bind(0, 0);
}

static unsigned pe_subscribe(const uint8_t from[4], uint8_t req, const char *resource)
{
	static char hdr[64], body[8], start[64];
	unsigned id = 0;
	snprintf(start, sizeof start, "{\"command\":\"start\",\"resource\":\"%s\"}", resource);
	pe_send_from(from, 0x38, req, start, 1, 1, "", 0);
	host_in_pe();
	pe_reply(0x39, req, from, 320, hdr, body, sizeof body);
	CHECK(sscanf(hdr, "{\"status\":200,\"subscribeId\":\"lp%u\"}", &id) == 1);
	return id;
}

/* the one full notify of res to `to`, or none */
static void pe_expect_notify(const uint8_t to[4], unsigned id, uint8_t res)
{
	static char hdr[64], body[512], want[512], expect[64];
	host_in_pe();
	if (!to) {
		CHECK_EQ(pe_count, 0);
		return;
	}
	struct pe_view v;
	CHECK(pe_count >= 1);
	pe_view(&pe_rx[0], &v);
	uint16_t n = pe_reply(0x38, v.req, to, 320, hdr, body, sizeof body);
	snprintf(expect, sizeof expect, "{\"command\":\"full\",\"subscribeId\":\"lp%u\"}", id);
	CHECK(strcmp(hdr, expect) == 0);
	CHECK_EQ(n, pe_render(res, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);
}

/* X-SurfaceSettings end to end: Get, a Set from another initiator applied
 * like a menu change, and the subscriber's notify */
static void test_ci_pe_surface_settings(void)
{
	static char hdr[64], body[512], want[512];
	unsigned id;
	uint16_t n;

	pe_setup(4096, 1);
	pe_send(0x34, 1, "{\"resource\":\"X-SurfaceSettings\"}", "");
	host_in_pe();
	n = pe_reply(0x35, 1, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":200}") == 0);
	CHECK_EQ(n, pe_render(CI_PE_SURFACE_SETTINGS, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	id = pe_subscribe(PE_ME, 2, "X-SurfaceSettings");
	CHECK(id != 0);
	{
		const char *set = "{\"splitMode\":\"y\",\"upperPalette\":\"glacier\",\"upperChannel\":5}";

		pe_send_from(PE_OTHER, 0x36, 3, "{\"resource\":\"X-SurfaceSettings\"}", 1, 1, set,
		             (uint16_t)strlen(set));
	}
	host_in_pe();
	CHECK(pe_count >= 2);
	CHECK_EQ(pe_rx[0].buf[4], 0x37);
	CHECK_EQ(fake_applies, 1);
	CHECK_EQ(fake_s.split_mode, SPLIT_Y);
	CHECK_EQ(fake_s.upper_palette, PALETTE_GLACIER);
	CHECK_EQ(fake_s.upper_channel, 4);
	CHECK_EQ(pe_rx[1].buf[4], 0x38);
	CHECK_MEM(pe_rx[1].buf + 10, PE_ME, 4);

	/* a palette change on the device notifies too */
	fake_s.palette = PALETTE_CITRUS;
	pe_expect_notify(PE_ME, id, CI_PE_SURFACE_SETTINGS);

	/* the ChannelList now lists both halves */
	pe_send(0x34, 4, "{\"resource\":\"ChannelList\"}", "");
	host_in_pe();
	n = pe_reply(0x35, 4, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strstr(body, "\"Standalone Right\",\"channel\":5") != 0);
}

static void test_ci_pe_subscribe(void)
{
	static char hdr[64], body[8];
	unsigned id;

	pe_setup(4096, 1);
	id = pe_subscribe(PE_ME, 1, "X-PlayOptions");
	CHECK(id != 0);
	pe_expect_notify(0, 0, 0);

	/* a change on the device (the menu) notifies the subscriber */
	fake_s.channel = 3;
	pe_expect_notify(PE_ME, id, CI_PE_PLAY_OPTIONS);
	/* a Scale change does not touch PlayOptions */
	fake_s.root = 7;
	pe_expect_notify(0, 0, 0);

	/* the subscriber's own Set gets its reply and no notify */
	pe_send(0x36, 2, "{\"resource\":\"X-PlayOptions\"}", "{\"dinMirror\":true}");
	host_in_pe();
	CHECK_EQ(pe_count, 1);
	CHECK_EQ(pe_rx[0].buf[4], 0x37);

	/* a Set from another initiator does notify it */
	pe_send_from(PE_OTHER, 0x36, 3, "{\"resource\":\"X-PlayOptions\"}", 1, 1, "{\"dinMirror\":false}", 19);
	host_in_pe();
	CHECK(pe_count >= 2);
	CHECK_EQ(pe_rx[0].buf[4], 0x37);
	CHECK_MEM(pe_rx[0].buf + 10, PE_OTHER, 4);
	CHECK_EQ(pe_rx[1].buf[4], 0x38);
	CHECK_MEM(pe_rx[1].buf + 10, PE_ME, 4);

	/* end by subscribeId */
	snprintf(hdr, sizeof hdr, "{\"command\":\"end\",\"subscribeId\":\"lp%u\"}", id);
	pe_send(0x38, 4, hdr, "");
	host_in_pe();
	pe_reply(0x39, 4, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":200}") == 0);
	fake_s.channel = 4;
	pe_expect_notify(0, 0, 0);

	/* the latest subscriber wins */
	pe_subscribe(PE_ME, 5, "X-ScaleSettings");
	id = pe_subscribe(PE_OTHER, 6, "X-ScaleSettings");
	fake_s.octave = 6;
	pe_expect_notify(PE_OTHER, id, CI_PE_SCALE_SETTINGS);

	/* a read-only resource, unknown resource, or unknown command */
	pe_send(0x38, 7, "{\"command\":\"start\",\"resource\":\"DeviceInfo\"}", "");
	host_in_pe();
	pe_reply(0x39, 7, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":405}") == 0);
	pe_send(0x38, 10, "{\"command\":\"start\",\"resource\":\"ResourceList\"}", "");
	host_in_pe();
	pe_reply(0x39, 10, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":405}") == 0);
	pe_send(0x38, 11, "{\"command\":\"start\",\"resource\":\"X-Nope\"}", "");
	host_in_pe();
	pe_reply(0x39, 11, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":404}") == 0);
	pe_send(0x38, 12, "{\"command\":\"start\"}", "");
	host_in_pe();
	pe_reply(0x39, 12, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x38, 13, "{\"command\":\"start\",\"resource\":123}", "");
	host_in_pe();
	pe_reply(0x39, 13, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x38, 14, "{\"command\":\"start\",\"resource\":null}", "");
	host_in_pe();
	pe_reply(0x39, 14, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x38, 15, "{\"command\":\"start\",\"resource\":[\"X-PlayOptions\"]}", "");
	host_in_pe();
	pe_reply(0x39, 15, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);
	pe_send(0x38, 8, "{\"command\":\"full\",\"subscribeId\":\"lp1\"}", "");
	host_in_pe();
	pe_reply(0x39, 8, PE_ME, 320, hdr, body, sizeof body);
	CHECK(strcmp(hdr, "{\"status\":400}") == 0);

	/* invalidating the subscriber's MUID drops its subscription */
	{
		uint8_t invalidate[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 1, 2, 3, 4,
			0x7F, 0x7F, 0x7F, 0x7F, 5, 6, 7, 8, 0xF7};
		host_out_sysex(0, invalidate, sizeof invalidate);
	}
	fake_s.octave = 1;
	pe_expect_notify(0, 0, 0);

	/* a USB reset drops every subscription */
	id = pe_subscribe(PE_ME, 9, "X-ScaleSettings");
	enter_alt1();
	{
		struct ump_sysex7_rx reply;
		host_out_sysex(0, ci_discovery, sizeof ci_discovery);
		host_in_ci(&reply, 0);
		memcpy(pe_muid, reply.buf + 6, 4);
	}
	fake_s.octave = 3;
	pe_expect_notify(0, 0, 0);

	midici_pe_bind(0, 0);
}

/* Drains currently queued USB IN packets without running __wrap_midi_parsers_poll(). */
static void host_in_pe_no_poll(void)
{
	struct ump_sysex7_rx cur;
	uint32_t words[16];
	uint16_t count;
	pe_count = 0;
	ump_sysex7_rx_reset(&cur);
	while ((count = host_in(words)) > 0) {
		for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
			if (words[i] >> 28 != 3)
				continue;
			CHECK_EQ((words[i] >> 24) & 0xF, 0);
			int r = ump_sysex7_rx_feed(&cur, words + i);
			CHECK(r >= 0);
			if (r == 1) {
				CHECK(pe_count < PE_MSGS);
				if (pe_count < PE_MSGS)
					pe_rx[pe_count++] = cur;
				ump_sysex7_rx_reset(&cur);
			}
		}
	}
	CHECK_EQ(cur.active, 0);
}

static void send_discovery_from(const uint8_t src[4], uint32_t max_sysex, struct ump_sysex7_rx *reply)
{
	uint8_t d[sizeof(ci_discovery)];
	memcpy(d, ci_discovery, sizeof d);
	memcpy(d + 6, src, 4);
	d[26] = (uint8_t)(max_sysex & 0x7F);
	d[27] = (uint8_t)((max_sysex >> 7) & 0x7F);
	d[28] = (uint8_t)((max_sysex >> 14) & 0x7F);
	d[29] = (uint8_t)((max_sysex >> 21) & 0x7F);
	host_out_sysex(0, d, sizeof d);
	host_in_ci(reply, 0);
	CHECK_EQ(reply->len, 33);
	CHECK_EQ(reply->buf[4], 0x71);
}

static void test_ci_pe_one_chunk_per_pass_and_backpressure(void)
{
	static char body[4096], want[4096];
	struct pe_view v;
	uint16_t total_chunks, blen = 0;

	/* Case A: strictly 1 chunk emitted per __wrap_midi_parsers_poll() / midici_responder_poll() pass */
	pe_setup(4096, 1);
	pe_send(0x34, 1, "{\"resource\":\"ResourceList\"}", "");
	host_in_pe_no_poll();
	CHECK_EQ(pe_count, 1);
	pe_view(&pe_rx[0], &v);
	CHECK_EQ(v.type, 0x35);
	CHECK_EQ(v.req, 1);
	CHECK_EQ(v.num, 1);
	CHECK(v.count > 2);
	total_chunks = v.count;
	memcpy(body + blen, v.data, v.data_len);
	blen = (uint16_t)(blen + v.data_len);

	for (uint16_t c = 2; c <= total_chunks; c++) {
		host_in_pe_no_poll();
		CHECK_EQ(pe_count, 0);
		if (c & 1)
			midici_responder_poll();
		else
			__wrap_midi_parsers_poll();
		host_in_pe_no_poll();
		CHECK_EQ(pe_count, 1);
		pe_view(&pe_rx[0], &v);
		CHECK_EQ(v.num, c);
		CHECK_EQ(v.count, total_chunks);
		if (blen + v.data_len <= sizeof body) {
			memcpy(body + blen, v.data, v.data_len);
			blen = (uint16_t)(blen + v.data_len);
		}
	}
	__wrap_midi_parsers_poll();
	host_in_pe_no_poll();
	CHECK_EQ(pe_count, 0);
	CHECK_EQ(blen, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, blen);

	/* Case B: CiTxQueue backpressure stalls cleanly and retries the exact chunk without drops */
	pe_setup(128, 1);
	pe_send(0x34, 2, "{\"resource\":\"X-ScaleList\"}", "");
	host_in_pe_no_poll();
	CHECK_EQ(pe_count, 1);
	pe_view(&pe_rx[0], &v);
	CHECK_EQ(v.num, 1);
	CHECK(v.count > 2);
	total_chunks = v.count;
	blen = 0;
	memcpy(body + blen, v.data, v.data_len);
	blen = (uint16_t)(blen + v.data_len);

	{
		uint8_t fill60[60];
		uint8_t fill6[6];
		memset(fill60, 0x01, sizeof fill60);
		memset(fill6, 0x02, sizeof fill6);
		while (midici_tx_free_packets() >= 25)
			CHECK_EQ(midici_send_sysex(1, fill60, sizeof fill60), 0);
		while (midici_tx_free_packets() >= 18)
			CHECK_EQ(midici_send_sysex(1, fill6, sizeof fill6), 0);
	}
	{
		const uint32_t ovf0 = midici_tx_overflows();
		const uint16_t pend0 = midici_tx_pending();
		__wrap_midi_parsers_poll();
		__wrap_midi_parsers_poll();
		CHECK_EQ(midici_tx_overflows(), ovf0);
		CHECK_EQ(midici_tx_pending(), pend0);
	}
	/* Drain group 1 filler only; verify no group 0 chunk leaked while stalled */
	{
		uint32_t words[16];
		uint16_t count;
		while ((count = host_in(words)) > 0) {
			for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
				if (words[i] >> 28 == 3)
					CHECK_EQ((words[i] >> 24) & 0xF, 1);
			}
		}
	}
	for (uint16_t c = 2; c <= total_chunks; c++) {
		__wrap_midi_parsers_poll();
		host_in_pe_no_poll();
		CHECK_EQ(pe_count, 1);
		pe_view(&pe_rx[0], &v);
		CHECK_EQ(v.num, c);
		CHECK_EQ(v.count, total_chunks);
		if (blen + v.data_len <= sizeof body) {
			memcpy(body + blen, v.data, v.data_len);
			blen = (uint16_t)(blen + v.data_len);
		}
	}
	CHECK_EQ(blen, pe_render(CI_PE_SCALE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, blen);
	midici_pe_bind(0, 0);
}

static void test_ci_pe_multi_muid_max_sysex_cache(void)
{
	static char hdr[64], body[4096], want[4096];
	static const uint8_t PE_UNK[4] = {9, 10, 11, 12};
	static const uint8_t PE_C[4] = {13, 14, 15, 16};
	struct ump_sysex7_rx reply;
	uint16_t n;

	/* Peer A (128) then Peer B (4096): each retains its own maxSysEx */
	enter_alt1();
	settings_defaults(&fake_s);
	midici_pe_bind(fake_get, fake_apply);
	send_discovery_from(PE_ME, 128, &reply);
	memcpy(pe_muid, reply.buf + 6, 4);
	send_discovery_from(PE_OTHER, 4096, &reply);

	pe_send_from(PE_ME, 0x34, 1, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
	host_in_pe();
	CHECK(pe_count > 2);
	n = pe_reply(0x35, 1, PE_ME, 128, hdr, body, sizeof body);
	CHECK_EQ(n, pe_render(CI_PE_SCALE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	pe_send_from(PE_OTHER, 0x34, 2, "{\"resource\":\"ResourceList\"}", 1, 1, "", 0);
	host_in_pe();
	CHECK(pe_count > 1);
	CHECK(pe_rx[0].len > 128);
	n = pe_reply(0x35, 2, PE_OTHER, 320, hdr, body, sizeof body);
	CHECK_EQ(n, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	/* Unknown MUID defaults to 512 (clamped to CI_PE_MESSAGE_MAX = 320, > 128) */
	pe_send_from(PE_UNK, 0x34, 3, "{\"resource\":\"ResourceList\"}", 1, 1, "", 0);
	host_in_pe();
	CHECK(pe_count > 1);
	CHECK(pe_rx[0].len > 128);
	n = pe_reply(0x35, 3, PE_UNK, 320, hdr, body, sizeof body);
	CHECK_EQ(n, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
	CHECK_MEM(body, want, n);

	/* InitiatorMuidInvalidated clears only the targeted peer's entry */
	send_discovery_from(PE_C, 160, &reply);
	{
		uint8_t inv[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 5, 6, 7, 8,
			0x7F, 0x7F, 0x7F, 0x7F, PE_ME[0], PE_ME[1], PE_ME[2], PE_ME[3], 0xF7};
		host_out_sysex(0, inv, sizeof inv);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);
	}
	pe_send_from(PE_ME, 0x34, 4, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
	host_in_pe();
	CHECK(pe_rx[0].len > 128);
	pe_reply(0x35, 4, PE_ME, 320, hdr, body, sizeof body);

	pe_send_from(PE_C, 0x34, 5, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
	host_in_pe();
	pe_reply(0x35, 5, PE_C, 160, hdr, body, sizeof body);

	/* SourceMuid == 0 Discovery (4096) after 128-byte peer does not poison peer_cache or break forget_initiator */
	{
		static const uint8_t PE_ZERO[4] = {0, 0, 0, 0};
		static const uint8_t PE_SMALL[4] = {0x21, 0x22, 0x23, 0x24};
		enter_alt1();
		settings_defaults(&fake_s);
		midici_pe_bind(fake_get, fake_apply);
		send_discovery_from(PE_ME, 128, &reply);
		memcpy(pe_muid, reply.buf + 6, 4);
		send_discovery_from(PE_ZERO, 4096, &reply);
		CHECK_MEM(reply.buf + 10, PE_ZERO, 4);

		/* PE_ME (behind PE_ZERO in peer_cache) still gets <= 128-byte chunks */
		pe_send_from(PE_ME, 0x34, 21, "{\"resource\":\"DeviceInfo\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK_EQ(pe_count, 3);
		n = pe_reply(0x35, 21, PE_ME, 128, hdr, body, sizeof body);
		CHECK_EQ(n, pe_render(CI_PE_DEVICE_INFO, &fake_s, "TEST0001", want, sizeof want));
		CHECK_MEM(body, want, n);

		/* SourceMuid == 0 works cleanly with its own 4096 -> 320 limit */
		pe_send_from(PE_ZERO, 0x34, 22, "{\"resource\":\"ResourceList\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_count > 1);
		CHECK(pe_rx[0].len > 128);
		n = pe_reply(0x35, 22, PE_ZERO, 320, hdr, body, sizeof body);
		CHECK_EQ(n, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
		CHECK_MEM(body, want, n);

		/* With PE_ZERO at peer_cache[0], InitiatorMuidInvalidated for PE_ME still removes PE_ME */
		{
			uint8_t inv_me[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 5, 6, 7, 8,
				0x7F, 0x7F, 0x7F, 0x7F, PE_ME[0], PE_ME[1], PE_ME[2], PE_ME[3], 0xF7};
			host_out_sysex(0, inv_me, sizeof inv_me);
			host_in_ci(&reply, 0);
			CHECK_EQ(reply.len, 0);
		}
		pe_send_from(PE_ME, 0x34, 23, "{\"resource\":\"DeviceInfo\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK_EQ(pe_count, 1);
		CHECK(pe_rx[0].len > 128);
		pe_reply(0x35, 23, PE_ME, 320, hdr, body, sizeof body);

		/* InitiatorMuidInvalidated for PE_ZERO removes PE_ZERO without disturbing PE_C (160) */
		send_discovery_from(PE_C, 160, &reply);
		send_discovery_from(PE_ZERO, 128, &reply);
		{
			uint8_t inv_zero[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 5, 6, 7, 8,
				0x7F, 0x7F, 0x7F, 0x7F, 0, 0, 0, 0, 0xF7};
			host_out_sysex(0, inv_zero, sizeof inv_zero);
			host_in_ci(&reply, 0);
			CHECK_EQ(reply.len, 0);
		}
		pe_send_from(PE_ZERO, 0x34, 24, "{\"resource\":\"DeviceInfo\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK_EQ(pe_count, 1);
		CHECK(pe_rx[0].len > 128);
		pe_reply(0x35, 24, PE_ZERO, 320, hdr, body, sizeof body);
		pe_send_from(PE_C, 0x34, 25, "{\"resource\":\"DeviceInfo\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_count > 1);
		pe_reply(0x35, 25, PE_C, 160, hdr, body, sizeof body);

		/* max_sysex < 128 (e.g. 64) falls back safely to 512 clamped by CI_PE_MESSAGE_MAX (320) */
		send_discovery_from(PE_SMALL, 64, &reply);
		pe_send_from(PE_SMALL, 0x34, 26, "{\"resource\":\"ResourceList\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_count > 1);
		CHECK(pe_rx[0].len > 128);
		n = pe_reply(0x35, 26, PE_SMALL, 320, hdr, body, sizeof body);
		CHECK_EQ(n, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
		CHECK_MEM(body, want, n);
	}

	/* > 8 peers: LRU entry is evicted (and active lookup promotes to MRU) */
	enter_alt1();
	midici_pe_bind(fake_get, fake_apply);
	{
		uint8_t peers[9][4];
		for (int i = 0; i < 9; i++) {
			peers[i][0] = (uint8_t)(0x11 + i);
			peers[i][1] = 1;
			peers[i][2] = 1;
			peers[i][3] = 1;
		}
		send_discovery_from(peers[0], 128, &reply);
		memcpy(pe_muid, reply.buf + 6, 4);
		for (int i = 1; i < 8; i++)
			send_discovery_from(peers[i], 160, &reply);

		/* Touch peers[0] (128) so it moves to MRU, making peers[1] (160) the LRU */
		pe_send_from(peers[0], 0x34, 6, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
		host_in_pe();
		pe_reply(0x35, 6, peers[0], 128, hdr, body, sizeof body);

		/* 9th peer evicts peers[1] while peers[0] stays cached */
		send_discovery_from(peers[8], 192, &reply);
		pe_send_from(peers[8], 0x34, 7, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_rx[0].len > 160);
		pe_reply(0x35, 7, peers[8], 192, hdr, body, sizeof body);

		pe_send_from(peers[0], 0x34, 8, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
		host_in_pe();
		pe_reply(0x35, 8, peers[0], 128, hdr, body, sizeof body);

		pe_send_from(peers[1], 0x34, 9, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_rx[0].len > 160);
		pe_reply(0x35, 9, peers[1], 320, hdr, body, sizeof body);

		/* MuidCollision clears the entire cache */
		{
			uint8_t col[sizeof(ci_discovery)];
			memcpy(col, ci_discovery, sizeof col);
			memcpy(col + 6, pe_muid, 4);
			host_out_sysex(0, col, sizeof col);
			host_in_ci(&reply, 1);
			CHECK_EQ(reply.buf[4], 0x7E);
		}
		send_discovery_from(PE_OTHER, 4096, &reply);
		memcpy(pe_muid, reply.buf + 6, 4);
		pe_send_from(peers[0], 0x34, 10, "{\"resource\":\"X-ScaleList\"}", 1, 1, "", 0);
		host_in_pe();
		CHECK(pe_rx[0].len > 128);
		pe_reply(0x35, 10, peers[0], 320, hdr, body, sizeof body);
	}
	midici_pe_bind(0, 0);
}

static void test_ci_non_discovery_self_echo_ignored(void)
{
	static char hdr[64], body[4096], want[4096];
	struct ump_sysex7_rx reply;
	unsigned id;

	pe_setup(4096, 1);
	id = pe_subscribe(PE_ME, 1, "X-PlayOptions");
	CHECK(id != 0);

	/* Phase 1: idle responder ignores every non-Discovery self-echo (0 replies, 0 NAKs, 0 alerts) */
	{
		uint8_t ep_ok[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x72, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};
		uint8_t ep_bad_ver[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x72, 0x22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};
		uint8_t ep_short[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x72, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};
		uint8_t prof[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x20, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7};
		uint8_t caps[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x30, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0xF7};
		uint8_t inv_self[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 0, 0, 0, 0,
			0x7F, 0x7F, 0x7F, 0x7F, 0, 0, 0, 0, 0xF7};
		uint8_t inv_peer[] = {0xF0, 0x7E, 0x7F, 0x0D, 0x7E, 2, 0, 0, 0, 0,
			0x7F, 0x7F, 0x7F, 0x7F, PE_ME[0], PE_ME[1], PE_ME[2], PE_ME[3], 0xF7};
		uint8_t d_reply[33];

		memcpy(ep_ok + 6, pe_muid, 4); memcpy(ep_ok + 10, pe_muid, 4);
		host_out_sysex(0, ep_ok, sizeof ep_ok); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(ep_bad_ver + 6, pe_muid, 4); memcpy(ep_bad_ver + 10, pe_muid, 4);
		host_out_sysex(0, ep_bad_ver, sizeof ep_bad_ver); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(ep_short + 6, pe_muid, 4); memcpy(ep_short + 10, pe_muid, 4);
		host_out_sysex(0, ep_short, sizeof ep_short); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(prof + 6, pe_muid, 4); memcpy(prof + 10, pe_muid, 4);
		host_out_sysex(0, prof, sizeof prof); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(caps + 6, pe_muid, 4); memcpy(caps + 10, pe_muid, 4);
		host_out_sysex(0, caps, sizeof caps); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(inv_self + 6, pe_muid, 4); memcpy(inv_self + 14, pe_muid, 4);
		host_out_sysex(0, inv_self, sizeof inv_self); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memcpy(inv_peer + 6, pe_muid, 4);
		host_out_sysex(0, inv_peer, sizeof inv_peer); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);

		memset(d_reply, 0, sizeof d_reply);
		d_reply[0] = 0xF0; d_reply[1] = 0x7E; d_reply[2] = 0x7F; d_reply[3] = 0x0D;
		d_reply[4] = 0x71; d_reply[5] = 2;
		memcpy(d_reply + 6, pe_muid, 4); memcpy(d_reply + 10, PE_ME, 4);
		d_reply[32] = 0xF7;
		host_out_sysex(0, d_reply, sizeof d_reply); host_in_ci(&reply, 0); CHECK_EQ(reply.len, 0);
	}

	/* Phase 2: non-Discovery PE self-echoes (0x34/0x36/0x38) do not corrupt active out or subs */
	pe_send(0x34, 2, "{\"resource\":\"ResourceList\"}", "");
	host_in_pe_no_poll();
	CHECK_EQ(pe_count, 1);
	{
		struct pe_view v;
		uint16_t total_chunks, blen = 0;
		pe_view(&pe_rx[0], &v);
		CHECK_EQ(v.num, 1);
		total_chunks = v.count;
		memcpy(body + blen, v.data, v.data_len);
		blen = (uint16_t)(blen + v.data_len);

		pe_send_from(pe_muid, 0x34, 9, "{\"resource\":\"DeviceInfo\"}", 1, 1, "", 0);
		pe_send_from(pe_muid, 0x36, 10, "{\"resource\":\"X-PlayOptions\"}", 1, 1, "{\"channel\":9}", 13);
		pe_send_from(pe_muid, 0x38, 11, "{\"command\":\"start\",\"resource\":\"X-PlayOptions\"}", 1, 1, "", 0);
		CHECK_EQ(fake_applies, 0);

		/* Drain remaining chunks; every packet must belong to PE_ME's ResourceList reply */
		{
			struct ump_sysex7_rx cur;
			uint32_t words[16];
			uint16_t count, expected = 2;
			ump_sysex7_rx_reset(&cur);
			for (;;) {
				if ((count = host_in(words)) == 0) {
					__wrap_midi_parsers_poll();
					if ((count = host_in(words)) == 0)
						break;
				}
				for (uint16_t i = 0; i < count; i += ump_word_count(words[i])) {
					CHECK(words[i] != 0xF0118033u);
					if (words[i] >> 28 != 3)
						continue;
					int r = ump_sysex7_rx_feed(&cur, words + i);
					CHECK(r >= 0);
					if (r == 1) {
						pe_view(&cur, &v);
						CHECK_EQ(v.type, 0x35);
						CHECK_EQ(v.req, 2);
						CHECK_MEM(v.dst, PE_ME, 4);
						CHECK_EQ(v.num, expected++);
						if (blen + v.data_len <= sizeof body) {
							memcpy(body + blen, v.data, v.data_len);
							blen = (uint16_t)(blen + v.data_len);
						}
						ump_sysex7_rx_reset(&cur);
					}
				}
			}
			CHECK_EQ(expected - 1, total_chunks);
		}
		CHECK_EQ(blen, pe_render(CI_PE_RESOURCE_LIST, &fake_s, "", want, sizeof want));
		CHECK_MEM(body, want, blen);
	}
	(void)hdr;

	/* PE_ME's subscription and responder MUID remain intact */
	fake_s.channel = 5;
	pe_expect_notify(PE_ME, id, CI_PE_PLAY_OPTIONS);
	ci_discover(&reply);
	CHECK_MEM(reply.buf + 6, pe_muid, 4);

	/* Malformed or reserved-version Discovery with SourceMuid == responder.Muid() emits 0 replies / 0 NAKs / 0 FB0 alerts */
	{
		uint8_t bad_disc[sizeof(ci_discovery)];
		memcpy(bad_disc, ci_discovery, sizeof bad_disc);
		memcpy(bad_disc + 6, pe_muid, 4);
		bad_disc[5] = 0x22; /* reserved version */
		host_out_sysex(0, bad_disc, sizeof bad_disc);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);

		bad_disc[5] = 0x00; /* zero version */
		host_out_sysex(0, bad_disc, sizeof bad_disc);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);

		bad_disc[5] = 0x02;
		bad_disc[sizeof(bad_disc) - 2] = 0xF7; /* truncated v1.2 (29B payload < 30B) */
		host_out_sysex(0, bad_disc, sizeof(bad_disc) - 1);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);

		bad_disc[5] = 0x01;
		bad_disc[sizeof(bad_disc) - 3] = 0xF7; /* truncated v1.1 (28B payload < 29B) */
		host_out_sysex(0, bad_disc, sizeof(bad_disc) - 2);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);

		bad_disc[5] = 0x02;
		bad_disc[14] = 0xF7; /* 13B header-only Discovery */
		host_out_sysex(0, bad_disc, 15);
		host_in_ci(&reply, 0);
		CHECK_EQ(reply.len, 0);

		ci_discover(&reply);
		CHECK_MEM(reply.buf + 6, pe_muid, 4);
	}

	/* Discovery (0x70) with SourceMuid == responder.Muid() still triggers MuidCollision */
	{
		uint8_t collision[sizeof(ci_discovery)];
		memcpy(collision, ci_discovery, sizeof collision);
		memcpy(collision + 6, pe_muid, 4);
		host_out_sysex(0, collision, sizeof collision);
		host_in_ci(&reply, 1);
		CHECK_EQ(reply.buf[4], 0x7E);
		CHECK_MEM(reply.buf + 14, pe_muid, 4);
		CHECK(memcmp(reply.buf + 6, pe_muid, 4) != 0);
	}
	midici_pe_bind(0, 0);
}

/* ---- EP1 OUT / RX ---- */

static void test_ep1_out_odd_length(void)
{
	uint8_t p1[11] = {0x00,0x3C,0x90,0x40, 0x00,0x00,0xFF,0xFF, 0xAA,0xBB,0xCC};
	uint8_t p2[8]  = {0x00,0x3E,0x90,0x40, 0x00,0x00,0x00,0x80};
	enter_alt1(); stub_rx_packet(p1, 11); EP1_OUT_Callback();
	CHECK_EQ(ump_rx_free_words(), UMP_RX_WORDS - 2);	/* trailing 3 bytes dropped */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub_midi_n, 1);
	CHECK_EQ(stub_midi_log[0].port, 0);
	CHECK_EQ(stub_midi_log[0].status, 0x90);
	CHECK_EQ(stub_midi_log[0].d1, 60);
	CHECK_EQ(stub_midi_log[0].d2, 127);
	CHECK_EQ(MIDIToDev.tail - MIDIToDev.head, 0);		/* not the legacy path */
	stub_rx_packet(p2, 8); EP1_OUT_Callback(); __wrap_midi_parsers_poll();
	CHECK_EQ(stub_midi_n, 2); CHECK_EQ(stub_midi_log[1].d1, 62);
	CHECK_EQ(stub_midi_log[1].d2, 64);
	CHECK_EQ(ump_rx_overflows(), 0);
}

static void test_ep1_out_leaves_nak_until_poll(void)
{
	static const uint32_t w[1] = {0x20903C40};
	enter_alt1();
	host_out(w, 1);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_NAK);
	CHECK_EQ(stub.ep_rx_valid_calls, 0);
	CHECK_EQ(stub_midi_n, 0);				/* dispatched in the main loop */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub_midi_n, 1);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_VALID);
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
	__wrap_midi_parsers_poll();				/* already VALID: no re-arm */
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
}

static void test_backpressure_nak_until_space(void)
{
	static const uint32_t zeros[16];			/* MT 0 NOOPs */
	int packets = 0;
	enter_alt1(); midi2_debug_pause_rx(1);
	/* feed 64-byte packets until ump_rx_free_words() < 16 */
	while (ump_rx_free_words() >= 16) {
		host_out(zeros, 16);
		packets++;
	}
	CHECK_EQ(packets, UMP_RX_WORDS / 16);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_NAK);
	__wrap_midi_parsers_poll(); CHECK_EQ(stub.ep_rx_valid_calls, 0);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_NAK);
	CHECK_EQ(stub.din_parse_calls, 1);			/* DIN still parsed */
	midi2_debug_pause_rx(0); __wrap_midi_parsers_poll(); CHECK_EQ(stub.ep_rx_valid_calls, 1);
	CHECK_EQ(ump_rx_free_words(), UMP_RX_WORDS);
	CHECK_EQ(ump_rx_overflows(), 0);
}

static void test_rearm_threshold(void)
{
	static const uint32_t zeros[16];
	int i;
	enter_alt1(); midi2_debug_pause_rx(1);
	for (i = 0; i < UMP_RX_WORDS / 16 - 1; i++)
		host_out(zeros, 16);
	CHECK_EQ(ump_rx_free_words(), 16);			/* exactly one packet's room */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
	host_out(zeros, 1);
	CHECK_EQ(ump_rx_free_words(), 15);			/* one word short */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
}

/* Alt 1 paces the host to the DIN wire rate as c_midi does in alt 0: EP1 OUT
 * is re-armed only once the DIN Out fifo has drained. */
static void test_rearm_waits_for_din_out(void)
{
	static const uint32_t w[1] = {0x21903C40};		/* group 1 → DIN */
	enter_alt1();
	host_out(w, 1);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_NAK);
	stub_din_out_queue(3);					/* 3 bytes queued for DIN Out */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub_midi_n, 1);				/* RX still dispatched */
	CHECK_EQ(stub.ep_rx_valid_calls, 0);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_NAK);
	CHECK_EQ(stub.din_parse_calls, 1);			/* DIN In still parsed */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub.ep_rx_valid_calls, 0);
	stub_din_out_drain();					/* DIN Out drained */
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
	CHECK_EQ(stub.ep_rx_status[1], EP_RX_VALID);
}

static void test_din_parsed_in_both_alts(void)
{
	stubs_reset(); usb_midi2_usb_reset(); __wrap_midi_parsers_poll();
	CHECK_EQ(stub.real_parsers_poll_calls, 1);
	CHECK_EQ(stub.din_parse_calls, 0);			/* real does it in alt 0 */
	enter_alt1(); __wrap_midi_parsers_poll();
	CHECK_EQ(stub.real_parsers_poll_calls, 0);
	CHECK_EQ(stub.din_parse_calls, 1); CHECK_EQ(stub.din_parse_last_port, 2);
	CHECK(stub.din_parse_last_parser == DinRx);
}

static void test_legacy_paths_gated_in_alt1(void)
{
	enter_alt1(); __wrap_usbmidi_poll(); __wrap_midi_flush_windows_buffer();
	CHECK_EQ(stub.real_usbmidi_poll_calls, 0); CHECK_EQ(stub.real_flush_calls, 0);
	enter_alt0(); __wrap_usbmidi_poll(); __wrap_midi_flush_windows_buffer();
	CHECK_EQ(stub.real_usbmidi_poll_calls, 1); CHECK_EQ(stub.real_flush_calls, 1);
}

/* Alt 0: c_midi's usbmidi_poll checks EP1 TX status and then arms it, so it
 * runs with interrupts off (a SET_INTERFACE in between must not let one legacy
 * packet out after set_alt NAKed EP1). */
static void test_alt0_usbmidi_poll_atomic(void)
{
	enter_alt0();
	stub.real_usbmidi_poll_irq_depth = -1;
	__wrap_usbmidi_poll();
	CHECK_EQ(stub.real_usbmidi_poll_calls, 1);
	CHECK(stub.real_usbmidi_poll_irq_depth > 0);
	CHECK_EQ(stub_irq_depth, 0);				/* balanced */
}

/* Alt 0: hal_send_* reach c_midi from the timer ISR and the main loop, so the
 * real call runs with interrupts masked. */
static void test_alt0_hal_send_masked(void)
{
	static const uint8_t syx[] = { 0xF0, 0x7D, 0x01, 0xF7 };
	enter_alt0();
	stub.real_midi_irq_depth = -1;
	stub.real_sysex_irq_depth = -1;
	__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 100);
	__wrap_hal_send_sysex(USBSTANDALONE, syx, sizeof syx);
	CHECK_EQ(stub.real_midi_calls, 1);
	CHECK_EQ(stub.real_sysex_calls, 1);
	CHECK(stub.real_midi_irq_depth > 0);
	CHECK(stub.real_sysex_irq_depth > 0);
	CHECK_EQ(stub_irq_depth, 0);
}

static void test_alt1_entry_runs_service_first(void)
{
	static const uint32_t w[1] = {0x20903C40};
	enter_alt0();
	usb_midi2_set_alt(1);					/* ISR */
	MIDIToDev.head = 1;
	MIDIToDev.tail = 5;
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub.fifo_flush_calls, 1);
	CHECK_EQ(stub.fifo1k_flush_calls, 1);
	CHECK_EQ(stub.ep_rx_valid_calls, 1);
	CHECK_EQ(stub.real_parsers_poll_calls, 0);
	host_out(w, 1);
	__wrap_midi_parsers_poll();
	CHECK_EQ(stub_midi_n, 1);
}

/* ---- EP1 IN / TX ---- */

static int hook_calls;
static void count_hook(void) { hook_calls++; }

/* the app's main-loop hook runs once per poll pass, in both USB modes */
static void test_main_loop_hook_runs_in_both_alts(void)
{
	hook_calls = 0;
	midi2_set_main_loop_hook(count_hook);
	enter_alt0(); __wrap_midi_parsers_poll();
	CHECK_EQ(hook_calls, 1);
	enter_alt1(); __wrap_midi_parsers_poll();
	CHECK_EQ(hook_calls, 2);
	midi2_set_main_loop_hook(0);
	__wrap_midi_parsers_poll();
	CHECK_EQ(hook_calls, 2);
}

static void test_usbmidi_poll_tx_conditions(void)
{
	uint32_t w[16];
	enter_alt1();
	__wrap_hal_send_midi(USBMIDI, 0xB0, 1, 2);
	bDeviceState = USB_STATE_ADDRESSED;			/* not configured: hold */
	CHECK_EQ(host_in(w), 0);
	bDeviceState = USB_STATE_CONFIGURED;
	stub.ep_tx_status[1] = EP_TX_VALID;			/* previous packet in flight */
	__wrap_usbmidi_poll();
	CHECK_EQ(stub.tx_writes, 0);
	CHECK_EQ(host_in(w), 1);
	CHECK_EQ(w[0], 0x21B00102u);
	CHECK_EQ(stub.ep_tx_status[1], EP_TX_VALID);
	CHECK_EQ(host_in(w), 0);				/* nothing queued: stays NAK */
	CHECK_EQ(stub.ep_tx_status[1], EP_TX_NAK);
	CHECK_EQ(stub.real_usbmidi_poll_calls, 0);
}

static void test_note_on2_paths(void)
{
	stubs_reset(); usb_midi2_usb_reset();                 /* alt 0 → 7-bit via real */
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x0100);
	CHECK_EQ(stub.real_midi_calls, 1);
	CHECK_EQ(stub.real_midi_last[0], USBSTANDALONE);
	CHECK_EQ(stub.real_midi_last[1], 0x90);
	CHECK_EQ(stub.real_midi_last[2], 60);
	CHECK_EQ(stub.real_midi_last[3], 1);                  /* clamped ≥ 1 */
	enter_alt1(); hal_send_note_on2(USBSTANDALONE, 0, 60, 0xABCD); __wrap_usbmidi_poll();
	CHECK_EQ(stub.last_tx_len, 8); CHECK_EQ(stub.last_tx[2], 0x90); CHECK_EQ(stub.last_tx[6], 0xCD);
	CHECK_EQ(stub.last_tx[7], 0xAB);
	CHECK_EQ(stub.last_tx[3], 0x40);
	stub.ep_tx_status[1] = EP_TX_NAK;			/* host took it */
	hal_send_note_on2(USBMIDI, 0, 60, 0xFFFF); __wrap_usbmidi_poll();
	CHECK_EQ(stub.last_tx_len, 4); CHECK_EQ(stub.last_tx[0], 0x7F);   /* MT 2, group 1 */
	CHECK_EQ(stub.last_tx[3], 0x21);
	/* DIN in alt 1: 7-bit through the real path */
	hal_send_note_on2(DINMIDI, 3, 61, 0x0000);
	CHECK_EQ(stub.real_midi_calls, 1);
	CHECK_EQ(stub.real_midi_last[0], DINMIDI);
	CHECK_EQ(stub.real_midi_last[1], 0x93);
	CHECK_EQ(stub.real_midi_last[2], 61);
	CHECK_EQ(stub.real_midi_last[3], 1);
	CHECK_EQ(ump_tx_reserve_words(), 4);			/* two accepted attacks */
}

static void test_note_on2_pitch_paths(void)
{
	stubs_reset(); usb_midi2_usb_reset();                 /* alt 0: plain 7-bit note */
	hal_send_note_on2_pitch(USBSTANDALONE, 0, 64, 0x8000, 0x7C00);
	CHECK_EQ(stub.real_midi_calls, 1);
	CHECK_EQ(stub.real_midi_last[1], 0x90);
	CHECK_EQ(stub.real_midi_last[2], 64);
	CHECK_EQ(stub.real_midi_last[3], 0x40);
	enter_alt1(); hal_send_note_on2_pitch(USBSTANDALONE, 2, 64, 0xABCD, 0x7C01); __wrap_usbmidi_poll();
	CHECK_EQ(stub.last_tx_len, 8);
	CHECK_EQ(stub.last_tx[0], 3);                         /* attribute: Pitch 7.9 */
	CHECK_EQ(stub.last_tx[1], 64);
	CHECK_EQ(stub.last_tx[2], 0x92);
	CHECK_EQ(stub.last_tx[4], 0x01); CHECK_EQ(stub.last_tx[5], 0x7C);
	CHECK_EQ(stub.last_tx[6], 0xCD); CHECK_EQ(stub.last_tx[7], 0xAB);
	stub.ep_tx_status[1] = EP_TX_NAK;
	hal_send_note_on2_pitch(DINMIDI, 3, 61, 0xFFFF, 0x7C00);  /* DIN: plain */
	CHECK_EQ(stub.real_midi_last[0], DINMIDI);
	CHECK_EQ(stub.real_midi_last[1], 0x93);
	CHECK_EQ(stub.real_midi_last[2], 61);
}

static void test_note_off2_and_reserve(void)
{
	uint32_t w[16];
	enter_alt0();
	hal_send_note_off2(USBSTANDALONE, 2, 64, 0xFFFF);
	CHECK_EQ(stub.real_midi_last[1], 0x82);
	CHECK_EQ(stub.real_midi_last[3], 127);

	enter_alt1();
	hal_send_note_on2(USBSTANDALONE, 2, 64, 0x1234);
	hal_send_note_on2(USBSTANDALONE, 2, 64, 0x2345);	/* retrigger: no extra reserve */
	CHECK_EQ(ump_tx_reserve_words(), 2);
	hal_send_note_off2(USBSTANDALONE, 2, 64, 0x5678);
	CHECK_EQ(ump_tx_reserve_words(), 0);
	hal_send_note_off2(USBSTANDALONE, 2, 64, 0);		/* unmatched: still sent */
	CHECK_EQ(ump_tx_reserve_words(), 0);
	CHECK_EQ(host_in(w), 8);
	CHECK_EQ(w[0], 0x40924000u);
	CHECK_EQ(w[1], 0x12340000u);
	CHECK_EQ(w[2], 0x40924000u);
	CHECK_EQ(w[3], 0x23450000u);
	CHECK_EQ(w[4], 0x40824000u);
	CHECK_EQ(w[5], 0x56780000u);
	CHECK_EQ(w[6], 0x40824000u);
	CHECK_EQ(w[7], 0);
}

static void test_saturated_note_offs_always_queue(void)
{
	static uint32_t got[UMP_TX_NOTE_WORDS + 16];
	uint32_t w[16];
	uint16_t n = 0, k;
	int accepted, offs = 0;
	uint8_t note;

	enter_alt1();						/* host not reading */
	for (note = 0; note < 128; note++)
		hal_send_note_on2(USBSTANDALONE, 0, note, 0x8000);
	accepted = ump_tx_reserve_words() / 2;
	CHECK(accepted > 0);
	CHECK(accepted < 128);
	for (note = 0; note < 128; note++)
		hal_send_note_off2(USBSTANDALONE, 0, note, 0);
	CHECK_EQ(ump_tx_reserve_words(), 0);
	while ((k = host_in(w)) > 0) {
		memcpy(got + n, w, 4u * k);
		n = (uint16_t)(n + k);
	}
	for (k = 0; k < n; k += 2)
		if ((got[k] & 0xFFF00000u) == 0x40800000u && ((got[k] >> 8) & 0x7F) < accepted)
			offs++;
	CHECK_EQ(offs, accepted);				/* every accepted note is released */
}

static void test_hal_send_midi_alt1(void)
{
	uint32_t w[16];
	enter_alt1();
	__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 100);	/* MT 4 attack */
	__wrap_hal_send_midi(USBSTANDALONE, 0xB0, 7, 64);	/* MT 4 CC, note ring */
	__wrap_hal_send_midi(USBSTANDALONE, 0xF8, 0, 0);	/* system: control ring */
	CHECK_EQ(ump_tx_reserve_words(), 2);
	CHECK_EQ(host_in(w), 5);
	CHECK_EQ(w[0], 0x10F80000u);
	CHECK_EQ(w[1], 0x40903C00u);
	CHECK_EQ(w[2], ump_scale_up(100, 7, 16) << 16);
	CHECK_EQ(w[3], 0x40B00700u);
	CHECK_EQ(w[4], 0x80000000u);

	__wrap_hal_send_midi(USBMIDI, 0x91, 60, 100);		/* group 1 is MIDI 1.0 */
	__wrap_hal_send_midi(USBMIDI, 0x91, 60, 0);		/* note on vel 0 = release */
	CHECK_EQ(ump_tx_reserve_words(), 2);
	__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 0);	/* MT 4 Note Off */
	CHECK_EQ(ump_tx_reserve_words(), 0);
	CHECK_EQ(host_in(w), 4);
	CHECK_EQ(w[0], 0x21913C64u);
	CHECK_EQ(w[1], 0x21913C00u);
	CHECK_EQ(w[2], 0x40803C00u);
	CHECK_EQ(w[3], 0x80000000u);

	__wrap_hal_send_midi(USBSTANDALONE, 0xF0, 0, 0);	/* SysEx byte: dropped */
	__wrap_hal_send_midi(5, 0x90, 1, 1);			/* no such port */
	CHECK_EQ(host_in(w), 0);
	__wrap_hal_send_midi(DINMIDI, 0x90, 1, 2);
	CHECK_EQ(stub.real_midi_calls, 2);			/* port 5 and DIN via real */
	CHECK_EQ(stub.real_midi_last[0], DINMIDI);

	/* MIDI 1.0 protocol: Standalone goes as MT 2 too */
	{
		static const uint32_t req[4] = {0xF0050100u, 0, 0, 0};
		host_out(req, 4);
		__wrap_midi_parsers_poll();
		CHECK_EQ(host_in(w), 4);			/* notification */
		__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 100);
		CHECK_EQ(host_in(w), 1);
		CHECK_EQ(w[0], 0x20903C64u);
		CHECK_EQ(ump_tx_reserve_words(), 2);
	}
}

static void test_pressure2(void)
{
	uint32_t w[16];
	enter_alt0();
	hal_send_poly_pressure2(USBMIDI, 1, 60, 0xFFFFFFFFu);
	CHECK_EQ(stub.real_midi_last[1], 0xA1);
	CHECK_EQ(stub.real_midi_last[3], 127);
	hal_send_channel_pressure2(DINMIDI, 1, 0x80000000u);
	CHECK_EQ(stub.real_midi_last[0], DINMIDI);
	CHECK_EQ(stub.real_midi_last[1], 0xD1);
	CHECK_EQ(stub.real_midi_last[2], 64);

	enter_alt1();
	hal_send_poly_pressure2(USBSTANDALONE, 1, 60, 0x11111111u);
	hal_send_poly_pressure2(USBSTANDALONE, 1, 60, 0x22222222u);	/* coalesced */
	hal_send_channel_pressure2(USBSTANDALONE, 1, 0x33333333u);
	hal_send_poly_pressure2(USBMIDI, 1, 60, 0x80000000u);		/* MT 2 */
	CHECK_EQ(host_in(w), 5);
	CHECK_EQ(w[0], 0x21A13C40u);					/* note ring first */
	CHECK_EQ(w[1], 0x40A13C00u);
	CHECK_EQ(w[2], 0x22222222u);
	CHECK_EQ(w[3], 0x40D10000u);
	CHECK_EQ(w[4], 0x33333333u);
}

static void test_protocol_change_releases_notes(void)
{
	static const uint32_t to_midi1[4] = {0xF0050100u, 0, 0, 0};
	static const uint32_t to_midi2[4] = {0xF0050200u, 0, 0, 0};
	uint32_t w[16];

	/* alt 1 MIDI 2: note 60 sent; note 62 on + off still queued; Stream
	   Config request to MIDI 1 via RX → on the wire, in order: the queued
	   Note On 62, its Note Off, the hook's MIDI 2.0 Note Off 0x40803C00 for
	   the still-sounding 60, then 0xF0060100; nothing dropped */
	enter_alt1();
	CHECK_EQ(midi2_active(), 1);
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
	CHECK_EQ(host_in(w), 2);
	hal_send_note_on2(USBSTANDALONE, 0, 62, 0x9000);
	hal_send_note_off2(USBSTANDALONE, 0, 62, 0);
	host_out(to_midi1, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI1);
	CHECK_EQ(midi2_active(), 0);
	CHECK_EQ(ump_tx_reserve_words(), 0);
	CHECK_EQ(host_in(w), 10);
	CHECK_EQ(w[0], 0x40903E00u);
	CHECK_EQ(w[1], 0x90000000u);
	CHECK_EQ(w[2], 0x40803E00u);
	CHECK_EQ(w[3], 0);
	CHECK_EQ(w[4], 0x40803C00u);
	CHECK_EQ(w[5], 0);
	CHECK_EQ(w[6], 0xF0060100u);
	CHECK_EQ(w[7], 0);
	CHECK_EQ(host_in(w), 0);
	/* the sounding bit was cleared: a later note off is sent once, as MT 2 */
	hal_send_note_off2(USBSTANDALONE, 0, 60, 0);
	CHECK_EQ(host_in(w), 1);
	CHECK_EQ(w[0], 0x20803C00u);

	/* back to MIDI 2: a MIDI 1.0 note is released as MIDI 1.0 */
	hal_send_note_on2(USBSTANDALONE, 5, 70, 0xFFFF);
	CHECK_EQ(host_in(w), 1);
	CHECK_EQ(w[0], 0x2095467Fu);
	host_out(to_midi2, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(midi2_active(), 1);
	CHECK_EQ(host_in(w), 5);
	CHECK_EQ(w[0], 0x20854600u);
	CHECK_EQ(w[1], 0xF0060200u);

	/* a request for the current protocol releases nothing */
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
	CHECK_EQ(host_in(w), 2);
	host_out(to_midi2, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(host_in(w), 4);
	CHECK_EQ(w[0], 0xF0060200u);
	CHECK_EQ(ump_tx_reserve_words(), 2);
}

static void test_protocol_change_keeps_group1(void)
{
	static const uint32_t req[4] = {0xF0050100u, 0, 0, 0};
	uint32_t w[16];
	enter_alt1();
	hal_send_note_on2(USBMIDI, 0, 61, 0x8000);
	hal_send_note_on2(USBMIDI, 0, 63, 0x8000);
	CHECK_EQ(host_in(w), 2);
	hal_send_note_off2(USBMIDI, 0, 61, 0);			/* queued at the change */
	hal_send_poly_pressure2(USBSTANDALONE, 0, 60, 5);	/* stale group 0 pressure */
	CHECK_EQ(ump_tx_reserve_words(), 2);
	host_out(req, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(host_in(w), 5);
	CHECK_EQ(w[0], 0x21803D00u);				/* group 1 Note Off survives */
	CHECK_EQ(w[1], 0xF0060100u);				/* no Note Off for group 1 note 63 */
	CHECK_EQ(host_in(w), 0);				/* group 0 pressure dropped */
	CHECK_EQ(ump_tx_reserve_words(), 2);			/* note 63 still holds its reserve */
	hal_send_note_off2(USBMIDI, 0, 63, 0);
	CHECK_EQ(ump_tx_reserve_words(), 0);
	CHECK_EQ(host_in(w), 1);
	CHECK_EQ(w[0], 0x21803F00u);
}

static void test_protocol_change_notify_survives_full_control_ring(void)
{
	static const uint32_t req[4] = {0xF0050100u, 0, 0, 0};
	static const uint32_t ctl[4] = {0xF0010101u, 0x82000300u, 0, 0};
	static uint32_t got[256];
	uint32_t w[16];
	uint16_t n = 0, k;
	int pushed = 0;
	enter_alt1();
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
	CHECK_EQ(host_in(w), 2);
	while (ump_tx_push(UMP_TX_CONTROL, ctl, 4) == 0)
		pushed++;
	CHECK_EQ(pushed, UMP_TX_CONTROL_WORDS / 4);
	host_out(req, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(ump_stream_protocol(), UMP_PROTO_MIDI1);
	while ((k = host_in(w)) > 0 && n + k <= 256) {
		memcpy(got + n, w, 4u * k);
		n = (uint16_t)(n + k);
	}
	CHECK_EQ(n, UMP_TX_CONTROL_WORDS + 6);
	CHECK_EQ(got[n - 6], 0x40803C00u);
	CHECK_EQ(got[n - 4], 0xF0060100u);
}

static void test_protocol_change_keeps_queued_replies(void)
{
	static const uint32_t req[8] = {0xF0000101u, 0x00000001u, 0, 0,	/* EP discovery: info */
	                                0xF0050100u, 0, 0, 0};		/* then config → MIDI 1 */
	uint32_t w[16];
	enter_alt1();
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
	host_out(req, 8);
	__wrap_midi_parsers_poll();
	CHECK_EQ(host_in(w), 12);
	CHECK_EQ(w[0], 0xF0010101u);				/* Endpoint Info kept */
	CHECK_EQ(w[4], 0x40903C00u);				/* queued Note On kept */
	CHECK_EQ(w[6], 0x40803C00u);
	CHECK_EQ(w[8], 0xF0060100u);
}

static void test_parsers_init_registers_hook(void)
{
	static const uint32_t req[4] = {0xF0050100u, 0, 0, 0};
	uint32_t w[16];
	enter_alt1();
	ump_stream_set_protocol_hook(0);
	__wrap_midi_parsers_init();
	hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
	CHECK_EQ(host_in(w), 2);
	host_out(req, 4);
	__wrap_midi_parsers_poll();
	CHECK_EQ(host_in(w), 6);
	CHECK_EQ(w[0], 0x40803C00u);
}

static void test_hal_send_sysex_alt1(void)
{
	/* 20-byte F0..F7 → 4 SysEx7 packets on group 0 sent in order through EP1 IN */
	uint8_t sx[22];
	uint32_t ref[8], w[16];
	int i;
	sx[0] = 0xF0;
	for (i = 1; i <= 20; i++)
		sx[i] = (uint8_t)i;
	sx[21] = 0xF7;
	CHECK_EQ(ump_sysex7_pack(ref, 8, 0, sx, 22), 8);

	enter_alt1();
	__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 100);	/* note ring: after SysEx */
	__wrap_hal_send_sysex(USBSTANDALONE, sx, 22);
	CHECK_EQ(host_in(w), 10);
	CHECK_MEM(w, ref, sizeof ref);
	CHECK_EQ(w[0] >> 20, 0x301);
	CHECK_EQ(w[2] >> 20, 0x302);
	CHECK_EQ(w[6] >> 20, 0x303);
	CHECK_EQ(w[8], 0x40903C00u);
	CHECK_EQ(stub.real_sysex_calls, 0);

	__wrap_hal_send_sysex(USBMIDI, sx, 22);			/* group 1 */
	CHECK_EQ(host_in(w), 8);
	CHECK_EQ(w[0] >> 24, 0x31);
	__wrap_hal_send_sysex(USBMIDI, sx, 21);			/* no F7: dropped */
	CHECK_EQ(host_in(w), 0);
	__wrap_hal_send_sysex(DINMIDI, sx, 22);
	CHECK_EQ(stub.real_sysex_calls, 1);
	CHECK(stub.real_sysex_data == sx);

	enter_alt0();
	__wrap_hal_send_sysex(USBSTANDALONE, sx, 22);
	CHECK_EQ(stub.real_sysex_calls, 1);
	CHECK_EQ(stub.real_sysex_port, USBSTANDALONE);
}

/* A host that isn't reading EP1 IN yet: 20 ms clock fills the control ring,
 * but a full Endpoint Discovery request still gets all of its replies. */
static void test_stream_replies_survive_clock_flood(void)
{
	static const uint32_t disc[4] = {0xF0000101u, 0x0000001Fu, 0, 0};
	static const uint32_t fbdisc[4] = {0xF010FF03u, 0, 0, 0};
	static const uint8_t sx[3] = {0xF0, 0x01, 0xF7};
	static uint32_t got[256];
	uint32_t w[16];
	uint16_t n = 0, k, clocks = 0, replies = 0;
	int i;

	enter_alt1();
	for (i = 0; i < 300; i++)				/* until well past refused */
		__wrap_hal_send_midi(USBSTANDALONE, 0xF8, 0, 0);
	__wrap_hal_send_sysex(USBSTANDALONE, sx, 3);		/* app SysEx: no room */
	host_out(disc, 4);
	__wrap_midi_parsers_poll();
	while ((k = host_in(w)) > 0 && n + k <= 256) {
		memcpy(got + n, w, 4u * k);
		n = (uint16_t)(n + k);
	}
	while (clocks < n && got[clocks] == 0x10F80000u)
		clocks++;
	CHECK_EQ(clocks, UMP_TX_CONTROL_WORDS - UMP_TX_CONTROL_STREAM_HEADROOM);
	for (k = clocks; k + 4 <= n; k += 4)
		if ((got[k] >> 28) == 0xF)
			replies++;
	CHECK_EQ(replies, 6);					/* info, identity, name x2, id, config */
	CHECK_EQ(n, clocks + 24);
	CHECK_EQ(got[clocks], 0xF0010101u);
	CHECK_EQ(got[n - 4], 0xF0060200u);

	/* the same with Function Block discovery (all FBs, info and name) */
	enter_alt1();
	for (i = 0; i < 300; i++)
		__wrap_hal_send_midi(USBMIDI, 0xF8, 0, 0);
	host_out(fbdisc, 4);
	__wrap_midi_parsers_poll();
	n = 0;
	while ((k = host_in(w)) > 0 && n + k <= 256) {
		memcpy(got + n, w, 4u * k);
		n = (uint16_t)(n + k);
	}
	CHECK_EQ(n, UMP_TX_CONTROL_WORDS - UMP_TX_CONTROL_STREAM_HEADROOM + 20);
	CHECK_EQ(got[n - 20], 0xF0118033u);
}

static void test_hal_send_does_not_run_service(void)
{
	static const uint8_t sx[3] = {0xF0, 0x01, 0xF7};
	enter_alt1();
	usb_midi2_set_alt(1);					/* ISR: service pending */
	__wrap_hal_send_midi(USBSTANDALONE, 0x90, 60, 100);
	__wrap_hal_send_sysex(USBSTANDALONE, sx, 3);
	hal_send_note_on2(USBSTANDALONE, 0, 61, 0x8000);
	CHECK_EQ(stub.fifo_flush_calls, 0);
	CHECK_EQ(stub.ep_rx_valid_calls, 0);
	__wrap_usbmidi_poll();
	CHECK_EQ(stub.fifo_flush_calls, 1);
}

static void test_alt_and_active(void)
{
	enter_alt0();
	CHECK_EQ(midi2_usb_alt(), 0);
	CHECK_EQ(midi2_active(), 0);
	enter_alt1();
	CHECK_EQ(midi2_usb_alt(), 1);
	CHECK_EQ(midi2_active(), 1);
	usb_midi2_usb_reset();
	CHECK_EQ(midi2_usb_alt(), 0);
	CHECK_EQ(midi2_active(), 0);
}

/* An alt switch (USB interrupt) at any irq_save of a note on or off must leave
 * the release reserve matching the sounding table: no leak, no lost release. */
static int irq_count, irq_fire_at;

static void irq_set_alt1(void)
{
	if (++irq_count == irq_fire_at) {
		stub_irq_hook = 0;
		usb_midi2_set_alt(1);
	}
}

static void test_sounding_vs_set_alt_interrupt(void)
{
	int at;
	for (at = 1; at <= 6; at++) {
		uint16_t r;
		enter_alt1();
		hal_send_note_on2(USBSTANDALONE, 0, 60, 0x8000);
		irq_count = 0;
		irq_fire_at = at;
		stub_irq_hook = irq_set_alt1;
		hal_send_note_on2(USBSTANDALONE, 0, 62, 0x8000);
		hal_send_note_off2(USBSTANDALONE, 0, 60, 0);
		stub_irq_hook = 0;
		r = ump_tx_reserve_words();
		CHECK(r == 0 || r == 2);
		hal_send_note_off2(USBSTANDALONE, 0, 62, 0);
		hal_send_note_off2(USBSTANDALONE, 0, 60, 0);
		CHECK_EQ(ump_tx_reserve_words(), 0);
	}
}

void test_usb_data_all(void)
{
	RUN(test_ci_discovery_endpoint_and_version);
	RUN(test_ci_nak_address_and_sysex_limit);
	RUN(test_ci_unsupported_profile_request_nak_details);
	RUN(test_ci_muid_replacement_and_collision);
	RUN(test_ci_reply_retries_and_is_discarded_on_alt_change);
	RUN(test_ci_pe_capabilities_and_get);
	RUN(test_ci_pe_small_initiator_and_terminate);
	RUN(test_ci_pe_set);
	RUN(test_ci_pe_subscribe);
	RUN(test_ci_pe_one_chunk_per_pass_and_backpressure);
	RUN(test_ci_pe_multi_muid_max_sysex_cache);
	RUN(test_ci_non_discovery_self_echo_ignored);
	RUN(test_ci_pe_surface_settings);
	RUN(test_ep1_out_odd_length);
	RUN(test_ep1_out_leaves_nak_until_poll);
	RUN(test_backpressure_nak_until_space);
	RUN(test_rearm_threshold);
	RUN(test_rearm_waits_for_din_out);
	RUN(test_din_parsed_in_both_alts);
	RUN(test_legacy_paths_gated_in_alt1);
	RUN(test_alt0_usbmidi_poll_atomic);
	RUN(test_alt0_hal_send_masked);
	RUN(test_alt1_entry_runs_service_first);
	RUN(test_usbmidi_poll_tx_conditions);
	RUN(test_note_on2_paths);
	RUN(test_note_on2_pitch_paths);
	RUN(test_note_off2_and_reserve);
	RUN(test_saturated_note_offs_always_queue);
	RUN(test_hal_send_midi_alt1);
	RUN(test_pressure2);
	RUN(test_protocol_change_releases_notes);
	RUN(test_protocol_change_keeps_queued_replies);
	RUN(test_protocol_change_keeps_group1);
	RUN(test_protocol_change_notify_survives_full_control_ring);
	RUN(test_parsers_init_registers_hook);
	RUN(test_hal_send_sysex_alt1);
	RUN(test_stream_replies_survive_clock_flood);
	RUN(test_hal_send_does_not_run_service);
	RUN(test_alt_and_active);
	RUN(test_sounding_vs_set_alt_interrupt);
	RUN(test_main_loop_hook_runs_in_both_alts);
	enter_alt0();
	ump_stream_set_protocol_hook(0);
}
