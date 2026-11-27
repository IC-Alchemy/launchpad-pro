/* MIDI-CI Property Exchange: the JSON writer and reader, and the resources. */
#include <stdlib.h>
#include "test.h"
#include "midici/ci_json.h"
#include "midici/ci_pe.h"
#include "instrument/palette.h"
#include "instrument/settings.h"
#include "scales/scales.h"
#include "scales/tuning.h"

static uint16_t render(uint8_t res, const struct settings *s, const char *piid, char *buf, uint16_t cap)
{
	struct ci_json_out o;
	ci_json_out_init(&o, buf, 0, cap);
	ci_pe_render(res, s, piid, &o);
	if (o.pos < cap)
		buf[o.pos] = 0;
	return o.pos;
}

static uint16_t apply(uint8_t res, const char *body, struct settings *s)
{
	return ci_pe_apply_set(res, body, (uint16_t)strlen(body), s);
}

static uint8_t same(const struct settings *a, const struct settings *b)
{
	return memcmp(a, b, sizeof *a) == 0;
}

static void test_json_writer_window(void)
{
	char full[64], win[8];
	struct ci_json_out o;

	ci_json_out_init(&o, full, 0, sizeof full);
	ci_json_raw(&o, "{");
	ci_json_key(&o, "a\"b\\");
	ci_json_int(&o, -2147483647 - 1);
	ci_json_raw(&o, ",");
	ci_json_key(&o, "t");
	ci_json_bool(&o, 1);
	ci_json_raw(&o, ",");
	ci_json_key(&o, "s");
	ci_json_str(&o, "x\x01\xC3y");
	ci_json_raw(&o, "}");
	full[o.pos] = 0;
	CHECK(strcmp(full, "{\"a\\\"b\\\\\":-2147483648,\"t\":true,\"s\":\"x??y\"}") == 0);

	/* a window gets exactly its slice; pos still counts everything */
	uint16_t total = o.pos;
	memset(win, '#', sizeof win);
	ci_json_out_init(&o, win, 5, 6);
	ci_json_raw(&o, full);
	CHECK_EQ(o.pos, total);
	CHECK_MEM(win, full + 5, 6);
	CHECK_EQ(win[6], '#');

	/* counting only */
	ci_json_out_init(&o, 0, 0, 100);
	ci_json_int(&o, 1234);
	CHECK_EQ(o.pos, 4);
}

static void test_json_reader(void)
{
	static const char J[] =
		" { \"a\" : 12 , \"b\":\"x\\\"y\", \"c\":[1,{\"d\":[2,3]},\"]\"], \"e\":{}, "
		"\"f\":false, \"g\":null, \"h\":1.5, \"i\":-7, \"j\":1234567890 } ";
	struct ci_json_it it;
	struct ci_json_member m;

	ci_json_it_init(&it, J, (uint16_t)(sizeof J - 1));
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "a"));
	CHECK_EQ(m.type, CI_JSON_INT);
	CHECK_EQ(m.num, 12);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "b"));
	CHECK_EQ(m.type, CI_JSON_STR);
	CHECK_EQ(m.str_len, 4);	/* raw: x\"y */
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "c"));
	CHECK_EQ(m.type, CI_JSON_OTHER);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "e"));
	CHECK_EQ(m.type, CI_JSON_OTHER);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "f"));
	CHECK_EQ(m.type, CI_JSON_BOOL);
	CHECK_EQ(m.num, 0);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK_EQ(m.type, CI_JSON_NULL);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "h"));
	CHECK_EQ(m.type, CI_JSON_OTHER);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK_EQ(m.type, CI_JSON_INT);
	CHECK(m.num == -7);
	CHECK_EQ(ci_json_next(&it, &m), 1);
	CHECK(ci_json_key_is(&m, "j"));
	CHECK_EQ(m.type, CI_JSON_OTHER);	/* 10 digits */
	CHECK_EQ(ci_json_next(&it, &m), 0);
	CHECK_EQ(ci_json_next(&it, &m), 0);

	ci_json_it_init(&it, "{}", 2);
	CHECK_EQ(ci_json_next(&it, &m), 0);

	static const char *const BAD[] = {
		"", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":1 \"b\":2}",
		"{\"a\":tru}", "{\"a\":[1,2}", "{\"a\":{\"b\" 1}}", "{\"a\":1}x", "{\"a\":\"\x01\"}",
		"{\"a\":[[[[[[[[[1]]]]]]]]]}", "{\"a\":-}", "{a:1}",
	};
	for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
		int r;
		ci_json_it_init(&it, BAD[i], (uint16_t)strlen(BAD[i]));
		while ((r = ci_json_next(&it, &m)) == 1)
			;
		if (r != -1)
			printf("  json accepted: %s\n", BAD[i]);
		CHECK_EQ(r, -1);
		CHECK_EQ(ci_json_next(&it, &m), -1);
	}
}

