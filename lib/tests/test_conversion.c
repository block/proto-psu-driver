// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Unit tests for PMBus data format conversion functions.
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Test vectors derived from real PSU register values observed in production.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpsu/pmbus_conversion.h>

static int tests_run;
static int tests_passed;

#define ASSERT_EQ(expected, actual, fmt, ...)                                  \
	do {                                                                   \
		tests_run++;                                                   \
		if ((expected) == (actual)) {                                   \
			tests_passed++;                                        \
		} else {                                                       \
			fprintf(stderr, "FAIL %s:%d: " fmt                    \
				" expected=%d actual=%d\n",                    \
				__FILE__, __LINE__, ##__VA_ARGS__,             \
				(int)(expected), (int)(actual));                \
		}                                                              \
	} while (0)

/*
 * Test pmbus_extract_exponent_5bit
 */
static void test_exponent_extraction(void)
{
	/* Positive exponent: 3 = 0b00011 */
	ASSERT_EQ(3, pmbus_extract_exponent_5bit(0x03), "positive exponent 3");

	/* Zero exponent */
	ASSERT_EQ(0, pmbus_extract_exponent_5bit(0x00), "zero exponent");

	/* Negative exponent: -1 = 0b11111 */
	ASSERT_EQ(-1, pmbus_extract_exponent_5bit(0x1F), "negative exponent -1");

	/* Negative exponent: -9 = 0b10111 */
	ASSERT_EQ(-9, pmbus_extract_exponent_5bit(0x17), "negative exponent -9");

	/* Negative exponent: -7 = 0b11001 */
	ASSERT_EQ(-7, pmbus_extract_exponent_5bit(0x19), "negative exponent -7");

	/* Maximum positive: 15 = 0b01111 */
	ASSERT_EQ(15, pmbus_extract_exponent_5bit(0x0F), "max positive exponent 15");

	/* Maximum negative: -16 = 0b10000 */
	ASSERT_EQ(-16, pmbus_extract_exponent_5bit(0x10), "max negative exponent -16");
}

/*
 * Test pmbus_linear11_to_milli
 */
static void test_linear11_to_milli(void)
{
	/*
	 * 230V input voltage: exponent=-1 (0b11111=0x1F), mantissa=460 (0x1CC)
	 * Raw word: (0x1F << 11) | 0x1CC = 0xF9CC
	 * Expected: 460 * 2^-1 * 1000 = 460 * 500 = 230000 mV
	 */
	ASSERT_EQ(230000, pmbus_linear11_to_milli(0xF9CC), "230V input");

	/*
	 * 12.5A input current: exponent=-3 (0x1D), mantissa=100 (0x064)
	 * Raw word: (0x1D << 11) | 0x064 = 0xE864
	 * Expected: 100 * 2^-3 * 1000 = 100 * 125 = 12500 mA
	 */
	ASSERT_EQ(12500, pmbus_linear11_to_milli(0xE864), "12.5A current");

	/*
	 * Zero value: exponent=0, mantissa=0
	 */
	ASSERT_EQ(0, pmbus_linear11_to_milli(0x0000), "zero value");

	/*
	 * Negative temperature: -10C, exponent=0 (0x00), mantissa=-10 (0x7F6)
	 * Raw word: (0x00 << 11) | 0x7F6 = 0x07F6
	 * mantissa = 0x7F6, sign bit (0x400) is set, so sign-extend: -10
	 * Expected: -10 * 2^0 * 1000 = -10000 mC
	 */
	ASSERT_EQ(-10000, pmbus_linear11_to_milli(0x07F6), "negative temperature -10C");
}

/*
 * Test pmbus_linear11_to_milli_unsigned
 *
 * Mirrors test_linear11_to_milli but asserts that negative results are clamped
 * to 0 instead of wrapping when assigned to a u32 field.
 */
