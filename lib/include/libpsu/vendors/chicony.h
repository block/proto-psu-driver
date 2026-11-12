/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * Chicony PSU Protocol Definitions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Register definitions and constants shared across all Chicony PSU models.
 * Currently supports:
 * - Chicony S24 / S4K0E001P (R2, 4kW)
 */

#ifndef LIBPSU_VENDORS_CHICONY_H
#define LIBPSU_VENDORS_CHICONY_H

#include <libpsu/pmbus_types.h>

/*
 * Chicony Common Constants
 */

/* OPERATION command values (Chicony-specific, non-standard) */
#define CHICONY_OPERATION_ON_VAL	0x80  /* Standard PMBus ON value */
#define CHICONY_OPERATION_OFF_VAL	0x40  /* NOT 0x00! Chicony uses 0x40 for OFF */

/* VOUT exponent used by Chicony S24 PSUs */
#define CHICONY_VOUT_EXPONENT		-9

/*
 * Chicony Custom Limit Configuration Registers
 * These are manufacturer-specific registers for setting protection limits.
 */
#define CHICONY_OTP_LIMIT_ADDR		0xBC  /* Over-Temperature Protection limit */
#define CHICONY_OCP_LIMIT_ADDR		0xBD  /* Over-Current Protection limit */
#define CHICONY_OVP_LIMIT_ADDR		0xBE  /* Over-Voltage Protection limit */
#define CHICONY_UV_LIMIT_ADDR		0xB6  /* Under-Voltage limit */

/*
 * Chicony voltage control quirks
 *
 * Chicony firmware has a bug where small voltage changes (<55mV) are ignored.
 * This value represents the minimum voltage change that will actually be applied.
 * Source: Production Rust code chicony_s24/driver.rs
 */
#define CHICONY_MIN_VOLTAGE_CHANGE_MV	55

/*
 * Chicony STATUS_MFR_SPECIFIC (0x80) bit definitions
 * Reports power module status for dual-module PSU architecture.
 * Note: Only reported by Module 1 (Address 0x10)
 *
 * Fault interpretation:
 * - Only PowerNoGood set: LLC section issue
 * - PFC_NOK + PowerNoGood (no Vin_UV): PFC section fault
 * - All three set: No input voltage to module
 */
#define CHICONY_MFR_MODULE1_VIN_UV_FAULT_BIT	PSU_BIT(0)
#define CHICONY_MFR_MODULE1_PFC_NOK_BIT		PSU_BIT(1)
#define CHICONY_MFR_MODULE1_POWER_NO_GOOD_BIT	PSU_BIT(2)
#define CHICONY_MFR_MODULE2_VIN_UV_FAULT_BIT	PSU_BIT(3)
#define CHICONY_MFR_MODULE2_PFC_NOK_BIT		PSU_BIT(4)
#define CHICONY_MFR_MODULE2_POWER_NO_GOOD_BIT	PSU_BIT(5)

/*
 * Chicony Firmware Version Register
 * Block read, returns 7 bytes of firmware information.
 *
 * Sysfs attributes:
 * - fw_revision_m1: Module 1 firmware, 7 bytes output as hex string
 * - fw_revision_m2: Module 2 firmware, 7 bytes output as hex string
 *
 * Parsing of individual version components is handled in userspace.
 */
#define CHICONY_MFR_FW_REVISION_ADDR	0xD9
#define CHICONY_FW_REVISION_LEN		7

/*
 * Chicony I2C Module Addresses
 * Chicony PSUs have two modules at separate I2C addresses.
 * The primary module (0x08) can read both modules' firmware.
 */
#define CHICONY_PRIMARY_I2C_ADDR	0x08
#define CHICONY_SECONDARY_I2C_ADDR	0x09

/*
 * Chicony S24 (S4K0E001P) Hardware Limits
 * R24000W 4000W PSU hardcoded limits from specification.
 * Used as fallback if MFR_* limit registers are not available.
 */
#define CHICONY_S24_VOUT_MIN_MV		15000   /* 15V minimum output voltage */
#define CHICONY_S24_VOUT_MAX_MV		19000   /* 19V maximum output voltage */
#define CHICONY_S24_IOUT_MAX_MA		235250  /* 235.25A maximum output current */
#define CHICONY_S24_POUT_MAX_MW		4000000 /* 4000W maximum output power */
#define CHICONY_S24_VIN_MIN_MV		208000  /* 208V minimum input voltage */
#define CHICONY_S24_VIN_MAX_MV		240000  /* 240V maximum input voltage */
#define CHICONY_S24_IIN_MAX_MA		12000   /* 12A maximum input current */
#define CHICONY_S24_PIN_MAX_MW		4000000 /* 4000W maximum input power */

#endif /* LIBPSU_VENDORS_CHICONY_H */