static void test_pe_lookup(void)
{
	CHECK_EQ(ci_pe_lookup("ResourceList", 12), CI_PE_RESOURCE_LIST);
	CHECK_EQ(ci_pe_lookup("DeviceInfo", 10), CI_PE_DEVICE_INFO);
	CHECK_EQ(ci_pe_lookup("X-ScaleSettings", 15), CI_PE_SCALE_SETTINGS);
	CHECK_EQ(ci_pe_lookup("X-PlayOptionsX", 13), CI_PE_PLAY_OPTIONS);
	CHECK_EQ(ci_pe_lookup("X-PlayOption", 12), CI_PE_NONE);
	CHECK_EQ(ci_pe_lookup("ProgramList", 11), CI_PE_NONE);
	CHECK_EQ(ci_pe_lookup("", 0), CI_PE_NONE);
	for (uint8_t r = 0; r < CI_PE_RESOURCE_COUNT; r++)
		CHECK_EQ(ci_pe_lookup(ci_pe_name(r), (uint16_t)strlen(ci_pe_name(r))), r);
	CHECK(ci_pe_can_set(CI_PE_SCALE_SETTINGS) && ci_pe_can_set(CI_PE_PLAY_OPTIONS));
	CHECK(!ci_pe_can_set(CI_PE_DEVICE_INFO) && !ci_pe_can_set(CI_PE_RESOURCE_LIST));
	CHECK(ci_pe_can_subscribe(CI_PE_PLAY_OPTIONS) && !ci_pe_can_subscribe(CI_PE_SCALE_LIST));
}

static void test_pe_fixed_resources(void)
{
	static char buf[4096];
	struct settings s;
	uint16_t n;

	settings_defaults(&s);
	n = render(CI_PE_RESOURCE_LIST, &s, "", buf, sizeof buf);
	CHECK(n < sizeof buf);
	CHECK_EQ(n, ci_pe_body_len(CI_PE_RESOURCE_LIST, &s, ""));
	CHECK(buf[0] == '[' && buf[n - 1] == ']');
	CHECK(strstr(buf, "\"resource\":\"DeviceInfo\"") != 0);
	CHECK(strstr(buf, "\"resource\":\"X-ScaleList\"") != 0);
	CHECK(strstr(buf, "\"resource\":\"ResourceList\"") == 0);

	n = render(CI_PE_DEVICE_INFO, &s, "0123456789ABCDEF01234567", buf, sizeof buf);
	CHECK(strstr(buf, "\"manufacturerId\":[0,32,41]") != 0);
	CHECK(strstr(buf, "\"manufacturer\":\"Novation\"") != 0);
	CHECK(strstr(buf, "\"familyId\":[81,0]") != 0);
	CHECK(strstr(buf, "\"modelId\":[0,0]") != 0);
	CHECK(strstr(buf, "\"model\":\"Launchpad Pro MIDI 2.0\"") != 0);
	CHECK(strstr(buf, "\"versionId\":[0,1,0,0]") != 0);
	CHECK(strstr(buf, "\"serialNumber\":\"0123456789ABCDEF01234567\"") != 0);
	render(CI_PE_DEVICE_INFO, &s, "", buf, sizeof buf);
	CHECK(strstr(buf, "serialNumber") == 0);

	s.channel = 9;
	render(CI_PE_CHANNEL_LIST, &s, "", buf, sizeof buf);
	CHECK(strcmp(buf, "[{\"title\":\"Standalone Port\",\"channel\":10}]") == 0);

	render(CI_PE_SCALE_LIST, &s, "", buf, sizeof buf);
	CHECK(buf[0] == '[');
	CHECK(strncmp(buf + 1, "{\"name\":", 8) == 0);
	CHECK(strstr(buf, SCALES[0].name) != 0);
	CHECK(strstr(buf, SCALES[SCALE_COUNT - 1].name) != 0);

	render(CI_PE_TUNING_LIST, &s, "", buf, sizeof buf);
	CHECK(strncmp(buf, "[{\"id\":0,\"name\":\"12-EDO (Standard)\",\"degrees\":12},", 47) == 0);
	CHECK(strstr(buf, "{\"id\":1,\"name\":\"24-EDO Quarter-Tone\",\"degrees\":24,\"scales\":[32,33,34,35,31]}") != 0);
	CHECK(strstr(buf, "{\"id\":131,\"name\":\"Carlos Gamma\",\"degrees\":1,\"scales\":[31]}]") != 0);
	CHECK_EQ(ci_pe_lookup("X-TuningList", 12), CI_PE_TUNING_LIST);
	CHECK(!ci_pe_can_set(CI_PE_TUNING_LIST));

	/* every resource fits in a window slice by slice */
	for (uint8_t r = 0; r < CI_PE_RESOURCE_COUNT; r++) {
		static char whole[4096], part[4096];
		uint16_t len = render(r, &s, "PIID", whole, sizeof whole);
		for (uint16_t at = 0; at < len; at += 37) {
			struct ci_json_out o;
			uint16_t w = (uint16_t)(len - at < 37 ? len - at : 37);
			ci_json_out_init(&o, part + at, at, w);
			ci_pe_render(r, &s, "PIID", &o);
		}
		CHECK_MEM(part, whole, len);
	}
}

