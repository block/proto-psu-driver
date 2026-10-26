/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PMBus Protocol Definitions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * PMBus command codes (register addresses), status flags, and data format
 * constants. These definitions are derived from the PMBus specification
 * and are portable across Linux kernel modules and bare-metal firmware.
 *
 * See PMBus-Specification-Rev-1-4-Part-II-20220112.pdf for more information.
 */

#ifndef LIBPSU_PMBUS_PROTOCOL_H
#define LIBPSU_PMBUS_PROTOCOL_H

#include <libpsu/pmbus_types.h>

/*
 * PMBus Command Codes (Register Addresses)
 * NOTE: Only defining the commands that are used by the PSU driver.
 */
#define PMBUS_OPERATION_ADDR			0x01
#define PMBUS_CLEAR_FAULTS_ADDR			0x03
#define PMBUS_CAPABILITY_ADDR			0x19

#define PMBUS_VOUT_MODE_ADDR			0x20
#define PMBUS_VOUT_COMMAND_ADDR			0x21

#define PMBUS_FAN_CONFIG_1_2_ADDR		0x3A
#define PMBUS_FAN_COMMAND_1_ADDR		0x3B
#define PMBUS_FAN_COMMAND_2_ADDR		0x3C
#define PMBUS_FAN_CONFIG_3_4_ADDR		0x3D
#define PMBUS_FAN_COMMAND_3_ADDR		0x3E
#define PMBUS_FAN_COMMAND_4_ADDR		0x3F

#define PMBUS_STATUS_BYTE_ADDR			0x78
#define PMBUS_STATUS_WORD_ADDR			0x79
#define PMBUS_STATUS_VOUT_ADDR			0x7A
#define PMBUS_STATUS_IOUT_ADDR			0x7B
#define PMBUS_STATUS_INPUT_ADDR			0x7C
#define PMBUS_STATUS_TEMPERATURE_ADDR		0x7D
#define PMBUS_STATUS_CML_ADDR			0x7E
#define PMBUS_STATUS_OTHER_ADDR			0x7F
#define PMBUS_STATUS_MFR_SPECIFIC_ADDR		0x80
#define PMBUS_STATUS_FANS_1_2_ADDR		0x81
#define PMBUS_STATUS_FANS_3_4_ADDR		0x82

#define PMBUS_READ_VIN_ADDR			0x88
#define PMBUS_READ_IIN_ADDR			0x89
#define PMBUS_READ_VOUT_ADDR			0x8B
#define PMBUS_READ_IOUT_ADDR			0x8C
#define PMBUS_READ_TEMPERATURE_1_ADDR		0x8D
#define PMBUS_READ_TEMPERATURE_2_ADDR		0x8E
#define PMBUS_READ_TEMPERATURE_3_ADDR		0x8F
#define PMBUS_READ_FAN_SPEED_1_ADDR		0x90
#define PMBUS_READ_FAN_SPEED_2_ADDR		0x91
#define PMBUS_READ_FAN_SPEED_3_ADDR		0x92
#define PMBUS_READ_FAN_SPEED_4_ADDR		0x93
#define PMBUS_READ_POUT_ADDR			0x96
#define PMBUS_READ_PIN_ADDR			0x97

#define PMBUS_REVISION_ADDR			0x98
#define PMBUS_MFR_ID_ADDR			0x99
#define PMBUS_MFR_MODEL_ADDR			0x9A
#define PMBUS_MFR_REVISION_ADDR			0x9B
#define PMBUS_MFR_LOCATION_ADDR			0x9C
#define PMBUS_MFR_DATE_ADDR			0x9D
#define PMBUS_MFR_SERIAL_ADDR			0x9E

/*
 * Manufacturer-Specific Limit Commands (0xA0-0xA7)
 * These commands read the hardware capability limits from the PSU.
 * Supported by Chicony, BOCO, and most PMBus-compliant PSUs.
 * All values returned in LINEAR11 format except VOUT limits (LINEAR16).
 */
