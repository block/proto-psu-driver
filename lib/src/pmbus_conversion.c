// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PMBus Data Format Conversion Functions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Portable conversion functions for PMBus LINEAR11 and LINEAR16 data formats.
 * These functions are pure math with no OS dependencies.
 */

#include <libpsu/pmbus_conversion.h>

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>  /* For do_div() */
/*
 * Kernel-safe 64-bit division for 32-bit ARM.
 * The kernel does not link libgcc, so plain '/' on u64 generates calls to
 * __aeabi_uldivmod / __aeabi_ldivmod which are undefined. do_div() expands
 * to the appropriate arch-specific inline implementation.
 */
#define LIBPSU_DIV_U64(dividend, divisor) do_div((dividend), (divisor))
#else
#define LIBPSU_DIV_U64(dividend, divisor) ((dividend) /= (divisor))
#endif

/**
 * pmbus_extract_exponent_5bit - Extract and sign-extend 5-bit two's complement exponent
 * @value: 5-bit exponent value (already masked and shifted if needed)
 *
 * Returns: Sign-extended 8-bit exponent
 *
 * This helper extracts a 5-bit two's complement exponent and sign-extends it to 8 bits.
 * Used for both VOUT_MODE and LINEAR11 exponent extraction.
 */
s8 pmbus_extract_exponent_5bit(u8 value)
{
	s8 exponent = (s8)(value & LINEAR11_EXPONENT_MASK);

	/* Sign extend 5-bit to 8-bit if negative */
	if (exponent & LINEAR11_EXPONENT_SIGN_BIT)
		exponent |= LINEAR11_EXPONENT_SIGN_EXT;

	return exponent;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_extract_exponent_5bit);
#endif

/*
 * Internal: LINEAR11 -> milli arithmetic in s64, no narrowing.
 * The mantissa*2^exponent*1000 product can exceed s32 range for valid encodings
 * (mantissa=1023, exponent=15 -> 33.5e9); wrappers narrow or clamp as needed.
 */
static s64 linear11_to_milli_s64(u16 data)
{
	s16 mantissa;
	s8 exponent;
	s64 result;

	/* Extract exponent (5 bits, two's complement) */
	exponent = pmbus_extract_exponent_5bit(data >> LINEAR11_EXPONENT_SHIFT);

	/* Extract mantissa (11 bits, two's complement) */
	mantissa = (s16)(data & LINEAR11_MANTISSA_MASK);
	if (mantissa & LINEAR11_MANTISSA_SIGN_BIT)
		mantissa |= LINEAR11_MANTISSA_SIGN_EXT;  /* Sign extend 11-bit to 16-bit */

	/* Calculate: mantissa * 2^exponent * 1000 (convert to milli-units) */
	if (exponent >= 0)
		result = ((s64)mantissa << exponent) * 1000;
	else
		result = ((s64)mantissa * 1000) >> (-exponent);

	return result;
}

/**
 * pmbus_linear11_to_milli - Convert LINEAR11 format to milli-units
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: Value in milli-units (mV, mA, mW, or mC for temperature)
 *          Signed to support negative temperatures.
 *
 * PMBus Linear-11 returns values in base units (V, A, W, C), so we multiply by 1000.
 */
s32 pmbus_linear11_to_milli(u16 data)
{
	return (s32)linear11_to_milli_s64(data);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_linear11_to_milli);
#endif

/**
 * pmbus_linear11_to_milli_unsigned - LINEAR11 to milli-units, clamped at [0, S32_MAX]
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: LINEAR11 milli-value clamped into [0, LIBPSU_S32_MAX], for callers
 * that store the result in u32 fields (voltage/current/power, not temperature).
 *
 * The clamp is done in s64 space before narrowing, so both negative results
 * (e.g. PIN noise dithering across zero) and large-positive results that would
 * overflow s32 (corrupted register reads, mantissa*2^exp*1000 > INT32_MAX) are
 * handled correctly without sign-flip artifacts.
 *
 * Ceiling is INT32_MAX rather than UINT32_MAX because sysfs readers round-trip
 * u32 fields through `int`/`%d`; values above INT32_MAX would re-print as
 * negative and reintroduce userspace parse errors.
 */
u32 pmbus_linear11_to_milli_unsigned(u16 data)
{
	s64 r = linear11_to_milli_s64(data);

	if (r < 0)
		return 0;
	if (r > LIBPSU_S32_MAX)
		return (u32)LIBPSU_S32_MAX;
	return (u32)r;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_linear11_to_milli_unsigned);
#endif

/**
 * pmbus_linear11_to_int - Convert LINEAR11 format to integer (no scaling)
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: Value as integer (for RPM, Hz, or other non-milli values)
 *
 * Unlike pmbus_linear11_to_milli(), this does NOT multiply by 1000.
 * Use this for fan speed (RPM) and other values that are not in milli-units.
 */
