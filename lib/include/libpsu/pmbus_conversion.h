/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PMBus Data Format Conversion Functions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Portable conversion functions for PMBus LINEAR11 and LINEAR16 data formats.
 * These functions are pure math with no OS dependencies.
 */

#ifndef LIBPSU_PMBUS_CONVERSION_H
#define LIBPSU_PMBUS_CONVERSION_H

#include <libpsu/pmbus_types.h>
#include <libpsu/pmbus_protocol.h>

/**
 * pmbus_extract_exponent_5bit - Extract and sign-extend 5-bit two's complement exponent
 * @value: 5-bit exponent value (already masked and shifted if needed)
 *
 * Returns: Sign-extended 8-bit exponent
 *
 * This helper extracts a 5-bit two's complement exponent and sign-extends it to 8 bits.
 * Used for both VOUT_MODE and LINEAR11 exponent extraction.
 */
s8 pmbus_extract_exponent_5bit(u8 value);

/**
 * pmbus_linear11_to_milli - Convert LINEAR11 format to milli-units
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: Value in milli-units (mV, mA, mW, or mC for temperature)
 *          Signed to support negative temperatures.
 */
s32 pmbus_linear11_to_milli(u16 data);

/**
 * pmbus_linear11_to_milli_unsigned - LINEAR11 to milli-units, clamped at 0
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: pmbus_linear11_to_milli(data) clamped to [0, INT32_MAX], for callers
 * that store the result in u32 fields (voltage/current/power, not temperature).
 * Negative LINEAR11 results - which occur as physically-meaningless near-zero
 * noise on power readings - are mapped to 0 instead of wrapping to a huge u32.
 */
u32 pmbus_linear11_to_milli_unsigned(u16 data);

/**
 * pmbus_linear11_to_int - Convert LINEAR11 format to integer (no scaling)
 * @data: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 *
 * Returns: Value as integer (for RPM, Hz, or other non-milli values)
 *
 * Unlike pmbus_linear11_to_milli(), this does NOT multiply by 1000.
 * Use this for fan speed (RPM) and other values that are not in milli-units.
 */
s32 pmbus_linear11_to_int(u16 data);

/**
 * pmbus_milli_to_linear11 - Convert millivolts/milliamps/milliwatts to LINEAR11 format
 * @milli: Value in milli-units (mV, mA, or mW)
 *
 * Returns: 16-bit LINEAR11 value (5-bit exponent, 11-bit mantissa)
 */
u16 pmbus_milli_to_linear11(u32 milli);

/**
 * pmbus_mv_to_linear16 - Convert millivolts to LINEAR16 format
 * @mv: Value in millivolts
 * @exponent: Exponent value (typically read from VOUT_MODE)
 *
 * Returns: 16-bit LINEAR16 value
 */
u16 pmbus_mv_to_linear16(u32 mv, s8 exponent);

/**
 * pmbus_linear16_to_mv - Convert LINEAR16 format to millivolts
 * @data: 16-bit LINEAR16 mantissa
 * @exponent: Exponent value (typically read from VOUT_MODE)
 *
 * Returns: Value in millivolts
 */
u32 pmbus_linear16_to_mv(u16 data, s8 exponent);

#endif /* LIBPSU_PMBUS_CONVERSION_H */
