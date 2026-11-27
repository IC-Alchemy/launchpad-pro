#include "test.h"

int g_test_failures = 0;

int test_failures(void)
{
	return g_test_failures;
}

void test_harness_all(void);
void test_ump_codec_all(void);
void test_ump_tx_all(void);
void test_ump_stream_all(void);
void test_ump_rx_all(void);
void test_usb_desc_all(void);
void test_usb_prop_all(void);
void test_usb_data_all(void);
void test_pad_velocity_all(void);
void test_scales_all(void);
void test_tuning_all(void);
void test_settings_all(void);
void test_pad_engine_all(void);
void test_pressure_all(void);
void test_ui_all(void);
void test_palette_all(void);
void test_pads_all(void);
void test_twostage_all(void);
void test_midici_all(void);
void test_ci_pe_all(void);
void test_midici_initiator_all(void);

int main(void)
{
	test_harness_all();
	test_ump_codec_all();
	test_ump_tx_all();
	test_ump_stream_all();
	test_ump_rx_all();
	test_usb_desc_all();
	test_usb_prop_all();
	test_usb_data_all();
	test_pad_velocity_all();
	test_scales_all();
	test_tuning_all();
	test_settings_all();
	test_pad_engine_all();
	test_pressure_all();
	test_ui_all();
	test_palette_all();
	test_pads_all();
	test_twostage_all();
	test_midici_all();
	test_ci_pe_all();
	test_midici_initiator_all();

	printf("%d failures\n", test_failures());
	return test_failures() ? 1 : 0;
}
