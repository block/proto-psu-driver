// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Driver - PMBus Protocol Implementation
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This file implements the PMBus protocol operations for reading measurements,
 * controlling PSU output, monitoring status, and reading device information.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "psu_pmbus.h"

/* External debug flag from main driver */
extern bool debug;

/* Fault severity levels for table-driven fault handling */
enum psu_fault_severity {
	PSU_FAULT_CRITICAL,  /* psu_err, increment fault_count */
	PSU_FAULT_WARNING,   /* psu_warn, increment fault_count */
	PSU_FAULT_INFO,      /* psu_dbg only, no fault_count */
};

/* Fault descriptor for table-driven status checking */
struct psu_fault_desc {
	u16 status_bit;
	enum psu_fault_severity severity;
	const char *fault_msg;
	const char *clear_msg;
};

/* Status word fault descriptors */
static const struct psu_fault_desc fault_table[] = {
	{ PMBUS_STATUS_VOUT_OV_FAULT_BIT, PSU_FAULT_CRITICAL,
	  "Output overvoltage fault", "Output overvoltage fault cleared" },
	{ PMBUS_STATUS_IOUT_OC_FAULT_BIT, PSU_FAULT_CRITICAL,
	  "Output overcurrent fault", "Output overcurrent fault cleared" },
	{ PMBUS_STATUS_VIN_UV_FAULT_BIT, PSU_FAULT_CRITICAL,
	  "Input undervoltage fault", "Input undervoltage fault cleared" },
	{ PMBUS_STATUS_TEMPERATURE_BIT, PSU_FAULT_WARNING,
	  "Temperature fault/warning", "Temperature fault cleared" },
	{ PMBUS_STATUS_CML_BIT, PSU_FAULT_WARNING,
	  "Communication fault", "Communication fault cleared" },
	{ PMBUS_STATUS_OTHER_BIT, PSU_FAULT_WARNING,
	  "Other fault detected", "Other fault cleared" },
	{ PMBUS_STATUS_UNKNOWN_BIT, PSU_FAULT_WARNING,
	  "Unknown fault detected", "Unknown fault cleared" },
	{ PMBUS_STATUS_POWER_GOOD_N_BIT, PSU_FAULT_WARNING,
	  "Power good negated", "Power good asserted" },
	{ PMBUS_STATUS_INPUT_BIT, PSU_FAULT_WARNING,
	  "Input fault detected", "Input fault cleared" },
	{ PMBUS_STATUS_IOUT_POUT_BIT, PSU_FAULT_WARNING,
	  "IOUT/POUT fault detected", "IOUT/POUT fault cleared" },
	{ PMBUS_STATUS_VOUT_BIT, PSU_FAULT_WARNING,
	  "VOUT fault detected", "VOUT fault cleared" },
	{ PMBUS_STATUS_FANS_BIT, PSU_FAULT_WARNING,
	  "Fan fault detected", "Fan fault cleared" },
	{ PMBUS_STATUS_OFF_BIT, PSU_FAULT_INFO,
	  "PSU is off", "PSU is on" },
	{ PMBUS_STATUS_BUSY_BIT, PSU_FAULT_INFO,
	  "PSU is busy", "PSU is no longer busy" },
};

/*
 * NOTE: PMBus data format conversion functions (Linear11/Linear16) have been
 * moved to libpsu/src/pmbus_conversion.c for portability across kernel and
 * bare-metal consumers. They are included in the module build via Makefile.
 */

/*
 * =============================================================================
 * Device Information Functions
 * =============================================================================
 */

