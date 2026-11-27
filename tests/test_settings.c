#include "test.h"
#include "instrument/settings.h"
#include "instrument/palette.h"
#include "scales/scales.h"
#include "stubs/io_stub.h"

static int same(const struct settings *a, const struct settings *b)
{
	return memcmp(a, b, sizeof *a) == 0;
}

static void test_defaults_valid(void)
{
	struct settings s;
	settings_defaults(&s);
	CHECK(settings_valid(&s));
	CHECK_EQ(s.root, 0);
	CHECK_EQ(s.scale, 0);
	CHECK_EQ(s.octave, 4);
	CHECK_EQ(s.layout, 0);
	CHECK_EQ(s.absolute_root, 0);
	CHECK_EQ(s.velocity_mode, VEL_PEAK_WINDOW);
	CHECK_EQ(s.velocity_curve, CURVE_LINEAR);
	CHECK_EQ(s.pressure_mode, PRESS_POLY);
	CHECK_EQ(s.pressure_threshold, THRESH_MED);
	CHECK_EQ(s.din_mirror, 0);
	CHECK_EQ(s.channel, 0);
	CHECK_EQ(s.pressure_smoothing, SMOOTH_LIGHT);
	CHECK_EQ(s.tuning, 0);
	CHECK_EQ(s.a4_tenths, 4400);
}

static void test_record_round_trip(void)
{
	struct settings s, d, back;
	uint8_t rec[SETTINGS_RECORD_LEN];
	unsigned sum = 0;
	int i;

	settings_defaults(&s);
	s.root = 7; s.scale = 24; s.octave = 2; s.layout = 4; s.absolute_root = 1;
	s.velocity_mode = VEL_TWO_STAGE; s.velocity_curve = CURVE_HARD;
	s.pressure_mode = PRESS_OFF; s.pressure_threshold = THRESH_HIGH;
	s.din_mirror = 1; s.channel = 15; s.pressure_smoothing = SMOOTH_HEAVY;
	s.tuning = 96; s.scale = 27; s.a4_tenths = 4321;
	s.palette = PALETTE_CONTRAST; s.split = 1; s.upper_palette = PALETTE_FOREST; s.upper_channel = 9;
	settings_encode(&s, rec);
	CHECK_EQ(SETTINGS_RECORD_LEN, 25);
	CHECK_MEM(rec, "LPM2\x04", 5);
	CHECK_EQ(rec[16], SMOOTH_HEAVY);
	CHECK_EQ(rec[17], 96);
	CHECK_EQ(rec[18], 4321 & 0xFF);
	CHECK_EQ(rec[19], 4321 >> 8);
	CHECK_EQ(rec[20], PALETTE_CONTRAST);
	CHECK_EQ(rec[21], 1);
	CHECK_EQ(rec[22], PALETTE_FOREST);
	CHECK_EQ(rec[23], 9);
	for (i = 0; i < SETTINGS_RECORD_LEN; i++) sum += rec[i];
	CHECK_EQ(sum & 0xFF, 0);
	settings_defaults(&d);
	back = d;
	CHECK_EQ(settings_decode(rec, &back), 1);
	CHECK(same(&back, &s));

	/* a failed decode leaves the target untouched */
	rec[5] = 12;
	rec[20] = (uint8_t)(rec[20] - 5);
	back = d;
	CHECK_EQ(settings_decode(rec, &back), 0);
	CHECK(same(&back, &d));
}

static void test_velocity_mode_range(void)
{
	struct settings s;

	settings_defaults(&s);
	s.velocity_mode = 3;
	CHECK_EQ(settings_valid(&s), 1);
	CHECK_EQ(VEL_TWO_STAGE, 3);
	s.velocity_mode = 4;
	CHECK_EQ(settings_valid(&s), 0);
}

static void test_smoothing_range(void)
{
	struct settings s;

	settings_defaults(&s);
	s.pressure_smoothing = 3;
	CHECK_EQ(settings_valid(&s), 1);
	CHECK_EQ(SMOOTH_HEAVY, 3);
	s.pressure_smoothing = 4;
	CHECK_EQ(settings_valid(&s), 0);
}

/* a version 1 record: 17 bytes, no smoothing, checksum at 16; the byte
 * after it is erased flash */
