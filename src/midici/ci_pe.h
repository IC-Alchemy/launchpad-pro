#ifndef CI_PE_H
#define CI_PE_H

#include <stdint.h>
#include "midici/ci_json.h"
#include "instrument/settings.h"

/*
 * MIDI-CI Property Exchange resources (Common Rules for PE 1.1): what each
 * resource says, and how a Set changes the settings. No transport here; the
 * session in pe_session.cpp frames, chunks and routes the messages.
 *
 * Bodies are rendered through a ci_json_out window, so a chunk is filled
 * straight into the reply buffer and no body is ever held in RAM.
 */

enum ci_pe_resource {
	CI_PE_RESOURCE_LIST,
	CI_PE_DEVICE_INFO,
	CI_PE_CHANNEL_LIST,
	CI_PE_SCALE_SETTINGS,	/* "X-ScaleSettings": Get / Set / Subscribe */
	CI_PE_PLAY_OPTIONS,	/* "X-PlayOptions": Get / Set / Subscribe */
	CI_PE_SCALE_LIST,	/* "X-ScaleList": the scale names, read-only */
	CI_PE_RESOURCE_COUNT,
	CI_PE_NONE = 0xFF
};

/* Bit per resource, for what a settings change touches. */
#define CI_PE_BIT(r) ((uint8_t)(1u << (r)))

/* Largest Set body accepted; a longer one gets status 413. */
#define CI_PE_SET_BODY_MAX 256

/* PE status codes used in reply headers */
#define CI_PE_STATUS_OK 200
#define CI_PE_STATUS_BAD_REQUEST 400
#define CI_PE_STATUS_NOT_FOUND 404
#define CI_PE_STATUS_NOT_ALLOWED 405
#define CI_PE_STATUS_TOO_LARGE 413

/* CI_PE_NONE if the name is not a resource we have. */
uint8_t ci_pe_lookup(const char *name, uint16_t len);
const char *ci_pe_name(uint8_t res);
uint8_t ci_pe_can_set(uint8_t res);
uint8_t ci_pe_can_subscribe(uint8_t res);

/* Renders a resource's body. s is the settings to describe (unused by the
 * fixed resources); piid is the Product Instance ID (DeviceInfo serialNumber). */
void ci_pe_render(uint8_t res, const struct settings *s, const char *piid, struct ci_json_out *o);
/* Body length in bytes (a render with an empty window). */
uint16_t ci_pe_body_len(uint8_t res, const struct settings *s, const char *piid);

/* Full-object Set: the fields given replace their values in *s, missing ones
 * keep theirs, unknown fields are ignored. On any error (malformed JSON, a
 * wrong type, an out-of-range value) *s is left untouched and the result is
 * CI_PE_STATUS_BAD_REQUEST; otherwise CI_PE_STATUS_OK. A resource that cannot
 * be Set gets CI_PE_STATUS_NOT_ALLOWED. */
uint16_t ci_pe_apply_set(uint8_t res, const char *body, uint16_t len, struct settings *s);

/* Resources whose body differs between a and b (CI_PE_BIT mask). */
uint8_t ci_pe_changed(const struct settings *a, const struct settings *b);

#endif
