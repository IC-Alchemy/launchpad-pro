/*
 * MIDI-CI Property Exchange resources. See ci_pe.h and section 8 of
 * docs/superpowers/specs/2026-11-21-midi2-scale-instrument-design.md.
 */
#include "midici/ci_pe.h"
#include "midici/ci_identity.h"
#include "scales/scales.h"
#include "scales/tuning.h"
#include "instrument/palette.h"

/* The schemas below hard-code the ranges: keep them in step. */
typedef char ci_pe_scale_count_is_61[(SCALE_COUNT == 61) ? 1 : -1] __attribute__((unused));
typedef char ci_pe_thresh_count_is_4[(THRESH_COUNT == 4) ? 1 : -1] __attribute__((unused));
typedef char ci_pe_a4_range[(A4_TENTHS_MIN == 4150 && A4_TENTHS_MAX == 4660) ? 1 : -1] __attribute__((unused));
typedef char ci_pe_layout_count_is_5[(LAYOUT_COUNT == 5) ? 1 : -1] __attribute__((unused));
typedef char ci_pe_palette_count_is_8[(PALETTE_COUNT == 8) ? 1 : -1] __attribute__((unused));

static const char *const NAMES[CI_PE_RESOURCE_COUNT] = {
	"ResourceList", "DeviceInfo", "ChannelList", "X-ScaleSettings", "X-PlayOptions", "X-ScaleList",
	"X-TuningList", "X-SurfaceSettings",
};

/* Lives in flash. ResourceList does not list itself (Common Rules for PE).
 * The schemas let a generic editor (MIDI 2.0 Workbench) show real controls. */
static const char RESOURCE_LIST[] =
	"[{\"resource\":\"DeviceInfo\"},"
	"{\"resource\":\"ChannelList\"},"
	"{\"resource\":\"X-ScaleSettings\",\"canSet\":\"full\",\"canSubscribe\":true,"
	"\"schema\":{\"title\":\"Scale\",\"type\":\"object\",\"properties\":{"
	"\"root\":{\"title\":\"Root (0 = C)\",\"type\":\"integer\",\"minimum\":0,\"maximum\":11},"
	"\"scale\":{\"title\":\"Scale (X-ScaleList index; one the tuning offers)\",\"type\":\"integer\",\"minimum\":0,\"maximum\":60},"
	"\"octave\":{\"title\":\"Octave\",\"type\":\"integer\",\"minimum\":0,\"maximum\":7},"
	"\"layout\":{\"title\":\"Layout\",\"type\":\"string\",\"enum\":[\"inKey4ths\",\"inKey3rds\","
	"\"inKeySequential\",\"chromatic4ths\",\"chromaticGuitar\"]},"
	"\"absoluteRoot\":{\"title\":\"Absolute root\",\"type\":\"boolean\"},"
	"\"tuning\":{\"title\":\"Tuning (X-TuningList id)\",\"type\":\"integer\",\"enum\":[0,1,2,3,4,5,6,7,8,9,10,11,"
	"32,33,34,35,36,37,64,65,66,67,96,97,98,128,129,130,131]},"
	"\"a4Tenths\":{\"title\":\"A4 (tenths of a Hz)\",\"type\":\"integer\",\"minimum\":4150,\"maximum\":4660}}}},"
	"{\"resource\":\"X-PlayOptions\",\"canSet\":\"full\",\"canSubscribe\":true,"
	"\"schema\":{\"title\":\"Play options\",\"type\":\"object\",\"properties\":{"
	"\"velocityMode\":{\"title\":\"Velocity mode\",\"type\":\"string\","
	"\"enum\":[\"peakWindow\",\"zeroLatency\",\"twoStage\",\"fixed\"]},"
	"\"velocityCurve\":{\"title\":\"Velocity curve\",\"type\":\"string\",\"enum\":[\"soft\",\"linear\",\"hard\"]},"
	"\"pressureMode\":{\"title\":\"Pressure\",\"type\":\"string\",\"enum\":[\"poly\",\"channel\",\"off\"]},"
	"\"pressureThreshold\":{\"title\":\"Pressure threshold\",\"type\":\"string\","
	"\"enum\":[\"low\",\"med\",\"high\",\"instant\"]},"
	"\"pressureSmoothing\":{\"title\":\"Pressure smoothing\",\"type\":\"string\","
	"\"enum\":[\"none\",\"light\",\"medium\",\"heavy\"]},"
	"\"dinMirror\":{\"title\":\"Mirror pads to DIN\",\"type\":\"boolean\"},"
	"\"channel\":{\"title\":\"MIDI channel\",\"type\":\"integer\",\"minimum\":1,\"maximum\":16}}}},"
	"{\"resource\":\"X-ScaleList\",\"schema\":{\"title\":\"Scale names\",\"type\":\"array\","
	"\"items\":{\"type\":\"string\"}}},"
	"{\"resource\":\"X-TuningList\",\"schema\":{\"title\":\"Tunings\",\"type\":\"array\","
	"\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},"
	"\"name\":{\"type\":\"string\"},\"degrees\":{\"type\":\"integer\"},"
	"\"scales\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}}}}},"
	"{\"resource\":\"X-SurfaceSettings\",\"canSet\":\"full\",\"canSubscribe\":true,"
	"\"schema\":{\"title\":\"Surface\",\"type\":\"object\",\"properties\":{"
	"\"palette\":{\"title\":\"Palette (lower half when split)\",\"type\":\"string\",\"enum\":["
	"\"ocean\",\"ember\",\"forest\",\"amethyst\",\"citrus\",\"glacier\",\"rose\",\"contrast\"]},"
	"\"split\":{\"title\":\"Split (rows 5-8 on their own palette and channel)\",\"type\":\"boolean\"},"
	"\"upperPalette\":{\"title\":\"Upper palette\",\"type\":\"string\",\"enum\":["
	"\"ocean\",\"ember\",\"forest\",\"amethyst\",\"citrus\",\"glacier\",\"rose\",\"contrast\"]},"
	"\"upperChannel\":{\"title\":\"Upper MIDI channel\",\"type\":\"integer\",\"minimum\":1,\"maximum\":16}}}}]";