static void test_v1_record_loads_with_default_smoothing(void)
{
	struct settings s, d;
	uint8_t v1[17] = {'L', 'P', 'M', '2', 1, 7, 24, 2, 4, 1, 2, 1, 0, 1, 1, 9, 0};
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 16; i++) sum = (uint8_t)(sum + v1[i]);
	v1[16] = (uint8_t)(0u - sum);
	io_stub_reset();
	memcpy(io_flash, v1, 17);
	memset(io_flash + 17, 0xFF, 8);
	memset(&s, 0x55, sizeof s);
	settings_load(&s);
	settings_defaults(&d);
	CHECK_EQ(s.root, 7);
	CHECK_EQ(s.scale, 24);
	CHECK_EQ(s.octave, 2);
	CHECK_EQ(s.channel, 9);
	CHECK_EQ(s.din_mirror, 1);
	CHECK_EQ(s.pressure_smoothing, SMOOTH_LIGHT);
	CHECK_EQ(io_flash_writes, 0);
	/* a bad v1 checksum is rejected */
	v1[16] ^= 1;
	memcpy(io_flash, v1, 17);
	settings_load(&s);
	CHECK(same(&s, &d));
}

static void test_v2_record_loads_twelve_edo(void)
{
	struct settings s;
	uint8_t v2[18] = {'L', 'P', 'M', '2', 2, 7, 24, 2, 4, 1, 2, 1, 0, 1, 1, 9, 3, 0};
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 17; i++) sum = (uint8_t)(sum + v2[i]);
	v2[17] = (uint8_t)(0u - sum);
	io_stub_reset();
	memset(io_flash, 0xFF, sizeof io_flash);
	memcpy(io_flash, v2, 18);
	settings_load(&s);
	CHECK_EQ(s.root, 7);
	CHECK_EQ(s.scale, 24);
	CHECK_EQ(s.pressure_smoothing, 3);
	CHECK_EQ(s.tuning, 0);
	CHECK_EQ(s.a4_tenths, 4400);
}

/* a record from before the surface fields: its settings, Ocean, no split */
static void test_v3_record_loads_without_split(void)
{
	struct settings s;
	uint8_t v3[21] = {'L', 'P', 'M', '2', 3, 7, 27, 2, 4, 1, 2, 1, 0, 1, 1, 9, 3, 96,
	                  4321 & 0xFF, 4321 >> 8, 0};
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 20; i++) sum = (uint8_t)(sum + v3[i]);
	v3[20] = (uint8_t)(0u - sum);
	io_stub_reset();
	memset(io_flash, 0xFF, sizeof io_flash);
	memcpy(io_flash, v3, 21);
	settings_load(&s);
	CHECK_EQ(s.root, 7);
	CHECK_EQ(s.scale, 27);
	CHECK_EQ(s.channel, 9);
	CHECK_EQ(s.tuning, 96);
	CHECK_EQ(s.a4_tenths, 4321);
	CHECK_EQ(s.palette, PALETTE_OCEAN);
	CHECK_EQ(s.split, 0);
	CHECK_EQ(s.upper_palette, PALETTE_EMBER);
	CHECK_EQ(s.upper_channel, 1);
}

static void test_surface_fields_validated(void)
{
	struct settings s;

	settings_defaults(&s);
	CHECK_EQ(s.palette, PALETTE_OCEAN);
	CHECK_EQ(s.split, 0);
	CHECK(s.upper_channel != s.channel);  /* split on: two channels at once */
	s.palette = PALETTE_COUNT - 1;
	s.upper_palette = PALETTE_COUNT - 1;
	s.upper_channel = 15;
	s.split = 1;
	CHECK(settings_valid(&s));
	s.palette = PALETTE_COUNT;
	CHECK(!settings_valid(&s));
	s.palette = 0;
	s.upper_palette = PALETTE_COUNT;
	CHECK(!settings_valid(&s));
	s.upper_palette = 0;
	s.upper_channel = 16;
	CHECK(!settings_valid(&s));
	s.upper_channel = 0;
	s.split = 2;
	CHECK(!settings_valid(&s));
}