#define PMBUS_MFR_VIN_MIN_ADDR			0xA0
#define PMBUS_MFR_VIN_MAX_ADDR			0xA1
#define PMBUS_MFR_IIN_MAX_ADDR			0xA2
#define PMBUS_MFR_PIN_MAX_ADDR			0xA3
#define PMBUS_MFR_VOUT_MIN_ADDR			0xA4
#define PMBUS_MFR_VOUT_MAX_ADDR			0xA5
#define PMBUS_MFR_IOUT_MAX_ADDR		0xA6
#define PMBUS_MFR_POUT_MAX_ADDR		0xA7

/*
 * Common FW Update Registers
 */
#define PMBUS_MFR_FWUPLOAD_MODE_ADDR		0xD6
#define PMBUS_MFR_FWUPLOAD_STATUS_ADDR		0xD8

/* FW Update Mode Values */
#define PMBUS_FWUPLOAD_MODE_NORMAL_VAL		0x00
#define PMBUS_FWUPLOAD_MODE_BOOTLOADER_VAL	0x01

/*
 * PMBus Operation Command Values
 */
#define PMBUS_OPERATION_ON_VAL		0x80
#define PMBUS_OPERATION_OFF_VAL		0x00

/*
 * PMBus Status Word Flags (16-bit)
 * NOTE: These are BIT flags for checking STATUS_WORD register value.
 * Use _BIT suffix to avoid collision with register address defines above.
 */
#define PMBUS_STATUS_NONE_BIT			PSU_BIT(0)
#define PMBUS_STATUS_CML_BIT			PSU_BIT(1)
#define PMBUS_STATUS_TEMPERATURE_BIT		PSU_BIT(2)
#define PMBUS_STATUS_VIN_UV_FAULT_BIT		PSU_BIT(3)
#define PMBUS_STATUS_IOUT_OC_FAULT_BIT		PSU_BIT(4)
#define PMBUS_STATUS_VOUT_OV_FAULT_BIT		PSU_BIT(5)
#define PMBUS_STATUS_OFF_BIT			PSU_BIT(6)
#define PMBUS_STATUS_BUSY_BIT			PSU_BIT(7)
#define PMBUS_STATUS_UNKNOWN_BIT		PSU_BIT(8)
#define PMBUS_STATUS_OTHER_BIT			PSU_BIT(9)
#define PMBUS_STATUS_FANS_BIT			PSU_BIT(10)
#define PMBUS_STATUS_POWER_GOOD_N_BIT		PSU_BIT(11)
#define PMBUS_STATUS_MFR_SPECIFIC_BIT		PSU_BIT(12)
#define PMBUS_STATUS_INPUT_BIT			PSU_BIT(13)
#define PMBUS_STATUS_IOUT_POUT_BIT		PSU_BIT(14)
#define PMBUS_STATUS_VOUT_BIT			PSU_BIT(15)

/*
 * PMBus Status VOUT Flags (8-bit)
 */
#define PMBUS_VOUT_TRACKING_BIT			PSU_BIT(0)
#define PMBUS_VOUT_TOFF_MAX_WARNING_BIT		PSU_BIT(1)
#define PMBUS_VOUT_TOFF_MAX_FAULT_BIT		PSU_BIT(2)
#define PMBUS_VOUT_MAX_MIN_WARNING_BIT		PSU_BIT(3)
#define PMBUS_VOUT_UV_FAULT_BIT			PSU_BIT(4)
#define PMBUS_VOUT_UV_WARNING_BIT		PSU_BIT(5)
#define PMBUS_VOUT_OV_WARNING_BIT		PSU_BIT(6)
#define PMBUS_VOUT_OV_FAULT_BIT			PSU_BIT(7)

/*
 * PMBus Status IOUT Flags (8-bit)
 */
#define PMBUS_IOUT_POUT_OP_WARNING_BIT		PSU_BIT(0)
#define PMBUS_IOUT_POUT_OP_FAULT_BIT		PSU_BIT(1)
#define PMBUS_IOUT_IN_PWR_LIMITING_BIT		PSU_BIT(2)
#define PMBUS_IOUT_I_SHARE_FAULT_BIT		PSU_BIT(3)
#define PMBUS_IOUT_UC_FAULT_BIT			PSU_BIT(4)
#define PMBUS_IOUT_OC_WARNING_BIT		PSU_BIT(5)
#define PMBUS_IOUT_OC_LV_FAULT_BIT		PSU_BIT(6)
#define PMBUS_IOUT_OC_FAULT_BIT			PSU_BIT(7)