/* Strings for the enum fields, indexed by the settings value. */
static const char *const LAYOUTS[LAYOUT_COUNT] = {
	"inKey4ths", "inKey3rds", "inKeySequential", "chromatic4ths", "chromaticGuitar",
};
static const char *const VEL_MODES[4] = {"peakWindow", "zeroLatency", "fixed", "twoStage"};
static const char *const CURVES[3] = {"soft", "linear", "hard"};
static const char *const PRESSURES[3] = {"poly", "channel", "off"};
static const char *const THRESHOLDS[THRESH_COUNT] = {"low", "med", "high", "instant"};
static const char *const SMOOTHINGS[4] = {"none", "light", "medium", "heavy"};

uint8_t ci_pe_lookup(const char *name, uint16_t len)
{
	uint8_t r;
	uint16_t i;

	for (r = 0; r < CI_PE_RESOURCE_COUNT; r++) {
		const char *n = NAMES[r];
		for (i = 0; i < len && n[i] == name[i]; i++)
			;
		if (i == len && n[i] == 0)
			return r;
	}
	return CI_PE_NONE;
}

const char *ci_pe_name(uint8_t res)
{
	return res < CI_PE_RESOURCE_COUNT ? NAMES[res] : "";
}

uint8_t ci_pe_can_set(uint8_t res)
{
	return res == CI_PE_SCALE_SETTINGS || res == CI_PE_PLAY_OPTIONS || res == CI_PE_SURFACE_SETTINGS;
}

uint8_t ci_pe_can_subscribe(uint8_t res)
{
	return ci_pe_can_set(res);
}

/* ---- rendering ---- */

static void member_int(struct ci_json_out *o, const char *key, int32_t v, uint8_t comma)
{
	if (comma)
		ci_json_raw(o, ",");
	ci_json_key(o, key);
	ci_json_int(o, v);
}

static void member_str(struct ci_json_out *o, const char *key, const char *v, uint8_t comma)
{
	if (comma)
		ci_json_raw(o, ",");
	ci_json_key(o, key);
	ci_json_str(o, v);
}

static void member_bool(struct ci_json_out *o, const char *key, uint8_t v)
{
	ci_json_raw(o, ",");
	ci_json_key(o, key);
	ci_json_bool(o, v);
}

static void id_array(struct ci_json_out *o, const char *key, const uint8_t *b, uint8_t n)
{
	uint8_t i;
	ci_json_raw(o, ",");
	ci_json_key(o, key);
	ci_json_raw(o, "[");
	for (i = 0; i < n; i++) {
		if (i)
			ci_json_raw(o, ",");
		ci_json_int(o, b[i]);
	}
	ci_json_raw(o, "]");
}