static void test_pe_settings_round_trip(void)
{
	static char buf[512];
	struct settings s, t;

	/* every enum value of every field renders, and a Set of that body gives it back */
	for (uint8_t v = 0; v < 25; v++) {
		settings_defaults(&s);
		s.root = (uint8_t)(v % 12);
		s.scale = v;
		s.octave = (uint8_t)(v % 8);
		s.layout = (uint8_t)(v % LAYOUT_COUNT);
		s.absolute_root = v & 1;
		s.velocity_mode = (uint8_t)(v % 4);
		s.velocity_curve = (uint8_t)(v % 3);
		s.pressure_mode = (uint8_t)((v + 1) % 3);
		s.pressure_threshold = (uint8_t)((v + 2) % THRESH_COUNT);
		s.pressure_smoothing = (uint8_t)((v + 3) % 4);
		s.din_mirror = (v >> 1) & 1;
		s.channel = (uint8_t)(v % 16);
		CHECK(settings_valid(&s));

		settings_defaults(&t);
		render(CI_PE_SCALE_SETTINGS, &s, "", buf, sizeof buf);
		CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, buf, &t), CI_PE_STATUS_OK);
		render(CI_PE_PLAY_OPTIONS, &s, "", buf, sizeof buf);
		CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, buf, &t), CI_PE_STATUS_OK);
		CHECK(same(&s, &t));
	}

	/* every tuning with every scale it offers, at a few reference pitches */
	for (uint8_t i = 0; i < TUNING_COUNT; i++) {
		const struct tuning_def *tu = &TUNINGS[i];
		for (uint8_t j = 0; j < tuning_scale_slots(tu); j++) {
			settings_defaults(&s);
			s.tuning = tu->id;
			s.scale = tuning_scale_at(tu, j);
			s.a4_tenths = (uint16_t)(A4_TENTHS_MIN + (i * 37 + j * 11) % (A4_TENTHS_MAX - A4_TENTHS_MIN + 1));
			CHECK(settings_valid(&s));
			settings_defaults(&t);
			render(CI_PE_SCALE_SETTINGS, &s, "", buf, sizeof buf);
			CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, buf, &t), CI_PE_STATUS_OK);
			CHECK(same(&s, &t));
		}
	}

	settings_defaults(&s);
	render(CI_PE_SCALE_SETTINGS, &s, "", buf, sizeof buf);
	CHECK(strncmp(buf, "{\"root\":", 8) == 0);
	CHECK(strstr(buf, "\"tuning\":0,\"a4Tenths\":4400}") != 0);
	CHECK(strstr(buf, "\"absoluteRoot\":") != 0);
	s.velocity_mode = VEL_TWO_STAGE;
	s.channel = 15;
	render(CI_PE_PLAY_OPTIONS, &s, "", buf, sizeof buf);
	CHECK(strstr(buf, "\"velocityMode\":\"twoStage\"") != 0);
	CHECK(strstr(buf, "\"channel\":16}") != 0);
}

