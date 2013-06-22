#include <vd2/Kasumi/pixel.h>
#include "test.h"

DEFINE_TEST(Pixel) {
	TEST_ASSERT(VDConvertYCbCrToRGB(0x00, 0x80, 0x80, false, false) == 0x000000);
	TEST_ASSERT(VDConvertYCbCrToRGB(0x10, 0x80, 0x80, false, false) == 0x000000);
	TEST_ASSERT(VDConvertYCbCrToRGB(0x80, 0x80, 0x80, false, false) == 0x828282);
	TEST_ASSERT(VDConvertYCbCrToRGB(0xEB, 0x80, 0x80, false, false) == 0xFFFFFF);
	TEST_ASSERT(VDConvertYCbCrToRGB(0xFF, 0x80, 0x80, false, false) == 0xFFFFFF);

	TEST_ASSERT(VDConvertRGBToYCbCr(0x00, 0x00, 0x00, false, false) == 0x801080);
	TEST_ASSERT(VDConvertRGBToYCbCr(0x80, 0x80, 0x80, false, false) == 0x807E80);
	TEST_ASSERT(VDConvertRGBToYCbCr(0xFF, 0xFF, 0xFF, false, false) == 0x80EB80);

	return 0;
}