/*
 * PMBus Status INPUT Flags (8-bit)
 */
#define PMBUS_INPUT_PIN_OP_WARNING_BIT		PSU_BIT(0)
#define PMBUS_INPUT_IIN_OC_WARNING_BIT		PSU_BIT(1)
#define PMBUS_INPUT_IIN_OC_FAULT_BIT		PSU_BIT(2)
#define PMBUS_INPUT_LOW_VIN_OFF_BIT		PSU_BIT(3)
#define PMBUS_INPUT_VIN_UV_FAULT_BIT		PSU_BIT(4)
#define PMBUS_INPUT_VIN_UV_WARNING_BIT		PSU_BIT(5)
#define PMBUS_INPUT_VIN_OV_WARNING_BIT		PSU_BIT(6)
#define PMBUS_INPUT_VIN_OV_FAULT_BIT		PSU_BIT(7)

/*
 * PMBus Status TEMPERATURE Flags (8-bit)
 */
#define PMBUS_TEMP_UT_FAULT_BIT			PSU_BIT(4)
#define PMBUS_TEMP_UT_WARNING_BIT		PSU_BIT(5)
#define PMBUS_TEMP_OT_WARNING_BIT		PSU_BIT(6)
#define PMBUS_TEMP_OT_FAULT_BIT			PSU_BIT(7)

/*
 * PMBus Status CML Flags (8-bit)
 */
#define PMBUS_CML_OTHER_BIT			PSU_BIT(0)
#define PMBUS_CML_OTHER_COMM_FAULT_BIT		PSU_BIT(1)
#define PMBUS_CML_MCU_FAULT_BIT			PSU_BIT(3)
#define PMBUS_CML_MEM_FAULT_BIT			PSU_BIT(4)
#define PMBUS_CML_PEC_FAULT_BIT			PSU_BIT(5)
#define PMBUS_CML_INVALID_DATA_BIT		PSU_BIT(6)
#define PMBUS_CML_INVALID_COMMAND_BIT		PSU_BIT(7)

/*
 * PMBus Status OTHER Flags (8-bit)
 */
#define PMBUS_OTHER_SMBALERT_N_BIT		PSU_BIT(0)
#define PMBUS_OTHER_OUT_OR_FAULT_BIT		PSU_BIT(1)
#define PMBUS_OTHER_IN_B_OR_FAULT_BIT		PSU_BIT(2)
#define PMBUS_OTHER_IN_A_OR_FAULT_BIT		PSU_BIT(3)
#define PMBUS_OTHER_IN_B_CB_FAULT_BIT		PSU_BIT(4)
#define PMBUS_OTHER_IN_A_CB_FAULT_BIT		PSU_BIT(5)

/*
 * PMBus Status FANS 1-2 Flags (8-bit)
 */
#define PMBUS_FANS_AIRFLOW_WARNING_BIT		PSU_BIT(0)
#define PMBUS_FANS_AIRFLOW_FAULT_BIT		PSU_BIT(1)
#define PMBUS_FANS_FAN2_SPD_OVERRIDE_BIT	PSU_BIT(2)
#define PMBUS_FANS_FAN1_SPD_OVERRIDE_BIT	PSU_BIT(3)
#define PMBUS_FANS_FAN2_WARNING_BIT		PSU_BIT(4)
#define PMBUS_FANS_FAN1_WARNING_BIT		PSU_BIT(5)
#define PMBUS_FANS_FAN2_FAULT_BIT		PSU_BIT(6)
#define PMBUS_FANS_FAN1_FAULT_BIT		PSU_BIT(7)

/*
 * PMBus Status FANS 3-4 Flags (8-bit)
 */
#define PMBUS_FANS_FAN4_SPD_OVERRIDE_BIT	PSU_BIT(2)
#define PMBUS_FANS_FAN3_SPD_OVERRIDE_BIT	PSU_BIT(3)
#define PMBUS_FANS_FAN4_WARNING_BIT		PSU_BIT(4)
#define PMBUS_FANS_FAN3_WARNING_BIT		PSU_BIT(5)
#define PMBUS_FANS_FAN4_FAULT_BIT		PSU_BIT(6)
#define PMBUS_FANS_FAN3_FAULT_BIT		PSU_BIT(7)