static void test_tuning_fields_validated(void)
{
	struct settings s;

	settings_defaults(&s);
	s.tuning = 12; /* no such id */
	CHECK(!settings_valid(&s));
	s.tuning = 1;  /* 24-EDO doesn't offer Major */
	CHECK(!settings_valid(&s));
	s.scale = 32;  /* Maqam Rast */
	CHECK(settings_valid(&s));
	s.a4_tenths = 4149;
	CHECK(!settings_valid(&s));
	s.a4_tenths = 4660;
	CHECK(settings_valid(&s));
	s.a4_tenths = 4661;
	CHECK(!settings_valid(&s));
	/* the scale a tuning change keeps */
	CHECK_EQ(settings_scale_for(0, 5), 5);
	CHECK_EQ(settings_scale_for(1, 5), 32);
	CHECK_EQ(settings_scale_for(96, 1), 1);
	CHECK_EQ(settings_scale_for(96, 7), 16);
	CHECK_EQ(settings_scale_for(97, 31), 16);
	CHECK_EQ(settings_scale_for(129, 0), SCALE_ALL_DEGREES);
}

static void test_blank_flash_loads_defaults(void)
{
	struct settings s, d;
	io_stub_reset();
	settings_defaults(&d);
	memset(&s, 0x55, sizeof s);
	settings_load(&s);
	CHECK(same(&s, &d));
	CHECK_EQ(io_flash_writes, 0);
}

static void load_bad(const uint8_t *rec)
{
	struct settings s, d;
	io_stub_reset();
	memcpy(io_flash, rec, SETTINGS_RECORD_LEN);
	settings_defaults(&d);
	memset(&s, 0x55, sizeof s);
	settings_load(&s);
	CHECK(same(&s, &d));
	CHECK_EQ(io_flash_writes, 0);
}

static void test_corrupt_record_loads_defaults(void)
{
	struct settings s;
	uint8_t rec[SETTINGS_RECORD_LEN];

	settings_defaults(&s);
	s.root = 3;
	settings_encode(&s, rec);
	rec[17] ^= 0xFF;
	load_bad(rec);

	settings_encode(&s, rec);
	rec[4] = 3;
	rec[17] = (uint8_t)(rec[17] - 1);
	load_bad(rec);

	settings_encode(&s, rec);
	rec[6] = 25;
	rec[17] = (uint8_t)(rec[17] - 25);
	load_bad(rec);

	/* smoothing out of range */
	settings_encode(&s, rec);
	rec[16] = 4;
	rec[17] = (uint8_t)(rec[17] - 4);
	load_bad(rec);
}

static void test_save_waits_for_quiet_and_no_notes(void)
{
	struct settings s, r;
	io_stub_reset();
	settings_load(&s);
	s.root = 5;
	settings_touch(100);
	settings_poll(&s, 2099, 0);
	CHECK_EQ(io_flash_writes, 0);
	settings_poll(&s, 2100, 1);
	CHECK_EQ(io_flash_writes, 0);
	settings_poll(&s, 2100, 0);
	CHECK_EQ(io_flash_writes, 1);
	settings_poll(&s, 5000, 0);
	CHECK_EQ(io_flash_writes, 1);
	settings_load(&r);
	CHECK_EQ(r.root, 5);
}

static void test_unchanged_or_reverted_skips_write(void)
{
	struct settings s;
	io_stub_reset();
	settings_load(&s);
	s.scale = 3;
	settings_touch(100);
	settings_poll(&s, 1000, 0);
	CHECK_EQ(io_flash_writes, 0);
	s.scale = 0;
	settings_touch(1200);
	settings_poll(&s, 10000, 0);
	CHECK_EQ(io_flash_writes, 0);
}

static void test_save_quiet_across_wrap(void)
{
	struct settings s;
	io_stub_reset();
	settings_load(&s);
	s.octave = 6;
	settings_touch(0xFFFFFF00u);
	settings_poll(&s, 0x000006CFu, 0);
	CHECK_EQ(io_flash_writes, 0);
	settings_poll(&s, 0x000006D0u, 0);
	CHECK_EQ(io_flash_writes, 1);
}

void test_settings_all(void)
{
	RUN(test_defaults_valid);
	RUN(test_record_round_trip);
	RUN(test_velocity_mode_range);
	RUN(test_smoothing_range);
	RUN(test_v1_record_loads_with_default_smoothing);
	RUN(test_v2_record_loads_twelve_edo);
	RUN(test_v3_record_loads_without_split);
	RUN(test_surface_fields_validated);
	RUN(test_tuning_fields_validated);
	RUN(test_blank_flash_loads_defaults);
	RUN(test_corrupt_record_loads_defaults);
	RUN(test_save_waits_for_quiet_and_no_notes);
	RUN(test_unchanged_or_reverted_skips_write);
	RUN(test_save_quiet_across_wrap);
}