/**
 * psu_read_manufacturer_info - Read PSU manufacturer information
 * @client: I2C client device
 * @info: Pointer to device info structure
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_read_manufacturer_info(struct i2c_client *client,
			       struct psu_device_info *info)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u8 buf[PMBUS_STRING_SIZE];
	int ret, len;

	/* Read MFR_ID */
	ret = psu_i2c_read_block(client, PMBUS_MFR_ID_ADDR, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	len = min_t(int, buf[0], PMBUS_NAME_SIZE - 1);
	memcpy(info->vendor, &buf[1], len);
	info->vendor[len] = '\0';
	strim(info->vendor);

	/* Read MFR_MODEL */
	ret = psu_i2c_read_block(client, PMBUS_MFR_MODEL_ADDR, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	len = min_t(int, buf[0], PMBUS_NAME_SIZE - 1);
	memcpy(info->model, &buf[1], len);
	info->model[len] = '\0';
	strim(info->model);

	/* Read MFR_SERIAL */
	ret = psu_i2c_read_block(client, PMBUS_MFR_SERIAL_ADDR, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	len = min_t(int, buf[0], PMBUS_NAME_SIZE - 1);
	memcpy(info->serial, &buf[1], len);
	info->serial[len] = '\0';
	strim(info->serial);

	/* Read MFR_REVISION */
	ret = psu_i2c_read_block(client, PMBUS_MFR_REVISION_ADDR, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	len = min_t(int, buf[0], PMBUS_NAME_SIZE - 1);
	memcpy(info->revision, &buf[1], len);
	info->revision[len] = '\0';
	strim(info->revision);

	/* Detect PSU type from model */
	info->type = psu_detect_type(info->model);

	psu_info(data, "PSU: %s %s (SN: %s, Rev: %s)\n",
		 info->vendor, info->model, info->serial, info->revision);

	return 0;
}
EXPORT_SYMBOL_GPL(psu_read_manufacturer_info);

/**
 * psu_read_vout_mode - Read VOUT_MODE register to get exponent
 * @client: I2C client device
 * @exponent: Pointer to store exponent value
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_read_vout_mode(struct i2c_client *client, s8 *exponent)
{
	u8 mode;
	int ret;

	ret = psu_i2c_read_byte(client, PMBUS_VOUT_MODE_ADDR, &mode);
	if (ret < 0)
		return ret;

	/* Extract exponent (bits 0-4, two's complement) */
	*exponent = pmbus_extract_exponent_5bit(mode);
	return 0;
}
EXPORT_SYMBOL_GPL(psu_read_vout_mode);

/*
 * =============================================================================
 * Control Functions
 * =============================================================================
 */

/**
 * psu_clear_faults - Clear all PSU faults via PMBus command and GPIO power cycle
 * @client: I2C client device
 *
 * Clears faults via PMBus CLEAR_FAULTS command, then performs a GPIO power
 * cycle (disable -> enable) for a complete hardware reset of fault state.
 * The GPIO is restored to its original state after the power cycle.
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_clear_faults(struct i2c_client *client)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;
	int original_gpio_state;

	/* Power cycle PSU via GPIO for complete fault reset */
	if (data && data->psu_en_gpio) {
		/* Save original GPIO state to restore after power cycle */
		original_gpio_state = gpiod_get_value_cansleep(data->psu_en_gpio);

		/* Disable PSU (gpiod API handles active-low polarity) */
		ret = psu_set_enable_gpio(data, false);
		if (ret < 0) {
			psu_warn(data, "Failed to disable PSU via GPIO during fault clear: %d\n", ret);
			/* Continue anyway - PMBus clear succeeded */
		}

		msleep(PSU_EN_ASSERT_HOLD_MS);

		/* Re-enable PSU - required for fault clear to complete */
		ret = psu_set_enable_gpio(data, true);
		if (ret < 0) {
			psu_warn(data, "Failed to re-enable PSU via GPIO during fault clear: %d\n", ret);
			/* Continue anyway - PMBus clear succeeded */
		}

		/* Restore to original state if it was explicitly disabled (0).
		 * On GPIO read error (negative), default to leaving enabled.
		 */
		if (original_gpio_state == 0) {
			msleep(PSU_EN_ASSERT_HOLD_MS);
			ret = psu_set_enable_gpio(data, false);
			if (ret < 0) {
				psu_warn(data, "Failed to restore PSU GPIO to disabled state: %d\n", ret);
			}
		}
	}

	/* CLEAR_FAULTS is a PMBus "send byte" command, no data byte */
	ret = psu_i2c_send_byte(client, PMBUS_CLEAR_FAULTS_ADDR);
	if (ret < 0) {
		psu_warn(data, "Failed to clear faults via PMBUS CLEAR_FAULTS: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(psu_clear_faults);

/**
 * psu_set_enable - Enable or disable PSU output
 * @client: I2C client device
 * @enable: true to enable, false to disable
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: Uses device-specific OPERATION values since different PSUs use different values.
 * For example, Chicony uses 0x40 for OFF (not standard 0x00).
 */
int psu_set_enable(struct i2c_client *client, bool enable)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u8 val;

	/* Use device-specific OPERATION values if available */
	if (data->ops) {
		val = enable ? data->ops->operation_on_value : data->ops->operation_off_value;
	} else {
		/* Fallback to standard PMBus values */
		val = enable ? PMBUS_OPERATION_ON_VAL : PMBUS_OPERATION_OFF_VAL;
	}

	return psu_i2c_write_byte(client, PMBUS_OPERATION_ADDR, val);
}
EXPORT_SYMBOL_GPL(psu_set_enable);

/**
 * psu_get_enable - Get PSU output enable state
 * @client: I2C client device
 * @enabled: Pointer to store enable state
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: Checks against device-specific ON value since different PSUs use different values.
 */
int psu_get_enable(struct i2c_client *client, bool *enabled)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u8 val;
	int ret;

	ret = psu_i2c_read_byte(client, PMBUS_OPERATION_ADDR, &val);
	if (ret < 0)
		return ret;

	/* Check against device-specific ON value */
	if (data->ops) {
		*enabled = (val == data->ops->operation_on_value);
	} else {
		/* Fallback to standard PMBus value */
		*enabled = (val == PMBUS_OPERATION_ON_VAL);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(psu_get_enable);

/**
 * psu_set_output_voltage - Set PSU output voltage
 * @client: I2C client device
 * @voltage_mv: Voltage in millivolts
 * @exponent: VOUT exponent from VOUT_MODE
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_set_output_voltage(struct i2c_client *client, u32 voltage_mv, s8 exponent)
{
	u16 val = pmbus_mv_to_linear16(voltage_mv, exponent);

	return psu_i2c_write_word(client, PMBUS_VOUT_COMMAND_ADDR, val);
}
EXPORT_SYMBOL_GPL(psu_set_output_voltage);

/**
 * psu_get_output_voltage_setpoint - Get PSU voltage setpoint (VOUT_COMMAND)
 * @client: I2C client device
 * @voltage_mv: Pointer to store voltage setpoint in millivolts
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: This reads the programmed setpoint, not the measured output voltage.
 */
int psu_get_output_voltage_setpoint(struct i2c_client *client, u32 *voltage_mv)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_VOUT_COMMAND_ADDR, &val);
	if (ret < 0) {
		return ret;
	}

	*voltage_mv = pmbus_linear16_to_mv(val, data->vout_exponent);
	return 0;
}
EXPORT_SYMBOL_GPL(psu_get_output_voltage_setpoint);

/**
 * psu_get_output_voltage - Get PSU output voltage
 * @client: I2C client device
 * @voltage_mv: Pointer to store voltage in millivolts
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: VOUT uses Linear-16 format which requires the exponent.
 * We get the exponent from the stored psu_data.
 */
int psu_get_output_voltage(struct i2c_client *client, u32 *voltage_mv)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_VOUT_ADDR, &val);
	if (ret < 0)
		return ret;

	/* VOUT uses Linear-16 format with exponent from VOUT_MODE */
	*voltage_mv = pmbus_linear16_to_mv(val, data->vout_exponent);
	return 0;
}
EXPORT_SYMBOL_GPL(psu_get_output_voltage);

