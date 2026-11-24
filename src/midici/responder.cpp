#include "midici/responder.h"
#include "midici/ci_identity.h"
#include "vendor/MidiCiResponder.h"

extern "C" {
#include "app.h"
#include "midici/midici.h"
#include "platform/platform.h"
#include "ump/ump.h"
}

namespace ms = WindowsMidiServicesCapabilityInquiry;

namespace {

constexpr uint8_t kStandaloneGroup = 0;

ms::Responder responder;
bool initialized;
bool notify_pending;	/* Function Block 0 info must be re-sent: the MUID changed */

uint32_t replace_muid(uint32_t previous)
{
	uint32_t m = midici_muid_regenerate();
	for (uint8_t i = 0; i < 4 && m == previous; i++)
		m = midici_muid_regenerate();
	return m;
}

void init_responder()
{
	ms::ResponderConfig config{};
	config.Muid = midici_muid();
	/* Identity matches the UMP Device Identity and Product Instance ID replies. */
	config.ManufacturerSysExId[0] = CI_MANUFACTURER_0;
	config.ManufacturerSysExId[1] = CI_MANUFACTURER_1;
	config.ManufacturerSysExId[2] = CI_MANUFACTURER_2;
	config.DeviceFamily = CI_FAMILY;
	config.DeviceFamilyModelNumber = CI_MODEL;
	config.SoftwareRevisionLevel[0] = CI_SW_REV_0;
	config.SoftwareRevisionLevel[1] = CI_SW_REV_1;
	config.SoftwareRevisionLevel[2] = CI_SW_REV_2;
	config.SoftwareRevisionLevel[3] = CI_SW_REV_3;
	config.ReceivableMaximumSysExSize = CI_REASSEMBLY_BYTES;
	config.FunctionBlockNumber = 0;
	char id[25];
	platform_product_instance_id(id);
	for (const char *p = id; *p; ++p)
		config.ProductInstanceId[config.ProductInstanceIdByteCount++] = static_cast<uint8_t>(*p);
	responder.Initialize(config);
	initialized = true;
}

size_t build_nak(const ms::ParsedMessage &message, uint8_t status, uint8_t *out, size_t capacity)
{
	ms::AcknowledgmentFields fields{};
	fields.OriginalMessageType = static_cast<uint8_t>(message.Type);
	fields.StatusCode = status;
	return ms::BuildAcknowledgment(ms::MessageType::Nak, message.DeviceId, responder.Muid(),
		message.SourceMuid, fields, nullptr, 0, out, capacity);
}

/* payload: the MIDI-CI message from the 0x7E, without F0/F7. Returns the reply
 * length written to out (0 for none); sets muid_changed when our MUID was replaced. */
size_t respond(const uint8_t *payload, size_t len, uint8_t *out, size_t capacity, bool &muid_changed)
{
	using T = ms::MessageType;
	ms::ParsedMessage message{};
	if (len < ms::CommonHeaderByteCount)
		return 0;
	const auto parsed = ms::Parse(payload, len, message);
	if (!ms::MuidIsUsable(message.SourceMuid) ||
	    (message.DeviceId > 0x0F && message.DeviceId < 0x7E))
		return 0;
	if (!initialized)
		init_responder();

	const bool broadcast = message.DestinationMuid == ms::MuidBroadcast;
	const bool management = message.Type == T::Discovery || message.Type == T::InvalidateMuid;
	if (management ? !broadcast || message.DeviceId != ms::DeviceIdFunctionBlock
	               : message.DestinationMuid != responder.Muid())
		return 0;
	if (message.Type == T::EndpointInquiry && message.DeviceId != ms::DeviceIdFunctionBlock)
		return 0;

	const uint8_t version = message.VersionFormat;
	const bool inquiry = message.Type == T::Discovery || ms::MessageTypeExpectsAResponderAnswer(message.Type);
	if (!version || (version & 0x70)) {
		constexpr uint8_t nak_version_unsupported = 0x02;
		return inquiry ? build_nak(message, nak_version_unsupported, out, capacity) : 0;
	}
	if (parsed != ms::ParseStatus::Ok ||
	    (message.Type == T::Discovery && len < (version == ms::MessageVersion11
	        ? ms::DiscoveryByteCountVersion11 : ms::DiscoveryByteCount)) ||
	    (message.Type == T::EndpointInquiry && !message.HasEndpointFields))
		return inquiry ? build_nak(message, ms::NakStatusMessageMalformed, out, capacity) : 0;

	if (version == ms::MessageVersion11)
		message.OutputPathId = 0;
	/* M2-101 requires one outgoing format per MUID. The upstream responder
	 * mirrors the incoming version, so give it ours after parsing the wire form. */
	message.VersionFormat = MIDI_CI_VERSION;
	const uint32_t previous = responder.Muid();
	size_t bytes = 0;
	const auto action = responder.ProcessMessage(message, out, capacity, &bytes);
	if (action == ms::ResponderAction::MuidInvalidated || action == ms::ResponderAction::MuidCollision) {
		responder.SetMuid(replace_muid(previous));
		muid_changed = true;
		if (action == ms::ResponderAction::MuidCollision) {
			/* The target is the colliding ID. Use our replacement as source
			 * so peers such as MIDI2Sound don't discard it as their own echo. */
			bytes = ms::BuildInvalidateMuid(responder.Muid(), previous, out, capacity);
		}
	}
	return bytes;
}

void on_message(uint8_t group, const uint8_t *payload, uint16_t len)
{
	if (group != kStandaloneGroup) {
		/* MIDI port: not ours to answer; the app (or a DIN bridge) sees it as plain SysEx. */
		uint16_t cap = 0;
		uint8_t *fwd = midici_reply_buffer(&cap);
		if (len + 2u <= cap) {
			fwd[0] = 0xF0;
			for (uint16_t i = 0; i < len; i++)
				fwd[i + 1] = payload[i];
			fwd[len + 1] = 0xF7;
			app_sysex_event(group, fwd, static_cast<uint16_t>(len + 2));
		}
		return;
	}
	if (len < 4 || payload[0] != ms::UniversalSystemExclusiveId ||
	    payload[2] != ms::SubId1CapabilityInquiry)
		return;

	uint16_t capacity = 0;
	uint8_t *out = midici_reply_buffer(&capacity);
	bool muid_changed = false;
	const size_t bytes = respond(payload, len, out, capacity, muid_changed);
	/* A full CiTxQueue drops the reply; the initiator's Discovery retry recovers. */
	if (bytes)
		midici_send_sysex(group, out, static_cast<uint16_t>(bytes));
	if (muid_changed)
		notify_pending = true;	/* after the reply, so a collision's Invalidate goes first */
	midici_responder_poll();
}

} // namespace

extern "C" void midici_responder_init(void)
{
	midici_set_handler(on_message);
}

extern "C" void midici_responder_poll(void)
{
	if (notify_pending && ump_stream_notify_function_block(0) == 0)
		notify_pending = false;
}