static void test_pe_set_rules(void)
{
	struct settings s, before;

	settings_defaults(&s);
	s.root = 3;
	s.octave = 4;
	before = s;

	/* missing fields keep their values, unknown ones are ignored */
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{\"scale\":5,\"nonsense\":{\"x\":[1]},\"layout\":\"chromaticGuitar\"}", &s),
	         CI_PE_STATUS_OK);
	CHECK_EQ(s.scale, 5);
	CHECK_EQ(s.layout, 4);
	CHECK_EQ(s.root, 3);
	CHECK_EQ(s.octave, 4);
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{}", &s), CI_PE_STATUS_OK);

	/* instant aftertouch is a fourth pressure threshold */
	CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, "{\"pressureThreshold\":\"instant\"}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.pressure_threshold, THRESH_INSTANT);
	CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, "{\"pressureThreshold\":\"med\"}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.pressure_threshold, THRESH_MED);

	/* any bad field rejects the whole Set and changes nothing */
	before = s;
	static const char *const BAD_SCALE[] = {
		"{\"scale\":6,\"root\":12}", "{\"root\":-1}", "{\"scale\":61}", "{\"scale\":31}",
		"{\"tuning\":12}", "{\"tuning\":1,\"scale\":0}", "{\"a4Tenths\":4149}", "{\"a4Tenths\":4661}",
		"{\"a4Tenths\":440.5}", "{\"octave\":8}",
		"{\"layout\":\"diagonal\"}", "{\"layout\":2}", "{\"absoluteRoot\":1}", "{\"root\":\"3\"}",
		"{\"root\":3.0}", "{\"root\":null}", "{\"scale\":6", "[1]", "",
	};
	for (unsigned i = 0; i < sizeof BAD_SCALE / sizeof BAD_SCALE[0]; i++) {
		CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, BAD_SCALE[i], &s), CI_PE_STATUS_BAD_REQUEST);
		CHECK(same(&s, &before));
	}
	static const char *const BAD_PLAY[] = {
		"{\"channel\":0}", "{\"channel\":17}", "{\"velocityMode\":\"PeakWindow\"}",
		"{\"velocityCurve\":\"\"}", "{\"pressureMode\":true}", "{\"pressureThreshold\":\"medium\"}",
		"{\"pressureSmoothing\":\"max\"}", "{\"dinMirror\":\"true\"}",
	};
	for (unsigned i = 0; i < sizeof BAD_PLAY / sizeof BAD_PLAY[0]; i++) {
		CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, BAD_PLAY[i], &s), CI_PE_STATUS_BAD_REQUEST);
		CHECK(same(&s, &before));
	}

	/* a tuning without a scale keeps the scale if offered, else takes the
	 * tuning's default */
	settings_defaults(&s);
	s.scale = 1;
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{\"tuning\":96}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.tuning, 96);
	CHECK_EQ(s.scale, 1);
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{\"tuning\":3,\"a4Tenths\":4320}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.scale, 39);
	CHECK_EQ(s.a4_tenths, 4320);
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{\"scale\":40,\"tuning\":3}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.scale, 40);
	CHECK_EQ(apply(CI_PE_SCALE_SETTINGS, "{\"tuning\":0}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.scale, 0);
	settings_defaults(&s);
	s.root = 3;
	before = s;

	/* a Scale field in a PlayOptions Set is an unknown field there */
	CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, "{\"root\":11,\"channel\":2,\"dinMirror\":true}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.root, before.root);
	CHECK_EQ(s.channel, 1);
	CHECK_EQ(s.din_mirror, 1);

	before = s;
	CHECK_EQ(apply(CI_PE_DEVICE_INFO, "{}", &s), CI_PE_STATUS_NOT_ALLOWED);
	CHECK_EQ(apply(CI_PE_RESOURCE_LIST, "{}", &s), CI_PE_STATUS_NOT_ALLOWED);
	CHECK_EQ(apply(CI_PE_SCALE_LIST, "{}", &s), CI_PE_STATUS_NOT_ALLOWED);
	CHECK_EQ(apply(CI_PE_NONE, "{}", &s), CI_PE_STATUS_NOT_FOUND);
	CHECK(same(&s, &before));
}

static void test_pe_surface_settings(void)
{
	static char buf[512];
	struct settings s, t, before;

	CHECK_EQ(ci_pe_lookup("X-SurfaceSettings", 17), CI_PE_SURFACE_SETTINGS);
	CHECK(ci_pe_can_set(CI_PE_SURFACE_SETTINGS) && ci_pe_can_subscribe(CI_PE_SURFACE_SETTINGS));
	settings_defaults(&s);
	render(CI_PE_SURFACE_SETTINGS, &s, "", buf, sizeof buf);
	CHECK(strcmp(buf, "{\"palette\":\"ocean\",\"split\":false,\"splitMode\":\"off\",\"upperPalette\":\"contrast\",\"upperChannel\":2}") == 0);

	/* Every palette in both halves, every upper channel, all split modes. */
	for (uint8_t v = 0; v < 32; v++) {
		settings_defaults(&s);
		s.palette = (uint8_t)(v % PALETTE_COUNT);
		s.upper_palette = (uint8_t)((v / 2) % PALETTE_COUNT);
		s.upper_channel = (uint8_t)(v % 16);
		s.split_mode = (uint8_t)(v % SPLIT_COUNT);
		CHECK(settings_valid(&s));
		settings_defaults(&t);
		render(CI_PE_SURFACE_SETTINGS, &s, "", buf, sizeof buf);
		CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, buf, &t), CI_PE_STATUS_OK);
		CHECK(same(&s, &t));
	}

	/* a Set changes only what it names */
	settings_defaults(&s);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"split\":true,\"upperChannel\":16}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.split_mode, SPLIT_X);
	CHECK_EQ(s.upper_channel, 15);
	CHECK_EQ(s.palette, PALETTE_OCEAN);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"palette\":\"contrast\"}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.palette, PALETTE_CONTRAST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"splitMode\":\"y\",\"split\":false}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.split_mode, SPLIT_Y);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"split\":true}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.split_mode, SPLIT_Y);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"split\":false}", &s), CI_PE_STATUS_OK);
	CHECK_EQ(s.split_mode, SPLIT_OFF);

	/* one bad field rejects the whole Set */
	before = s;
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"split\":false,\"palette\":\"teal\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"upperChannel\":0}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"upperChannel\":17}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"upperPalette\":3}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"split\":1}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"splitMode\":\"diagonal\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"palette\":\"ember\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"palette\":\"forest\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"upperPalette\":\"rose\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK_EQ(apply(CI_PE_SURFACE_SETTINGS, "{\"upperPalette\":\"amethyst\"}", &s), CI_PE_STATUS_BAD_REQUEST);
	CHECK(same(&s, &before));
	/* the other resources ignore the surface fields */
	CHECK_EQ(apply(CI_PE_PLAY_OPTIONS, "{\"palette\":\"ember\"}", &s), CI_PE_STATUS_OK);
	CHECK(same(&s, &before));

	/* ChannelList: one entry per half when split */
	settings_defaults(&s);
	s.channel = 2;
	s.upper_channel = 6;
	render(CI_PE_CHANNEL_LIST, &s, "", buf, sizeof buf);
	CHECK(strcmp(buf, "[{\"title\":\"Standalone Port\",\"channel\":3}]") == 0);
	s.split_mode = SPLIT_X;
	render(CI_PE_CHANNEL_LIST, &s, "", buf, sizeof buf);
	CHECK(strcmp(buf, "[{\"title\":\"Standalone Lower\",\"channel\":3},"
	                  "{\"title\":\"Standalone Upper\",\"channel\":7}]") == 0);

	s.split_mode = SPLIT_Y;
	render(CI_PE_CHANNEL_LIST, &s, "", buf, sizeof buf);
	CHECK(strcmp(buf, "[{\"title\":\"Standalone Left\",\"channel\":3},"
	                  "{\"title\":\"Standalone Right\",\"channel\":7}]") == 0);

	/* the schema lists every palette by its name */
	{
		static char list[4096];
		render(CI_PE_RESOURCE_LIST, &s, "", list, sizeof list);
		CHECK(strstr(list, "{\"resource\":\"X-SurfaceSettings\",\"canSet\":\"partial\",\"canSubscribe\":true,") != 0);
		for (uint8_t p = 0; p < PALETTE_COUNT; p++) {
			char quoted[24];
			snprintf(quoted, sizeof quoted, "\"%s\"", palette_name(p));
			CHECK(strstr(list, quoted) != 0);
		}
	}
}