/*
 * =============================================================================
 * Measurement Functions
 * =============================================================================
 */

/**
 * psu_read_voltage_input - Read input voltage
 * @client: I2C client device
 * @voltage_mv: Pointer to store voltage in millivolts
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_voltage_input(struct i2c_client *client, u32 *voltage_mv)
{
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_VIN_ADDR, &val);
	if (ret < 0)
		return ret;

	*voltage_mv = pmbus_linear11_to_milli_unsigned(val);
	return 0;
}

/**
 * psu_read_current_input - Read input current
 * @client: I2C client device
 * @current_ma: Pointer to store current in milliamps
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_current_input(struct i2c_client *client, u32 *current_ma)
{
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_IIN_ADDR, &val);
	if (ret < 0)
		return ret;

	*current_ma = pmbus_linear11_to_milli_unsigned(val);
	return 0;
}

/**
 * psu_read_current_output - Read output current
 * @client: I2C client device
 * @current_ma: Pointer to store current in milliamps
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_current_output(struct i2c_client *client, u32 *current_ma)
{
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_IOUT_ADDR, &val);
	if (ret < 0)
		return ret;

	*current_ma = pmbus_linear11_to_milli_unsigned(val);
	return 0;
}

/**
 * psu_read_power_input - Read input power
 * @client: I2C client device
 * @power_mw: Pointer to store power in milliwatts
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_power_input(struct i2c_client *client, u32 *power_mw)
{
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_PIN_ADDR, &val);
	if (ret < 0)
		return ret;

	*power_mw = pmbus_linear11_to_milli_unsigned(val);
	return 0;
}

/**
 * psu_read_power_output - Read output power
 * @client: I2C client device
 * @power_mw: Pointer to store power in milliwatts
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_power_output(struct i2c_client *client, u32 *power_mw)
{
	u16 val;
	int ret;

	ret = psu_i2c_read_word(client, PMBUS_READ_POUT_ADDR, &val);
	if (ret < 0)
		return ret;

	*power_mw = pmbus_linear11_to_milli_unsigned(val);
	return 0;
}

/**
 * psu_read_temperature - Read temperature sensor
 * @client: I2C client device
 * @sensor: Sensor number (1-based: 1-3 for standard, 4+ for MFR-specific)
 * @temp_mc: Pointer to store temperature in millidegrees Celsius
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_temperature(struct i2c_client *client, int sensor, s32 *temp_mc)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u16 val;
	int ret;
	u8 cmd;

	switch (sensor) {
	case 1:
		cmd = PMBUS_READ_TEMPERATURE_1_ADDR;
		break;
	case 2:
		cmd = PMBUS_READ_TEMPERATURE_2_ADDR;
		break;
	case 3:
		cmd = PMBUS_READ_TEMPERATURE_3_ADDR;
		break;
	default:
		/* Sensors 4+ are MFR-specific - delegate to device driver */
		if (data->ops && data->ops->read_mfr_temp) {
			return data->ops->read_mfr_temp(client,
				sensor - PSU_TEMP_STD_SENSORS - 1, temp_mc);
		}
		return -EINVAL;
	}

	ret = psu_i2c_read_word(client, cmd, &val);
	if (ret < 0)
		return ret;

	/*
	 * Convert LINEAR11 to millidegrees Celsius
	 * pmbus_linear11_to_milli() already multiplies by 1000, so result is in milli-units
	 */
	*temp_mc = pmbus_linear11_to_milli(val);
	return 0;
}