/*
 * PMBus Capability Flags (8-bit)
 */
#define PMBUS_CAP_AVSBUS_SUPPORT_BIT		PSU_BIT(2)
#define PMBUS_CAP_NUMERIC_FORMAT_BIT		PSU_BIT(3)
#define PMBUS_CAP_SMBALERT_N_BIT		PSU_BIT(4)
#define PMBUS_CAP_PEC_SUPPORT_BIT		PSU_BIT(7)

/*
 * NOTE on PEC (Packet Error Checking):
 *
 * PEC support varies widely across PSU models and even across commands
 * on the same PSU. Some PSUs require PEC for certain commands
 * (e.g., voltage writes) but forbid it for others (e.g., manufacturer info reads).
 *
 * PEC handling is implemented per-PSU in device-specific code
 * using the psu_ops->use_pec_for_command() callback. This allows
 * fine-grained control on a per-transaction basis.
 */

/*
 * Linear11 Data Format Conversion Helpers
 *
 * PMBus LINEAR11 format packs two's complement signed values into 16 bits:
 * - Exponent: 5 bits (bits 15-11) - range -16 to +15
 * - Mantissa: 11 bits (bits 10-0) - range -1024 to +1023
 *
 * Formula: value = mantissa * 2^exponent
 */
#define LINEAR11_EXPONENT_MASK		0x1F    /* Extract bits 0-4 after right-shift by 11 */
#define LINEAR11_MANTISSA_MASK		0x07FF  /* Extract bits 0-10 directly */
#define LINEAR11_EXPONENT_SHIFT		11      /* Shift to move exponent to bits 0-4 */

/*
 * Sign Extension for LINEAR11 Two's Complement Values
 *
 * When we extract 5-bit or 11-bit signed values into larger variables (s8, s16),
 * we need to sign-extend to preserve the negative value correctly.
 *
 * Example for 5-bit exponent:
 * - Binary 11111 (5-bit) = -1 in two's complement
 * - After masking to 8-bit: 00011111 = +31 (WRONG!)
 * - After sign extension: 11111111 = -1 (CORRECT!)
 *
 * Method: If sign bit is set, OR with extension mask to fill upper bits with 1s.
 */
#define LINEAR11_EXPONENT_SIGN_BIT	0x10    /* Test bit 4 of 5-bit value (0b10000) */
#define LINEAR11_EXPONENT_SIGN_EXT	0xE0    /* Set bits 5-7 to extend negative (0b11100000) */
#define LINEAR11_MANTISSA_SIGN_BIT	0x400   /* Test bit 10 of 11-bit value (0b10000000000) */
#define LINEAR11_MANTISSA_SIGN_EXT	0xF800  /* Set bits 11-15 to extend negative (0b1111100000000000) */

/*
 * Linear16 Data Format Conversion Helpers
 *
 * LINEAR16 format uses:
 * - 16-bit mantissa (bits 15-0)
 * - Separate exponent from VOUT_MODE register
 */
#define LINEAR16_MANTISSA_MASK		0xFFFF

/*
 * PSU-Specific Constants
 */
#define PSU_EN_ASSERT_HOLD_MS			50	/* GPIO enable/disable settling time */
#define PSU_UPDATE_INTERVAL_MS			5000	/* Full measurement update interval */
#define PSU_STATUS_CHECK_INTERVAL_MS		250	/* Fast status word check interval */
#define MINIMUM_VOLTAGE_CHANGE_DELTA_MV		55	/* Minimum voltage change delta for Chicony PSU firmware bug workaround */

/*
 * Maximum string lengths for device info
 */
#define PMBUS_NAME_SIZE		32
#define PMBUS_STRING_SIZE	64

/*
 * Default I2C Retry Policy Constants
 *
 * These are exported as defaults; each consumer may choose different values.
 */
#define PSU_I2C_MAX_RETRIES_DEFAULT		3	/* Max I2C retries for transient errors */
#define PSU_I2C_RETRY_DELAY_MS_DEFAULT		30	/* PMBus retry delay (ms) */

#endif /* LIBPSU_PMBUS_PROTOCOL_H */