static void test_pe_changed(void)
{
	struct settings a, b;

	settings_defaults(&a);
	b = a;
	CHECK_EQ(ci_pe_changed(&a, &b), 0);
	b.octave++;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SCALE_SETTINGS));
	b = a;
	b.pressure_smoothing = (uint8_t)((a.pressure_smoothing + 1) % 4);
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_PLAY_OPTIONS));
	b = a;
	b.channel = (uint8_t)((a.channel + 1) % 16);
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_PLAY_OPTIONS) | CI_PE_BIT(CI_PE_CHANNEL_LIST));
	b.layout = (uint8_t)((a.layout + 1) % LAYOUT_COUNT);
	CHECK_EQ(ci_pe_changed(&a, &b),
	         CI_PE_BIT(CI_PE_PLAY_OPTIONS) | CI_PE_BIT(CI_PE_CHANNEL_LIST) | CI_PE_BIT(CI_PE_SCALE_SETTINGS));

	/* surface: the palettes touch only X-SurfaceSettings; split and the
	 * upper channel (while split) change the ChannelList too */
	b = a;
	b.palette = PALETTE_CITRUS;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SURFACE_SETTINGS));
	b = a;
	b.upper_palette = PALETTE_CITRUS;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SURFACE_SETTINGS));
	b = a;
	b.upper_channel = 7;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SURFACE_SETTINGS));
	b.split_mode = SPLIT_X;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SURFACE_SETTINGS) | CI_PE_BIT(CI_PE_CHANNEL_LIST));
	a.split_mode = SPLIT_X;
	CHECK_EQ(ci_pe_changed(&a, &b), CI_PE_BIT(CI_PE_SURFACE_SETTINGS) | CI_PE_BIT(CI_PE_CHANNEL_LIST));
}