static void device_info(struct ci_json_out *o, const char *piid)
{
	static const uint8_t man[3] = {CI_MANUFACTURER_0, CI_MANUFACTURER_1, CI_MANUFACTURER_2};
	static const uint8_t fam[2] = {CI_FAMILY & 0x7F, (CI_FAMILY >> 7) & 0x7F};
	static const uint8_t mod[2] = {CI_MODEL & 0x7F, (CI_MODEL >> 7) & 0x7F};
	static const uint8_t ver[4] = {CI_SW_REV_0, CI_SW_REV_1, CI_SW_REV_2, CI_SW_REV_3};
	uint8_t i;

	ci_json_raw(o, "{");
	/* the first member has no leading comma: write it by hand */
	ci_json_key(o, "manufacturerId");
	ci_json_raw(o, "[");
	for (i = 0; i < 3; i++) {
		if (i)
			ci_json_raw(o, ",");
		ci_json_int(o, man[i]);
	}
	ci_json_raw(o, "]");
	member_str(o, "manufacturer", CI_MANUFACTURER_NAME, 1);
	id_array(o, "familyId", fam, 2);
	member_str(o, "family", CI_FAMILY_NAME, 1);
	id_array(o, "modelId", mod, 2);
	member_str(o, "model", CI_ENDPOINT_NAME, 1);
	id_array(o, "versionId", ver, 4);
	ci_json_raw(o, ",");
	ci_json_key(o, "version");
	ci_json_raw(o, "\"");
	for (i = 0; i < 4; i++) {
		if (i)
			ci_json_raw(o, ".");
		ci_json_int(o, ver[i]);
	}
	ci_json_raw(o, "\"");
	if (piid && piid[0])
		member_str(o, "serialNumber", piid, 1);
	ci_json_raw(o, "}");
}

static void render_settings(uint8_t res, const struct settings *s, struct ci_json_out *o)
{
	ci_json_raw(o, "{");
	if (res == CI_PE_SCALE_SETTINGS) {
		member_int(o, "root", s->root, 0);
		member_int(o, "scale", s->scale, 1);
		member_int(o, "octave", s->octave, 1);
		member_str(o, "layout", s->layout < LAYOUT_COUNT ? LAYOUTS[s->layout] : "", 1);
		member_bool(o, "absoluteRoot", s->absolute_root);
		member_int(o, "tuning", s->tuning, 1);
		member_int(o, "a4Tenths", s->a4_tenths, 1);
	} else if (res == CI_PE_SURFACE_SETTINGS) {
		member_str(o, "palette", palette_name(s->palette), 0);
		member_bool(o, "split", s->split);
		member_str(o, "upperPalette", palette_name(s->upper_palette), 1);
		member_int(o, "upperChannel", s->upper_channel + 1, 1);
	} else {
		member_str(o, "velocityMode", s->velocity_mode < 4 ? VEL_MODES[s->velocity_mode] : "", 0);
		member_str(o, "velocityCurve", s->velocity_curve < 3 ? CURVES[s->velocity_curve] : "", 1);
		member_str(o, "pressureMode", s->pressure_mode < 3 ? PRESSURES[s->pressure_mode] : "", 1);
		member_str(o, "pressureThreshold",
		           s->pressure_threshold < THRESH_COUNT ? THRESHOLDS[s->pressure_threshold] : "", 1);
		member_str(o, "pressureSmoothing",
		           s->pressure_smoothing < 4 ? SMOOTHINGS[s->pressure_smoothing] : "", 1);
		member_bool(o, "dinMirror", s->din_mirror);
		member_int(o, "channel", s->channel + 1, 1);
	}
	ci_json_raw(o, "}");
}

/* [{"id":0,"name":"12-EDO (Standard)","degrees":12},
 *  {"id":1,"name":"24-EDO Quarter-Tone","degrees":24,"scales":[32,33,34,35,31]},...]
 * No "scales": the tuning offers every twelve-note scale, 0..30. */
static void tuning_list(struct ci_json_out *o)
{
	uint8_t i, j;

	ci_json_raw(o, "[");
	for (i = 0; i < TUNING_COUNT; i++) {
		const struct tuning_def *t = &TUNINGS[i];

		ci_json_raw(o, i ? ",{" : "{");
		member_int(o, "id", t->id, 0);
		member_str(o, "name", t->name, 1);
		member_int(o, "degrees", t->degrees, 1);
		if (t->nscales) {
			ci_json_raw(o, ",");
			ci_json_key(o, "scales");
			ci_json_raw(o, "[");
			for (j = 0; j < t->nscales; j++) {
				if (j)
					ci_json_raw(o, ",");
				ci_json_int(o, t->scales[j]);
			}
			ci_json_raw(o, "]");
		}
		ci_json_raw(o, "}");
	}
	ci_json_raw(o, "]");
}