/**
 * psu_read_fan_speed - Read fan speed
 * @client: I2C client device
 * @fan: Fan number (1-4)
 * @rpm: Pointer to store fan speed in RPM
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_read_fan_speed(struct i2c_client *client, int fan, u32 *rpm)
{
	u16 val;
	int ret;
	u8 cmd;

	switch (fan) {
	case 1:
		cmd = PMBUS_READ_FAN_SPEED_1_ADDR;
		break;
	case 2:
		cmd = PMBUS_READ_FAN_SPEED_2_ADDR;
		break;
	case 3:
		cmd = PMBUS_READ_FAN_SPEED_3_ADDR;
		break;
	case 4:
		cmd = PMBUS_READ_FAN_SPEED_4_ADDR;
		break;
	default:
		return -EINVAL;
	}

	ret = psu_i2c_read_word(client, cmd, &val);
	if (ret < 0)
		return ret;

	/* Convert LINEAR11 to RPM (no *1000 scaling - RPM is direct) */
	*rpm = pmbus_linear11_to_int(val);
	return 0;
}

/*
 * =============================================================================
 * Status Monitoring Functions
 * =============================================================================
 */

/**
 * psu_read_status - Read all status registers
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: Only STATUS_WORD is mandatory. Other status registers are optional
 * and failures are silently ignored to avoid log spam for unsupported features.
 */
