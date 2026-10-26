/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * BOCO PSU Protocol Definitions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Common register definitions and constants shared across all BOCO PSU models.
 * Currently supports:
 * - BOCO BS402A17 (R2, 4kW)
 * - BOCO BS502A17 (R2, 5kW)
 */

#ifndef LIBPSU_VENDORS_BOCO_H
#define LIBPSU_VENDORS_BOCO_H

#include <libpsu/pmbus_types.h>

/*
 * BOCO Common Constants
 */

/* I2C address for all BOCO PSUs */
#define BOCO_I2C_ADDR			0x10

/* VOUT exponent used by all BOCO PSUs */
#define BOCO_VOUT_EXPONENT		-7

/* Watchdog registers (shared across all BOCO models) */
#define BOCO_WDG_ENABLE_ADDR		0x07
#define BOCO_SET_WDG_TIME_ADDR		0x08
#define BOCO_WDG_EN_STATUS_ADDR		0x09

/*
 * BOCO Firmware Version Registers
 * Block reads, return 6-byte ASCII strings.
 *
 * Sysfs attributes (raw ASCII output):
 * - app_fw_revision (0xDB): Application firmware version
 * - bl_fw_revision (0xDC): Bootloader firmware version
 *
 * NOTE: These registers do NOT support PEC (Packet Error Checking).
 * PEC must be disabled before block reads.
 */
#define BOCO_APP_FW_REVISION_ADDR	0xDB
#define BOCO_BL_FW_REVISION_ADDR	0xDC
#define BOCO_FW_VERSION_LEN		6

/*
 * BOCO Manufacturer-Specific Temperature Registers
 * These registers are read in addition to standard PMBus temperature registers.
 */
#define BOCO_LLC_TEMP_ADDR		0xE9  /* LLC (resonant converter) temperature */

/* BOCO OTP (Over Temperature Protection) Threshold Registers
 * These registers report the OTP threshold limits internally configured by the PSU.
 * The register values are formatted in Linear-11 format.
 */
#define BOCO_MFR_MAX_TEMP_1_ADDR	0xC0  /* OTP ambient temperature threshold */
#define BOCO_MFR_MAX_TEMP_2_ADDR	0xC1  /* OTP SR temperature threshold */
#define BOCO_MFR_MAX_TEMP_3_ADDR	0xC2  /* OTP PFC temperature threshold */

/* Per-input voltage registers (MFR-specific, Linear-11, mV) */
#define BOCO_READ_VIN1_ADDR		0xE0  /* Input 1 voltage */
#define BOCO_READ_VIN2_ADDR		0xE1  /* Input 2 voltage */
#define BOCO_READ_VIN3_ADDR		0xE2  /* Input 3 voltage */

/* Per-input current registers (MFR-specific, Linear-11, mA) */
#define BOCO_READ_IIN1_ADDR		0xE3  /* Input 1 current */
#define BOCO_READ_IIN2_ADDR		0xE4  /* Input 2 current */
#define BOCO_READ_IIN3_ADDR		0xE5  /* Input 3 current */

/* Per-input power registers (MFR-specific, Linear-11, mW) */
#define BOCO_READ_PIN1_ADDR		0xE6  /* Input 1 power */
#define BOCO_READ_PIN2_ADDR		0xE7  /* Input 2 power */
#define BOCO_READ_PIN3_ADDR		0xE8  /* Input 3 power */

/* VSB (standby) status registers (V1.3+, raw byte) */
#define BOCO_STATUS_VSB_VOUT_ADDR	0xEB  /* VSB output voltage status */
#define BOCO_STATUS_VSB_IOUT_ADDR	0xEC  /* VSB output current status */

#endif /* LIBPSU_VENDORS_BOCO_H */