void ci_pe_render(uint8_t res, const struct settings *s, const char *piid, struct ci_json_out *o)
{
	uint8_t i;

	switch (res) {
	case CI_PE_RESOURCE_LIST:
		ci_json_rawn(o, RESOURCE_LIST, (uint16_t)(sizeof RESOURCE_LIST - 1));
		break;
	case CI_PE_DEVICE_INFO:
		device_info(o, piid);
		break;
	case CI_PE_CHANNEL_LIST:
		/* split: one entry per half, lower first */
		ci_json_raw(o, "[{");
		member_str(o, "title", s->split ? "Standalone Lower" : "Standalone Port", 0);
		member_int(o, "channel", s->channel + 1, 1);
		if (s->split) {
			ci_json_raw(o, "},{");
			member_str(o, "title", "Standalone Upper", 0);
			member_int(o, "channel", s->upper_channel + 1, 1);
		}
		ci_json_raw(o, "}]");
		break;
	case CI_PE_SCALE_SETTINGS:
	case CI_PE_PLAY_OPTIONS:
	case CI_PE_SURFACE_SETTINGS:
		render_settings(res, s, o);
		break;
	case CI_PE_SCALE_LIST:
		ci_json_raw(o, "[");
		for (i = 0; i < SCALE_COUNT; i++) {
			if (i)
				ci_json_raw(o, ",");
			ci_json_str(o, SCALES[i].name);
		}
		ci_json_raw(o, "]");
		break;
	case CI_PE_TUNING_LIST:
		tuning_list(o);
		break;
	default:
		break;
	}
}

uint16_t ci_pe_body_len(uint8_t res, const struct settings *s, const char *piid)
{
	struct ci_json_out o;
	ci_json_out_init(&o, 0, 0, 0);
	ci_pe_render(res, s, piid, &o);
	return o.pos;
}

/* ---- Set ---- */

/* index of the member's string in table[n], or -1 */
static int16_t enum_index(const struct ci_json_member *m, const char *const *table, uint8_t n)
{
	uint8_t i;
	if (m->type != CI_JSON_STR)
		return -1;
	for (i = 0; i < n; i++)
		if (ci_json_str_is(m, table[i]))
			return i;
	return -1;
}

static uint8_t set_int(const struct ci_json_member *m, int32_t lo, int32_t hi, int32_t bias, uint8_t *field)
{
	if (m->type != CI_JSON_INT || m->num < lo || m->num > hi)
		return 0;
	*field = (uint8_t)(m->num + bias);
	return 1;
}

static uint8_t set_u16(const struct ci_json_member *m, int32_t lo, int32_t hi, uint16_t *field)
{
	if (m->type != CI_JSON_INT || m->num < lo || m->num > hi)
		return 0;
	*field = (uint16_t)m->num;
	return 1;
}

static uint8_t set_bool(const struct ci_json_member *m, uint8_t *field)
{
	if (m->type != CI_JSON_BOOL)
		return 0;
	*field = (uint8_t)m->num;
	return 1;
}

static uint8_t set_enum(const struct ci_json_member *m, const char *const *table, uint8_t n, uint8_t *field)
{
	int16_t i = enum_index(m, table, n);
	if (i < 0)
		return 0;
	*field = (uint8_t)i;
	return 1;
}

/* a palette by its palette_name() */
static uint8_t set_palette(const struct ci_json_member *m, uint8_t *field)
{
	uint8_t i;
	if (m->type != CI_JSON_STR)
		return 0;
	for (i = 0; i < PALETTE_COUNT; i++)
		if (ci_json_str_is(m, palette_name(i))) {
			*field = i;
			return 1;
		}
	return 0;
}