int psu_read_status(struct psu_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	/* Read STATUS_WORD - this is mandatory */
	ret = psu_i2c_read_word(client, PMBUS_STATUS_WORD_ADDR,
				&data->measurements.status_word);
	if (ret < 0) {
		/* STATUS_WORD failure is a real error */
		if (debug)
			psu_err(data, "Failed to read STATUS_WORD: %d\n", ret);
		return ret;
	}

	/* Read STATUS_VOUT - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_VOUT_ADDR,
				&data->measurements.status_vout);
	if (ret < 0)
		data->measurements.status_vout = 0;

	/* Read STATUS_IOUT - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_IOUT_ADDR,
				&data->measurements.status_iout);
	if (ret < 0)
		data->measurements.status_iout = 0;

	/* Read STATUS_INPUT - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_INPUT_ADDR,
				&data->measurements.status_input);
	if (ret < 0)
		data->measurements.status_input = 0;

	/* Read STATUS_TEMPERATURE - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_TEMPERATURE_ADDR,
				&data->measurements.status_temp);
	if (ret < 0)
		data->measurements.status_temp = 0;

	/* Read STATUS_CML - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_CML_ADDR,
				&data->measurements.status_cml);
	if (ret < 0)
		data->measurements.status_cml = 0;

	/* Read STATUS_OTHER - optional, silently ignore failure */
	ret = psu_i2c_read_byte(client, PMBUS_STATUS_OTHER_ADDR,
				&data->measurements.status_other);
	if (ret < 0)
		data->measurements.status_other = 0;

	/* Read STATUS_MFR_SPECIFIC - only for PSUs that support it */
	if (data->ops && data->ops->supports_mfr_specific) {
		ret = psu_i2c_read_byte(client, PMBUS_STATUS_MFR_SPECIFIC_ADDR,
					&data->measurements.status_mfr_specific);
		if (ret < 0)
			data->measurements.status_mfr_specific = 0;
	} else {
		data->measurements.status_mfr_specific = 0;
	}

	if (data->ops && data->ops->fan_count > 0) {
		/* Read STATUS_FANS_1_2 - optional, silently ignore failure */
		ret = psu_i2c_read_byte(client, PMBUS_STATUS_FANS_1_2_ADDR,
					&data->measurements.status_fans);
		if (ret < 0)
			data->measurements.status_fans = 0;
	} else {
		data->measurements.status_fans = 0;
	}

	/* Read STATUS_FANS_3_4 - optional, silently ignore failure */
	if (data->ops && data->ops->fan_count > 2) {
		ret = psu_i2c_read_byte(client, PMBUS_STATUS_FANS_3_4_ADDR,
					&data->measurements.status_fans_34);
		if (ret < 0)
			data->measurements.status_fans_34 = 0;
	} else {
		data->measurements.status_fans_34 = 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(psu_read_status);

/**
 * psu_parse_faults - Parse status registers and classify faults
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * This function tracks fault state changes and only logs when faults
 * appear or clear, avoiding log spam on every status check.
 */
int psu_parse_faults(struct psu_data *data)
{
	u16 current_status = data->measurements.status_word;
	/* Use a separate baseline from psu_fast_status_check so the fast poll
	 * advancing prev_status_word doesn't suppress detailed classification
	 * here. This path is the only writer to prev_classified_status_word.
	 *
	 * Note: this path emits detailed log lines but never increments
	 * fault_count - that's owned by the fast-poll path (which sees every
	 * transition including transients). Counting here too would
	 * double-count any fault that persists into the next full update.
	 */
	u16 prev_status = data->measurements.prev_classified_status_word;
	u16 new_faults = current_status & ~prev_status;  /* Faults that just appeared */
	u16 cleared_faults = prev_status & ~current_status;  /* Faults that just cleared */

	/* Check for VOUT_OV_FAULT */
	if (new_faults & PMBUS_STATUS_VOUT_OV_FAULT_BIT)
		psu_warn(data, "FAULT: Output overvoltage fault\n");
	if (cleared_faults & PMBUS_STATUS_VOUT_OV_FAULT_BIT)
		psu_info(data, "CLEARED: Output overvoltage fault cleared\n");

	/* Check for IOUT_OC_FAULT */
	if (new_faults & PMBUS_STATUS_IOUT_OC_FAULT_BIT)
		psu_warn(data, "FAULT: Output overcurrent fault\n");
	if (cleared_faults & PMBUS_STATUS_IOUT_OC_FAULT_BIT)
		psu_info(data, "CLEARED: Output overcurrent fault cleared\n");

	/* Check for VIN_UV_FAULT */
	if (new_faults & PMBUS_STATUS_VIN_UV_FAULT_BIT)
		psu_warn(data, "FAULT: Input undervoltage fault\n");
	if (cleared_faults & PMBUS_STATUS_VIN_UV_FAULT_BIT)
		psu_info(data, "CLEARED: Input undervoltage fault cleared\n");

	/* Check for TEMPERATURE faults */
	if (new_faults & PMBUS_STATUS_TEMPERATURE_BIT) {
		u8 temp_status = data->measurements.status_temp;
		if (temp_status & PMBUS_TEMP_OT_FAULT_BIT)
			psu_warn(data, "FAULT: Overtemperature fault\n");
		if (temp_status & PMBUS_TEMP_OT_WARNING_BIT)
			psu_warn(data, "FAULT: Overtemperature warning\n");
	}
	if (cleared_faults & PMBUS_STATUS_TEMPERATURE_BIT)
		psu_info(data, "CLEARED: Temperature fault cleared\n");

	/* Check for FAN faults */
	if (new_faults & PMBUS_STATUS_FANS_BIT) {
		u8 fan_status = data->measurements.status_fans;
		u8 fan_status_34 = data->measurements.status_fans_34;
		if ((fan_status & (PMBUS_FANS_FAN1_FAULT_BIT | PMBUS_FANS_FAN2_FAULT_BIT)) ||
		    (fan_status_34 & (PMBUS_FANS_FAN3_FAULT_BIT | PMBUS_FANS_FAN4_FAULT_BIT)))
			psu_warn(data, "FAULT: Fan fault detected\n");
	}
	if (cleared_faults & PMBUS_STATUS_FANS_BIT)
		psu_info(data, "CLEARED: Fan fault cleared\n");

	/* Check for CML (Communication) faults */
	if (new_faults & PMBUS_STATUS_CML_BIT)
		psu_warn(data, "FAULT: Communication fault (status_cml=0x%02x)\n",
			 data->measurements.status_cml);
	if (cleared_faults & PMBUS_STATUS_CML_BIT)
		psu_info(data, "CLEARED: Communication fault cleared\n");

	/* Check for OFF bit (informational, not an error) */
	if (new_faults & PMBUS_STATUS_OFF_BIT) {
		psu_dbg(data, "PSU is off\n");
	}
	if (cleared_faults & PMBUS_STATUS_OFF_BIT) {
		psu_dbg(data, "PSU is on\n");
	}

	/* Check for BUSY bit (informational, not an error) */
	if (new_faults & PMBUS_STATUS_BUSY_BIT) {
		psu_dbg(data, "PSU is busy\n");
	}
	if (cleared_faults & PMBUS_STATUS_BUSY_BIT) {
		psu_dbg(data, "PSU is no longer busy\n");
	}

	/* Update baseline for next comparison */
	data->measurements.prev_classified_status_word = current_status;

	return 0;
}
EXPORT_SYMBOL_GPL(psu_parse_faults);

/**
 * psu_fast_status_check - Fast status word check for quick fault detection
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * This function performs a fast check of only the STATUS_WORD register
 * to enable quick fault detection and response. It runs on a separate
 * faster workqueue than the full measurement update.
 *
 * Uses change detection to only log faults when they appear or clear.
 */
int psu_fast_status_check(struct psu_data *data)
{
	struct i2c_client *client = data->client;
	u16 status_word = 0;
	u16 new_faults = 0;
	u16 cleared_faults = 0;
	int ret, i;

	/* Read STATUS_WORD with lightweight retry (ENXIO-only).
	 * This deliberately bypasses psu_i2c_with_retry() to avoid:
	 * - PEC toggling overhead on the 250ms hot path
	 * - ENXIO disconnect handling (caller manages disconnect detection)
	 * - Inter-command delay
	 */
	for (i = 0; i < I2C_MAX_RETRIES; i++) {
		ret = i2c_smbus_read_word_data(client, PMBUS_STATUS_WORD_ADDR);
		if (ret >= 0) {
			atomic_long_inc(&data->i2c_total_count);
			atomic_long_add(i, &data->i2c_retry_count);
			break;
		}
		if (ret != -ENXIO) {
			atomic_long_inc(&data->i2c_total_count);
			atomic_long_inc(&data->i2c_failure_count);
			atomic_long_add(i, &data->i2c_retry_count);
			break;  /* Only retry on ENXIO */
		}
		if (i < I2C_MAX_RETRIES - 1) {
			msleep(I2C_RETRY_DELAY_MS);
		}
	}

	/* On success, update the cached status_word BEFORE tracing. The
	 * normal I2C trace path reads data->measurements.status_word as its
	 * level-1 filter reference; updating the cache first ensures both
	 * paths observe consistent state, so whichever path's trace fires
	 * first emits the status-change event and the other correctly
	 * suppresses the duplicate via atomic_xchg on prev_trace_status_word.
	 */
	if (ret >= 0) {
		u16 prev_status;

		status_word = (u16)ret;
		mutex_lock(&data->update_lock);
		prev_status = data->measurements.prev_status_word;
		new_faults = status_word & ~prev_status;
		cleared_faults = prev_status & ~status_word;
		data->measurements.status_word = status_word;
		data->measurements.prev_status_word = status_word;
		mutex_unlock(&data->update_lock);
	}

	{
		struct psu_i2c_op_ctx trace_ctx = {
			.reg = PMBUS_STATUS_WORD_ADDR,
			.word_val = &status_word,
		};
		psu_trace_xfer(data, PSU_I2C_OP_READ_WORD, &trace_ctx,
			       ret >= 0 ? 0 : ret,
			       min_t(int, i, I2C_MAX_RETRIES - 1), false);
	}

	if (ret < 0) {
		/* If we exhausted all ENXIO retries (didn't break early) */
		if (i == I2C_MAX_RETRIES) {
			atomic_long_inc(&data->i2c_total_count);
			atomic_long_inc(&data->i2c_failure_count);
			atomic_long_add(I2C_MAX_RETRIES - 1, &data->i2c_retry_count);
		}
		if (debug)
			psu_info(data, "Fast status check: failed to read STATUS_WORD: %d (after %d attempts)\n",
				 ret, (i < I2C_MAX_RETRIES) ? i + 1 : I2C_MAX_RETRIES);
		return ret;
	}

	/* Process faults using table-driven approach */
	for (i = 0; i < ARRAY_SIZE(fault_table); i++) {
		const struct psu_fault_desc *f = &fault_table[i];

		if (new_faults & f->status_bit) {
			switch (f->severity) {
			case PSU_FAULT_CRITICAL:
				psu_err(data, "FAULT: %s (status=0x%04x)\n",
					f->fault_msg, status_word);
				data->fault_count++;
				break;
			case PSU_FAULT_WARNING:
				psu_warn(data, "FAULT: %s (status=0x%04x)\n",
					 f->fault_msg, status_word);
				data->fault_count++;
				break;
			case PSU_FAULT_INFO:
				psu_dbg(data, "%s\n", f->fault_msg);
				break;
			}
		}

		if (cleared_faults & f->status_bit) {
			if (f->severity == PSU_FAULT_INFO)
				psu_dbg(data, "%s\n", f->clear_msg);
			else
				psu_info(data, "CLEARED: %s\n", f->clear_msg);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(psu_fast_status_check);

/**
 * psu_read_limits - Read PSU hardware limits from manufacturer registers
 * @client: I2C client device
 * @limits: Pointer to limits structure
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: Reads from MFR_* registers (0xA0-0xA7) as documented in Chicony and BOCO specs.
 * All limit registers are optional. Failures are silently ignored.
 * This function always succeeds; limits are set to 0 if not readable.
 */
int psu_read_limits(struct i2c_client *client, struct psu_limits *limits)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u16 val;
	int ret;

	/* Initialize all limits to 0 */
	memset(limits, 0, sizeof(*limits));

	/* Read MFR_VIN_MIN (0xA0) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_VIN_MIN_ADDR, &val);
	if (ret == 0) {
		limits->vin_min_mv = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_VIN_MAX (0xA1) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_VIN_MAX_ADDR, &val);
	if (ret == 0) {
		limits->vin_max_mv = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_IIN_MAX (0xA2) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_IIN_MAX_ADDR, &val);
	if (ret == 0) {
		limits->iin_max_ma = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_IOUT_MAX (0xA6) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_IOUT_MAX_ADDR, &val);
	if (ret == 0) {
		limits->iout_max_ma = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_PIN_MAX (0xA3) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_PIN_MAX_ADDR, &val);
	if (ret == 0) {
		limits->pin_max_mw = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_POUT_MAX (0xA7) - Linear-11 */
	ret = psu_i2c_read_word(client, PMBUS_MFR_POUT_MAX_ADDR, &val);
	if (ret == 0) {
		limits->pout_max_mw = pmbus_linear11_to_milli_unsigned(val);
	}

	/* Read MFR_VOUT_MIN (0xA4) - Linear-16 with exponent */
	ret = psu_i2c_read_word(client, PMBUS_MFR_VOUT_MIN_ADDR, &val);
	if (ret == 0) {
		limits->vout_min_mv = pmbus_linear16_to_mv(val, data->vout_exponent);
	}

	/* Read MFR_VOUT_MAX (0xA5) - Linear-16 with exponent */
	ret = psu_i2c_read_word(client, PMBUS_MFR_VOUT_MAX_ADDR, &val);
	if (ret == 0) {
		limits->vout_max_mv = pmbus_linear16_to_mv(val, data->vout_exponent);
	}

	/*
	 * If device-specific driver provides hardcoded limits, use those as fallback.
	 * This is useful for PSUs that don't support MFR_* limit registers.
	 * The callback can provide any/all limits - we only use values that weren't
	 * successfully read from the hardware.
	 */
	if (data && data->ops && data->ops->get_limits) {
		struct psu_limits device_limits;
		memset(&device_limits, 0, sizeof(device_limits));

		ret = data->ops->get_limits(client, &device_limits);
		if (ret == 0) {
			/* Use device-specific limits for any values that are still 0 */
			if (limits->vout_min_mv == 0 && device_limits.vout_min_mv != 0) {
				psu_dbg(data, "Using default limits for VOUT_MIN as fallback\n");
				limits->vout_min_mv = device_limits.vout_min_mv;
			}
			if (limits->vout_max_mv == 0 && device_limits.vout_max_mv != 0) {
				psu_dbg(data, "Using default limits for VOUT_MAX as fallback\n");
				limits->vout_max_mv = device_limits.vout_max_mv;
			}
			if (limits->iout_max_ma == 0 && device_limits.iout_max_ma != 0) {
				psu_dbg(data, "Using default limits for IOUT_MAX as fallback\n");
				limits->iout_max_ma = device_limits.iout_max_ma;
			}
			if (limits->pout_max_mw == 0 && device_limits.pout_max_mw != 0) {
				psu_dbg(data, "Using default limits for POUT_MAX as fallback\n");
				limits->pout_max_mw = device_limits.pout_max_mw;
			}
			if (limits->vin_min_mv == 0 && device_limits.vin_min_mv != 0) {
				psu_dbg(data, "Using default limits for VIN_MIN as fallback\n");
				limits->vin_min_mv = device_limits.vin_min_mv;
			}
			if (limits->vin_max_mv == 0 && device_limits.vin_max_mv != 0) {
				psu_dbg(data, "Using default limits for VIN_MAX as fallback\n");
				limits->vin_max_mv = device_limits.vin_max_mv;
			}
			if (limits->iin_max_ma == 0 && device_limits.iin_max_ma != 0) {
				psu_dbg(data, "Using default limits for IIN_MAX as fallback\n");
				limits->iin_max_ma = device_limits.iin_max_ma;
			}
			if (limits->pin_max_mw == 0 && device_limits.pin_max_mw != 0) {
				psu_dbg(data, "Using default limits for PIN_MAX as fallback\n");
				limits->pin_max_mw = device_limits.pin_max_mw;
			}
		}
	}

	/* Log the limits if we successfully read any */
	if (limits->vout_min_mv || limits->vout_max_mv || limits->iout_max_ma ||
	    limits->pout_max_mw || limits->vin_min_mv || limits->vin_max_mv ||
	    limits->iin_max_ma || limits->pin_max_mw) {
		psu_info(data, "PSU Limits: VIN=%u-%u mV, VOUT=%u-%u mV, IIN_MAX=%u mA, "
			 "IOUT_MAX=%u mA, PIN_MAX=%u mW, POUT_MAX=%u mW\n",
			 limits->vin_min_mv, limits->vin_max_mv,
			 limits->vout_min_mv, limits->vout_max_mv,
			 limits->iin_max_ma, limits->iout_max_ma,
			 limits->pin_max_mw, limits->pout_max_mw);
	} else {
		psu_info(data, "PSU limits not available (MFR_* registers not supported)\n");
	}

	return 0;
}
EXPORT_SYMBOL_GPL(psu_read_limits);

/**
 * psu_update_measurements - Update all cached measurements
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: This function tries to read all measurements even if some fail.
 * Individual measurements are zeroed on failure but the update continues.
 */
int psu_update_measurements(struct psu_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	int i;

	/* Don't attempt measurements if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED) {
		return -ENODEV;
	}

	mutex_lock(&data->update_lock);

	/* Read voltages - don't bail on error, continue trying */
	ret = psu_read_voltage_input(client, &data->measurements.vin);
	if (ret < 0)
		data->measurements.vin = 0;
	else
		atomic_set(&data->cached_vin_mv, data->measurements.vin);

	ret = psu_get_output_voltage(client, &data->measurements.vout);
	if (ret < 0)
		data->measurements.vout = 0;

	/* Read currents */
	ret = psu_read_current_input(client, &data->measurements.iin);
	if (ret < 0)
		data->measurements.iin = 0;

	ret = psu_read_current_output(client, &data->measurements.iout);
	if (ret < 0) {
		data->measurements.iout = 0;
	}

	/* Read power */
	ret = psu_read_power_input(client, &data->measurements.pin);
	if (ret < 0) {
		data->measurements.pin = 0;
	}

	ret = psu_read_power_output(client, &data->measurements.pout);
	if (ret < 0) {
		data->measurements.pout = 0;
	}

	/* Read temperatures (standard + MFR-specific) */
	{
		int temp_count = PSU_TEMP_STD_SENSORS;
		if (data->ops)
			temp_count += data->ops->mfr_temp_sensor_count;

		for (i = 0; i < temp_count && i < PSU_TEMP_MAX_SENSORS; i++) {
			ret = psu_read_temperature(client, i + 1, &data->measurements.temp[i]);
			if (ret < 0) {
				/* Some sensors may not exist, that's OK */
				data->measurements.temp[i] = 0;
			}
		}
	}

	/* Read fan speeds (only if PSU supports fans) */
	data->measurements.fan_count = 0;
	if (data->ops && data->ops->fan_count > 0) {
		int max_fans = data->ops->fan_count;
		for (i = 0; i < max_fans && i < 4; i++) {
			ret = psu_read_fan_speed(client, i + 1, &data->measurements.fan_speed[i]);
			if (ret == 0 && data->measurements.fan_speed[i] > 0)
				data->measurements.fan_count++;
			else
				data->measurements.fan_speed[i] = 0;
		}
		for (; i < 4; i++)
			data->measurements.fan_speed[i] = 0;
	} else {
		for (i = 0; i < 4; i++)
			data->measurements.fan_speed[i] = 0;
	}

	ret = psu_read_status(data);
	if (ret == 0) {
		/* Parse faults only if status read succeeded */
		psu_parse_faults(data);
	}

	/* Update timestamp and mark measurements as valid
	 * We always mark as valid since we're tolerating partial failures
	 * and individual failed measurements are already set to 0
	 */
	data->measurements.last_update = jiffies;
	data->measurements.valid = true;

	if (debug) {
		psu_info(data, "Measurements: VIN=%u mV, VOUT=%u mV, IIN=%u mA, IOUT=%u mA, "
			"PIN=%u mW, POUT=%u mW, TEMP1=%d mC, TEMP2=%d mC, TEMP3=%d mC, "
			"status=0x%04x\n",
			data->measurements.vin, data->measurements.vout,
			data->measurements.iin, data->measurements.iout,
			data->measurements.pin, data->measurements.pout,
			data->measurements.temp[0], data->measurements.temp[1], data->measurements.temp[2],
			data->measurements.status_word);
	}

	mutex_unlock(&data->update_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(psu_update_measurements);

/**
 * psu_i2c_probe_address - Probe an I2C address for PSU presence
 * @adapter: I2C adapter to use
 * @addr: 7-bit I2C address to probe
 *
 * Returns: D6 register value (>= 0) if PSU detected, negative error code otherwise.
 *          0x00 = normal mode, 0x01 = bootloader mode.
 *
 * This function attempts to read the PMBUS_MFR_FWUPLOAD_MODE_ADDR register
 * to determine if a valid PSU is present at the given address. This is used
 * by the parent driver during address scanning and bootloader detection.
 */
int psu_i2c_probe_address(struct i2c_adapter *adapter, u8 addr)
{
	struct i2c_client client;
	int ret;

	/* Initialize a temporary I2C client structure */
	memset(&client, 0, sizeof(client));
	client.adapter = adapter;
	client.addr = addr;
	snprintf(client.name, I2C_NAME_SIZE, "psu-probe");

	/* Attempt to read PMBUS_MFR_FWUPLOAD_MODE_ADDR (0xD6) register
	 * This register exists on all PSUs and is safe to probe even in bootloader mode
	 * Valid values are 0x00 (normal mode) or 0x01 (bootloader mode)
	 * Using this instead of STATUS_WORD as it's safer for PSUs in bootloader mode
	 */
	ret = i2c_smbus_read_byte_data(&client, PMBUS_MFR_FWUPLOAD_MODE_ADDR);
	if (ret < 0) {
		/* Read failed - no PSU at this address or communication error */
		return ret;
	}

	/* Successfully read the mode register - PSU is present
	 * Return the register value so callers can check bootloader status.
	 * Value should be 0x00 (normal) or 0x01 (bootloader).
	 */
	return ret;
}
EXPORT_SYMBOL_GPL(psu_i2c_probe_address);

MODULE_DESCRIPTION("PSU PMBus Protocol Implementation");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
