#include "test.h"
#include "platform/pads.h"

/* Library press and release levels (10-bit ADC_Max) for a given
 * pad_setcalibration threshold, modelled from the cx_pad.o disassembly. */
static void test_lib_levels(void)
{
	CHECK_EQ(pad_lib_trigger_level(256), 358);
	CHECK_EQ(pad_lib_trigger_level(20), 113);
	CHECK_EQ(pad_lib_trigger_level(0), 92);
	CHECK_EQ(pad_lib_release_level(20), 103);
	CHECK_EQ(pad_lib_release_level(0), 96);
	CHECK(pad_lib_release_level(PAD_LIB_THRESHOLD) < pad_lib_trigger_level(PAD_LIB_THRESHOLD));
	CHECK(pad_lib_release_level(0) >= pad_lib_trigger_level(0));   /* why floor is 20 */
}

void test_pads_all(void)
{
	RUN(test_lib_levels);
}