uint16_t ci_pe_apply_set(uint8_t res, const char *body, uint16_t len, struct settings *s)
{
	struct settings t = *s;
	struct ci_json_it it;
	struct ci_json_member m;
	uint8_t scale_given = 0;
	int r;

	if (!ci_pe_can_set(res))
		return res < CI_PE_RESOURCE_COUNT ? CI_PE_STATUS_NOT_ALLOWED : CI_PE_STATUS_NOT_FOUND;
	ci_json_it_init(&it, body, len);
	while ((r = ci_json_next(&it, &m)) == 1) {
		uint8_t ok = 1;
		if (res == CI_PE_SCALE_SETTINGS) {
			if (ci_json_key_is(&m, "root"))
				ok = set_int(&m, 0, 11, 0, &t.root);
			else if (ci_json_key_is(&m, "scale"))
				ok = scale_given = set_int(&m, 0, SCALE_COUNT - 1, 0, &t.scale);
			else if (ci_json_key_is(&m, "tuning"))
				ok = set_int(&m, 0, 255, 0, &t.tuning) && tuning_find(t.tuning);
			else if (ci_json_key_is(&m, "a4Tenths"))
				ok = set_u16(&m, A4_TENTHS_MIN, A4_TENTHS_MAX, &t.a4_tenths);
			else if (ci_json_key_is(&m, "octave"))
				ok = set_int(&m, 0, 7, 0, &t.octave);
			else if (ci_json_key_is(&m, "layout"))
				ok = set_enum(&m, LAYOUTS, LAYOUT_COUNT, &t.layout);
			else if (ci_json_key_is(&m, "absoluteRoot"))
				ok = set_bool(&m, &t.absolute_root);
		} else if (res == CI_PE_SURFACE_SETTINGS) {
			if (ci_json_key_is(&m, "palette"))
				ok = set_palette(&m, &t.palette);
			else if (ci_json_key_is(&m, "split"))
				ok = set_bool(&m, &t.split);
			else if (ci_json_key_is(&m, "upperPalette"))
				ok = set_palette(&m, &t.upper_palette);
			else if (ci_json_key_is(&m, "upperChannel"))
				ok = set_int(&m, 1, 16, -1, &t.upper_channel);
		} else {
			if (ci_json_key_is(&m, "velocityMode"))
				ok = set_enum(&m, VEL_MODES, 4, &t.velocity_mode);
			else if (ci_json_key_is(&m, "velocityCurve"))
				ok = set_enum(&m, CURVES, 3, &t.velocity_curve);
			else if (ci_json_key_is(&m, "pressureMode"))
				ok = set_enum(&m, PRESSURES, 3, &t.pressure_mode);
			else if (ci_json_key_is(&m, "pressureThreshold"))
				ok = set_enum(&m, THRESHOLDS, THRESH_COUNT, &t.pressure_threshold);
			else if (ci_json_key_is(&m, "pressureSmoothing"))
				ok = set_enum(&m, SMOOTHINGS, 4, &t.pressure_smoothing);
			else if (ci_json_key_is(&m, "dinMirror"))
				ok = set_bool(&m, &t.din_mirror);
			else if (ci_json_key_is(&m, "channel"))
				ok = set_int(&m, 1, 16, -1, &t.channel);
		}
		if (!ok)
			return CI_PE_STATUS_BAD_REQUEST;
	}
	/* a new tuning without a scale keeps the scale if it can, else takes
	 * the tuning's default; a scale given that the tuning doesn't offer is
	 * an error (settings_valid) */
	if (r == 0 && !scale_given)
		t.scale = settings_scale_for(t.tuning, t.scale);
	if (r < 0 || !settings_valid(&t))
		return CI_PE_STATUS_BAD_REQUEST;
	*s = t;
	return CI_PE_STATUS_OK;
}

uint8_t ci_pe_changed(const struct settings *a, const struct settings *b)
{
	uint8_t bits = 0;

	if (a->root != b->root || a->scale != b->scale || a->octave != b->octave ||
	    a->layout != b->layout || a->absolute_root != b->absolute_root ||
	    a->tuning != b->tuning || a->a4_tenths != b->a4_tenths)
		bits |= CI_PE_BIT(CI_PE_SCALE_SETTINGS);
	if (a->velocity_mode != b->velocity_mode || a->velocity_curve != b->velocity_curve ||
	    a->pressure_mode != b->pressure_mode || a->pressure_threshold != b->pressure_threshold ||
	    a->pressure_smoothing != b->pressure_smoothing || a->din_mirror != b->din_mirror ||
	    a->channel != b->channel)
		bits |= CI_PE_BIT(CI_PE_PLAY_OPTIONS);
	if (a->palette != b->palette || a->split != b->split ||
	    a->upper_palette != b->upper_palette || a->upper_channel != b->upper_channel)
		bits |= CI_PE_BIT(CI_PE_SURFACE_SETTINGS);
	if (a->channel != b->channel || a->split != b->split ||
	    (a->split && a->upper_channel != b->upper_channel))
		bits |= CI_PE_BIT(CI_PE_CHANNEL_LIST);
	return bits;
}