static void test_json_pe_header_resource(void)
{
	struct ci_json_it it;
	struct ci_json_member m;
	int r, found;
	uint8_t res;

	/* Standard PE request header */
	static const char STD[] = "{\"mutualEncoding\":\"ASCII\",\"resource\":\"DeviceInfo\",\"setPartial\":false}";
	ci_json_it_init(&it, STD, (uint16_t)(sizeof STD - 1));
	found = 0;
	res = CI_PE_NONE;
	while ((r = ci_json_next(&it, &m)) == 1) {
		if (ci_json_key_is(&m, "resource")) {
			found++;
			CHECK_EQ(m.type, CI_JSON_STR);
			CHECK(ci_json_str_is(&m, "DeviceInfo"));
			res = ci_pe_lookup(m.str, m.str_len);
		}
	}
	CHECK_EQ(r, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(res, CI_PE_DEVICE_INFO);

	/* Nested "resource" inside another object is skipped; top-level wins */
	static const char NESTED[] = "{\"meta\":{\"resource\":\"DeviceInfo\"},\"resource\":\"X-ScaleSettings\"}";
	ci_json_it_init(&it, NESTED, (uint16_t)(sizeof NESTED - 1));
	found = 0;
	res = CI_PE_NONE;
	while ((r = ci_json_next(&it, &m)) == 1) {
		if (ci_json_key_is(&m, "resource")) {
			found++;
			CHECK_EQ(m.type, CI_JSON_STR);
			res = ci_pe_lookup(m.str, m.str_len);
		}
	}
	CHECK_EQ(r, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(res, CI_PE_SCALE_SETTINGS);

	/* Non-string "resource" values are not treated as CI_JSON_STR */
	static const char *const NON_STR[] = {
		"{\"resource\":123}",
		"{\"resource\":null}",
		"{\"resource\":[\"DeviceInfo\"]}",
	};
	for (unsigned i = 0; i < sizeof NON_STR / sizeof NON_STR[0]; i++) {
		ci_json_it_init(&it, NON_STR[i], (uint16_t)strlen(NON_STR[i]));
		found = 0;
		while ((r = ci_json_next(&it, &m)) == 1) {
			if (ci_json_key_is(&m, "resource")) {
				found++;
				CHECK(m.type != CI_JSON_STR);
			}
		}
		CHECK_EQ(r, 0);
		CHECK_EQ(found, 1);
	}

	/* Missing "resource" */
	static const char MISSING[] = "{\"command\":\"start\",\"mutualEncoding\":\"ASCII\"}";
	ci_json_it_init(&it, MISSING, (uint16_t)(sizeof MISSING - 1));
	found = 0;
	while ((r = ci_json_next(&it, &m)) == 1) {
		if (ci_json_key_is(&m, "resource"))
			found++;
	}
	CHECK_EQ(r, 0);
	CHECK_EQ(found, 0);
}

void test_ci_pe_all(void)
{
	RUN(test_json_writer_window);
	RUN(test_json_reader);
	RUN(test_json_pe_header_resource);
	RUN(test_pe_lookup);
	RUN(test_pe_fixed_resources);
	RUN(test_pe_settings_round_trip);
	RUN(test_pe_set_rules);
	RUN(test_pe_surface_settings);
	RUN(test_pe_changed);
}
