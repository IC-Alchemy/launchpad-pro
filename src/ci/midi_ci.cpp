#include "ci/midi_ci.h"
#include "platform/platform.h"
#include "ump/ump.h"

/* The upstream headers use Microsoft's static-analysis annotations. */
#define _In_
#define _In_reads_(n)
#define _In_reads_opt_(n)
#define _Out_
#define _Out_writes_(n)
#define _Out_writes_bytes_to_opt_(n, count)
#define _Out_writes_to_opt_(n, count)
#include "third_party/microsoft/MidiCiResponder.h"

namespace ms = WindowsMidiServicesCapabilityInquiry;

static ms::Responder responder;
static uint32_t rng;

static uint32_t next_muid(uint32_t previous)
{
	if (!rng) {
		rng = platform_muid_entropy();
		if (!rng)
			rng = 1;
	}
	uint32_t muid;
	do {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		muid = rng & ms::MuidMaxValue;
	} while (!muid || muid == previous || !ms::MuidIsUsable(muid));
	return muid;
}

static void init_responder()
{
	ms::ResponderConfig config{};
	config.Muid = next_muid(0);
	/* Identity matches the UMP Device Identity and Product Instance ID replies. */
	config.ManufacturerSysExId[1] = 0x20;
	config.ManufacturerSysExId[2] = 0x29;
	config.DeviceFamily = 0x0051;
	config.SoftwareRevisionLevel[1] = 0x01;
	config.ReceivableMaximumSysExSize = UMP_SYSEX7_BUFFER_BYTES - 2;
	config.FunctionBlockNumber = 0;
	char id[25];
	platform_product_instance_id(id);
	for (const char *p = id; *p; ++p)
		config.ProductInstanceId[config.ProductInstanceIdByteCount++] = static_cast<uint8_t>(*p);
	responder.Initialize(config);
}

static size_t build_nak(const ms::ParsedMessage &message, uint8_t status,
			uint8_t *out, size_t capacity)
{
	ms::AcknowledgmentFields fields{};
	fields.OriginalMessageType = static_cast<uint8_t>(message.Type);
	fields.StatusCode = status;
	return ms::BuildAcknowledgment(ms::MessageType::Nak, message.DeviceId, responder.Muid(),
		message.SourceMuid, fields, nullptr, 0, out, capacity);
}

static size_t respond(const uint8_t *payload, size_t len, midi_ci_reply &reply)
{
	using T = ms::MessageType;
	ms::ParsedMessage message{};
	if (len < ms::CommonHeaderByteCount)
		return 0;
	const auto parsed = ms::Parse(payload, len, message);
	if (!ms::MuidIsUsable(message.SourceMuid) ||
	    (message.DeviceId > 0x0F && message.DeviceId < 0x7E))
		return 0;
	if (!responder.Muid())
		init_responder();

	const bool broadcast = message.DestinationMuid == ms::MuidBroadcast;
	const bool management = message.Type == T::Discovery || message.Type == T::InvalidateMuid;
	if (management ? !broadcast || message.DeviceId != ms::DeviceIdFunctionBlock
	               : message.DestinationMuid != responder.Muid())
		return 0;
	if (message.Type == T::EndpointInquiry && message.DeviceId != ms::DeviceIdFunctionBlock)
		return 0;

	uint8_t *out = reply.data + 1;
	const size_t capacity = sizeof(reply.data) - 2;
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
		responder.SetMuid(next_muid(previous));
		reply.muid_changed = 1;
		if (action == ms::ResponderAction::MuidCollision) {
			/* The target is the colliding ID. Use our replacement as source
			 * so peers such as MIDI2Sound don't discard it as their own echo. */
			bytes = ms::BuildInvalidateMuid(responder.Muid(), previous, out, capacity);
		}
	}
	return bytes;
}

int midi_ci_receive(const uint8_t *data, uint16_t len, midi_ci_reply *reply)
{
	if (len < 5 || data[0] != 0xF0 || data[1] != ms::UniversalSystemExclusiveId ||
	    data[3] != ms::SubId1CapabilityInquiry || data[len - 1] != 0xF7)
		return 0;

	reply->muid_changed = 0;
	const size_t bytes = respond(data + 1, len - 2, *reply);
	reply->len = 0;
	if (bytes) {
		reply->data[0] = 0xF0;
		reply->data[bytes + 1] = 0xF7;
		reply->len = static_cast<uint16_t>(bytes + 2);
	}
	return 1;
}