s32 pmbus_linear11_to_int(u16 data)
{
	s16 mantissa;
	s8 exponent;
	s64 result;

	/* Extract exponent (5 bits, two's complement) */
	exponent = pmbus_extract_exponent_5bit(data >> LINEAR11_EXPONENT_SHIFT);

	/* Extract mantissa (11 bits, two's complement) */
	mantissa = (s16)(data & LINEAR11_MANTISSA_MASK);
	if (mantissa & LINEAR11_MANTISSA_SIGN_BIT)
		mantissa |= LINEAR11_MANTISSA_SIGN_EXT;  /* Sign extend 11-bit to 16-bit */

	/* Calculate: mantissa * 2^exponent (NO *1000 for non-milli values) */
	if (exponent >= 0)
		result = (s64)mantissa << exponent;
	else
		result = (s64)mantissa >> (-exponent);

	return (s32)result;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_linear11_to_int);
#endif

/**
 * pmbus_milli_to_linear11 - Convert millivolts/milliamps/milliwatts to LINEAR11 format
 * @milli: Value in milli-units (mV, mA, or mW)
 *
 * Returns: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * This is the inverse of pmbus_linear11_to_milli().
 * We need to find an exponent and mantissa such that:
 * milli = mantissa * 2^exponent * 1000
 *
 * Algorithm: Choose the smallest exponent that keeps mantissa in valid range (-1024 to +1023)
 */
u16 pmbus_milli_to_linear11(u32 milli)
{
	s16 mantissa;
	s8 exponent = 0;
	u64 value = milli;  /* Start with value in milli-units (unsigned) */

	/* Find appropriate exponent to keep mantissa in 11-bit range (-1024 to +1023) */
	/* Start with exponent 0: mantissa = milli / 1000 */
	while (value >= 1024000) {
		/* Value too large, need larger exponent */
		exponent++;
		if (exponent > 15)  /* Max positive exponent for 5-bit */
			break;
		value = (u64)milli >> exponent;  /* Divide by 2^exponent */
	}

	/* Calculate final mantissa: milli / (1000 * 2^exponent) */
	if (exponent >= 0) {
		LIBPSU_DIV_U64(value, 1000);
		mantissa = (s16)value;
	} else {
		u64 temp = value * (1 << (-exponent));
		LIBPSU_DIV_U64(temp, 1000);
		mantissa = (s16)temp;
	}

	/* Clamp mantissa to 11-bit two's complement range */
	if (mantissa > 1023)
		mantissa = 1023;
	else if (mantissa < -1024)
		mantissa = -1024;

	/* Pack into LINEAR11 format: [exponent:5][mantissa:11] */
	return ((u16)(exponent & 0x1F) << 11) | ((u16)mantissa & 0x07FF);
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_milli_to_linear11);
#endif

/**
 * pmbus_mv_to_linear16 - Convert millivolts to LINEAR16 format
 * @mv: Value in millivolts
 * @exponent: Exponent value (typically read from VOUT_MODE)
 *
 * Returns: 16-bit LINEAR16 value
 *
 * LINEAR16 format stores voltage in volts, not millivolts.
 * Formula: mantissa = (voltage_in_volts) * 2^(abs(exponent))
 *
 * For exponent -9: 18000mV -> 18V -> 18 * 2^9 = 18 * 512 = 9216
 */
u16 pmbus_mv_to_linear16(u32 mv, s8 exponent)
{
	u64 temp;
	u32 mantissa;

	/* Convert mV to V, then apply exponent */
	if (exponent >= 0) {
		/* Positive exponent: (mv / 1000) * 2^exponent */
		temp = (u64)mv << exponent;
		LIBPSU_DIV_U64(temp, 1000);
		mantissa = (u32)temp;
	} else {
		/* Negative exponent: (mv / 1000) * 2^abs(exponent) */
		/* Which is: mv * 2^abs(exponent) / 1000 */
		temp = (u64)mv << (-exponent);
		LIBPSU_DIV_U64(temp, 1000);
		mantissa = (u32)temp;
	}

	/* Clamp to 16-bit range */
	if (mantissa > 0xFFFF)
		mantissa = 0xFFFF;

	return (u16)mantissa;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_mv_to_linear16);
#endif

/**
 * pmbus_linear16_to_mv - Convert LINEAR16 format to millivolts
 * @data: 16-bit LINEAR16 mantissa
 * @exponent: Exponent value (typically read from VOUT_MODE)
 *
 * Returns: Value in millivolts
 *
 * LINEAR16 format stores voltage in volts. To avoid precision loss,
 * we multiply by 1000 BEFORE applying the exponent division.
 * Formula: millivolts = (mantissa * 1000) * 2^exponent
 *
 * For exponent -9: mantissa=8320 -> (8320*1000) / 512 = 16250 mV
 */
u32 pmbus_linear16_to_mv(u16 data, s8 exponent)
{
	s64 result;

	/* Multiply by 1000 first to preserve millivolt precision */
	result = (s64)data * 1000;

	/* Apply exponent */
	if (exponent >= 0)
		result = result << exponent;
	else
		result = result >> (-exponent);

	return (u32)result;
}
#ifdef __KERNEL__
EXPORT_SYMBOL_GPL(pmbus_linear16_to_mv);
#endif