static void test_linear11_to_milli_unsigned(void)
{
	/* Non-negative values pass through unchanged. */
	ASSERT_EQ(230000, pmbus_linear11_to_milli_unsigned(0xF9CC),
		  "230V passes through");
	ASSERT_EQ(12500, pmbus_linear11_to_milli_unsigned(0xE864),
		  "12.5A passes through");
	ASSERT_EQ(0, pmbus_linear11_to_milli_unsigned(0x0000),
		  "zero passes through");

	/*
	 * Negative LINEAR11 (mantissa = -1, exponent = 0) -> -1000 signed.
	 * Raw word: (0x00 << 11) | 0x7FF = 0x07FF
	 * Clamped path must return 0u, not wrap to a huge u32.
	 */
	ASSERT_EQ(0, pmbus_linear11_to_milli_unsigned(0x07FF),
		  "mantissa=-1 clamps to 0");

	/*
	 * Same word that produces -10000 in the signed path; must clamp to 0.
	 */
	ASSERT_EQ(0, pmbus_linear11_to_milli_unsigned(0x07F6),
		  "near-zero negative clamps to 0");

	/*
	 * Overflow saturation: mantissa = 1023 (0x3FF), exponent = 15 (0x0F).
	 * Raw word: (0x0F << 11) | 0x3FF = 0x7BFF.
	 * Product: 1023 * 2^15 * 1000 = 33,521,664,000 (~33.5e9) — well above
	 * INT32_MAX. The signed helper narrows this to a negative s32; if the
	 * unsigned helper checked the sign post-narrowing it would wrongly
	 * return 0. Clamping in s64 space saturates at LIBPSU_S32_MAX.
	 */
	ASSERT_EQ((s32)LIBPSU_S32_MAX,
		  (s32)pmbus_linear11_to_milli_unsigned(0x7BFF),
		  "overflow saturates at INT32_MAX");
}

/*
 * Test pmbus_linear11_to_int
 */
static void test_linear11_to_int(void)
{
	/*
	 * 5000 RPM: exponent=3 (0x03), mantissa=625 (0x271)
	 * Raw word: (0x03 << 11) | 0x271 = 0x1A71
	 * Expected: 625 * 2^3 = 5000
	 */
	ASSERT_EQ(5000, pmbus_linear11_to_int(0x1A71), "5000 RPM");

	/* Zero */
	ASSERT_EQ(0, pmbus_linear11_to_int(0x0000), "zero RPM");
}

/*
 * Test pmbus_milli_to_linear11
 */
static void test_milli_to_linear11(void)
{
	/*
	 * Round-trip test: convert to LINEAR11 then back to milli.
	 * Small deviations are acceptable due to quantization.
	 */
	u16 encoded;
	s32 decoded;

	/* 18000 mV (18V) */
	encoded = pmbus_milli_to_linear11(18000);
	decoded = pmbus_linear11_to_milli(encoded);
	/* Allow +/- 100 mV quantization error */
	ASSERT_EQ(1, (decoded >= 17900 && decoded <= 18100), "round-trip 18V");

	/* 230000 mV (230V) */
	encoded = pmbus_milli_to_linear11(230000);
	decoded = pmbus_linear11_to_milli(encoded);
	/* Allow +/- 500 mV quantization error for large values */
	ASSERT_EQ(1, (decoded >= 229500 && decoded <= 230500), "round-trip 230V");

	/* 0 mV */
	encoded = pmbus_milli_to_linear11(0);
	decoded = pmbus_linear11_to_milli(encoded);
	ASSERT_EQ(0, decoded, "round-trip 0V");
}

/*
 * Test pmbus_mv_to_linear16 and pmbus_linear16_to_mv
 */
static void test_linear16_roundtrip(void)
{
	u16 encoded;
	u32 decoded;

	/*
	 * 18000 mV with exponent -9 (Chicony):
	 * mantissa = 18000 * 2^9 / 1000 = 18000 * 512 / 1000 = 9216
	 */
	encoded = pmbus_mv_to_linear16(18000, -9);
	ASSERT_EQ(9216, encoded, "18V with exp -9");

	decoded = pmbus_linear16_to_mv(encoded, -9);
	ASSERT_EQ(18000, decoded, "round-trip 18V exp -9");

	/*
	 * 16250 mV with exponent -9:
	 * mantissa = 16250 * 512 / 1000 = 8320
	 */
	encoded = pmbus_mv_to_linear16(16250, -9);
	ASSERT_EQ(8320, encoded, "16.25V with exp -9");

	decoded = pmbus_linear16_to_mv(encoded, -9);
	ASSERT_EQ(16250, decoded, "round-trip 16.25V exp -9");

	/*
	 * BOCO exponent -7:
	 * 18000 mV -> mantissa = 18000 * 128 / 1000 = 2304
	 */
	encoded = pmbus_mv_to_linear16(18000, -7);
	ASSERT_EQ(2304, encoded, "18V with exp -7");

	decoded = pmbus_linear16_to_mv(encoded, -7);
	ASSERT_EQ(18000, decoded, "round-trip 18V exp -7");
}

int main(void)
{
	tests_run = 0;
	tests_passed = 0;

	test_exponent_extraction();
	test_linear11_to_milli();
	test_linear11_to_milli_unsigned();
	test_linear11_to_int();
	test_milli_to_linear11();
	test_linear16_roundtrip();

	printf("%d/%d tests passed\n", tests_passed, tests_run);

	return (tests_passed == tests_run) ? 0 : 1;
}
