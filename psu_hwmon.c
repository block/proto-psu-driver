// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Driver - hwmon Integration
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This file provides hwmon (hardware monitoring) subsystem integration
 * for PSU PMBus driver, exposing all PSU measurements through the
 * standard Linux sensor interface.
 */

#include <linux/module.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/limits.h>

#include "psu_pmbus.h"


/* Default temperature sensor labels (max 8 sensors) */
static const char *default_temp_labels[] = {
	"temp1", "temp2", "temp3", "temp4", "temp5", "temp6", "temp7", "temp8"
};

/* Fan sensor labels */
static const char *fan_labels[] = {
	"fan1", "fan2", "fan3", "fan4"
};

/*
 * =============================================================================
 * Helper Functions for Reading Sensor Values and Alarms
 * =============================================================================
 */

/**
 * read_2ch_value_u32 - Read a u32 value from one of two channels
 * @vals: Array of two u32 value pointers
 * @channel: Channel index (0 or 1)
 * @out: Output value
 *
 * Returns: 0 on success, -EOPNOTSUPP if channel out of range
 */
static inline int read_2ch_value_u32(const u32 *vals[2], int channel, long *out)
{
	if (channel < 0 || channel > 1)
		return -EOPNOTSUPP;
	*out = *vals[channel];
	return 0;
}

/**
 * read_2ch_alarm - Read alarm status from one of two channels
 * @status: Array of two status register pointers (u8)
 * @mask: Array of two bitmasks
 * @channel: Channel index (0 or 1)
 * @out: Output alarm status (0 or 1)
 *
 * Returns: 0 on success, -EOPNOTSUPP if channel out of range
 */
static inline int read_2ch_alarm(const u8 *status[2], const u32 mask[2],
				 int channel, long *out)
{
	if (channel < 0 || channel > 1)
		return -EOPNOTSUPP;
	*out = !!((*status[channel]) & mask[channel]);
	return 0;
}

/**
 * read_indexed_value_s32 - Read a s32 value from an array by index
 * @arr: Array of s32 values (e.g., temperature)
 * @count: Number of elements in array
 * @channel: Channel/index
 * @out: Output value
 *
 * Returns: 0 on success, -EOPNOTSUPP if channel out of range
 */
static inline int read_indexed_value_s32(const s32 *arr, int count, int channel, long *out)
{
	if (channel < 0 || channel >= count)
		return -EOPNOTSUPP;
	*out = arr[channel];
	return 0;
}

/**
 * read_indexed_value_u32 - Read a u32 value from an array by index
 * @arr: Array of u32 values (e.g., fan speed)
 * @count: Number of elements in array
 * @channel: Channel/index
 * @out: Output value
 *
 * Returns: 0 on success, -EOPNOTSUPP if channel out of range
 */
static inline int read_indexed_value_u32(const u32 *arr, int count, int channel, long *out)
{
	if (channel < 0 || channel >= count)
		return -EOPNOTSUPP;
	*out = arr[channel];
	return 0;
}


/*
 * =============================================================================
 * Per-Sensor-Type Handler Functions
 * =============================================================================
 */

/**
 * psu_hwmon_read_in - Handle voltage (in) sensor reads
 */
static int psu_hwmon_read_in(u32 attr, int channel, long *val,
			     const struct psu_data *data)
{
	const struct psu_measurements *m = &data->measurements;

	switch (attr) {
	case hwmon_in_input: {
		const u32 *in_vals[2] = { &m->vin, &m->vout };

		return read_2ch_value_u32(in_vals, channel, val);
	}
	case hwmon_in_min_alarm: {
		const u8 *in_min_stat[2] = { &m->status_input, &m->status_vout };
		const u32 in_min_mask[2] = { PMBUS_INPUT_VIN_UV_WARNING_BIT, PMBUS_VOUT_UV_WARNING_BIT };

		return read_2ch_alarm(in_min_stat, in_min_mask, channel, val);
	}
	case hwmon_in_lcrit_alarm: {
		const u8 *in_lcrit_stat[2] = { &m->status_input, &m->status_vout };
		const u32 in_lcrit_mask[2] = { PMBUS_INPUT_VIN_UV_FAULT_BIT, PMBUS_VOUT_UV_FAULT_BIT };

		return read_2ch_alarm(in_lcrit_stat, in_lcrit_mask, channel, val);
	}
	case hwmon_in_max_alarm: {
		const u8 *in_max_stat[2] = { &m->status_input, &m->status_vout };
		const u32 in_max_mask[2] = { PMBUS_INPUT_VIN_OV_WARNING_BIT, PMBUS_VOUT_OV_WARNING_BIT };

		return read_2ch_alarm(in_max_stat, in_max_mask, channel, val);
	}
	case hwmon_in_crit_alarm: {
		const u8 *in_crit_stat[2] = { &m->status_input, &m->status_vout };
		const u32 in_crit_mask[2] = { PMBUS_INPUT_VIN_OV_FAULT_BIT, PMBUS_VOUT_OV_FAULT_BIT };

		return read_2ch_alarm(in_crit_stat, in_crit_mask, channel, val);
	}
	case hwmon_in_label:
		return -EOPNOTSUPP; /* Handled by read_string */
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * psu_hwmon_read_curr - Handle current (curr) sensor reads
 */
static int psu_hwmon_read_curr(u32 attr, int channel, long *val,
			       const struct psu_data *data)
{
	const struct psu_measurements *m = &data->measurements;

	switch (attr) {
	case hwmon_curr_input: {
		const u32 *curr_vals[2] = { &m->iin, &m->iout };

		return read_2ch_value_u32(curr_vals, channel, val);
	}
	case hwmon_curr_max_alarm: {
		const u8 *curr_max_stat[2] = { &m->status_input, &m->status_iout };
		const u32 curr_max_mask[2] = { PMBUS_INPUT_IIN_OC_WARNING_BIT, PMBUS_IOUT_OC_WARNING_BIT };

		return read_2ch_alarm(curr_max_stat, curr_max_mask, channel, val);
	}
	case hwmon_curr_crit_alarm: {
		const u8 *curr_crit_stat[2] = { &m->status_input, &m->status_iout };
		const u32 curr_crit_mask[2] = { PMBUS_INPUT_IIN_OC_FAULT_BIT, PMBUS_IOUT_OC_FAULT_BIT };

		return read_2ch_alarm(curr_crit_stat, curr_crit_mask, channel, val);
	}
	case hwmon_curr_label:
		return -EOPNOTSUPP; /* Handled by read_string */
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * psu_hwmon_read_power - Handle power sensor reads
 */
static int psu_hwmon_read_power(u32 attr, int channel, long *val,
				const struct psu_data *data)
{
	const struct psu_measurements *m = &data->measurements;

	switch (attr) {
	case hwmon_power_input: {
		/*
		 * Convert milliwatts to microwatts for hwmon spec compliance.
		 *
		 * On 64-bit systems: Can handle up to 9,223 GW without overflow
		 * On 32-bit systems: Return -ERANGE if power > ~2.147 MW (INT32_MAX µW)
		 *
		 * For compatibility, custom sysfs attributes power*_input_mw provide
		 * values in milliwatts for applications that need to handle high power.
		 */
		const u32 *pwr_vals[2] = { &m->pin, &m->pout };
		long power_mw;

		if (read_2ch_value_u32(pwr_vals, channel, &power_mw) < 0)
			return -EOPNOTSUPP;

		#if BITS_PER_LONG == 32
		/* On 32-bit systems, check for overflow */
		if (power_mw > (INT_MAX / 1000)) {
			psu_warn(data, "Power value %ld mW would overflow on 32-bit system\n", power_mw);
			return -ERANGE;
		}
		#endif

		/* Convert mW to µW */
		*val = power_mw * 1000L;
		return 0;
	}
	case hwmon_power_max_alarm: {
		const u8 *pwr_max_stat[2] = { &m->status_input, &m->status_iout };
		const u32 pwr_max_mask[2] = { PMBUS_INPUT_PIN_OP_WARNING_BIT, PMBUS_IOUT_POUT_OP_WARNING_BIT };

		return read_2ch_alarm(pwr_max_stat, pwr_max_mask, channel, val);
	}
	case hwmon_power_crit_alarm:
		if (channel == 0) {
			return -EOPNOTSUPP;
		} else if (channel == 1) {
			*val = !!(m->status_iout & PMBUS_IOUT_POUT_OP_FAULT_BIT);
			return 0;
		}
		return -EOPNOTSUPP;
	case hwmon_power_label:
		return -EOPNOTSUPP; /* Handled by read_string */
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * psu_hwmon_read_temp - Handle temperature sensor reads
 *
 * Note: PMBus STATUS_TEMPERATURE is a global register, not per-sensor.
 * Alarm attributes are only exposed on channel 0 via is_visible().
 */
static int psu_hwmon_read_temp(u32 attr, int channel, long *val,
			       const struct psu_data *data)
{
	const struct psu_measurements *m = &data->measurements;
	int temp_count = PSU_TEMP_STD_SENSORS;

	/* Calculate actual sensor count including MFR-specific */
	if (data->ops)
		temp_count += data->ops->mfr_temp_sensor_count;

	switch (attr) {
	case hwmon_temp_input:
		return read_indexed_value_s32(m->temp, temp_count, channel, val);
	case hwmon_temp_min_alarm:
		*val = !!(m->status_temp & PMBUS_TEMP_UT_WARNING_BIT);
		return 0;
	case hwmon_temp_lcrit_alarm:
		*val = !!(m->status_temp & PMBUS_TEMP_UT_FAULT_BIT);
		return 0;
	case hwmon_temp_max_alarm:
		*val = !!(m->status_temp & PMBUS_TEMP_OT_WARNING_BIT);
		return 0;
	case hwmon_temp_crit_alarm:
		*val = !!(m->status_temp & PMBUS_TEMP_OT_FAULT_BIT);
		return 0;
	case hwmon_temp_label:
		return -EOPNOTSUPP; /* Handled by read_string */
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * psu_hwmon_read_fan - Handle fan sensor reads
 */
static int psu_hwmon_read_fan(u32 attr, int channel, long *val,
			      const struct psu_data *data)
{
	const struct psu_measurements *m = &data->measurements;

	switch (attr) {
	case hwmon_fan_input:
		return read_indexed_value_u32(m->fan_speed, 4, channel, val);
	case hwmon_fan_alarm:
		/* Combine FAULT and WARNING bits; fans 1-2 and 3-4 use different status regs */
		if (channel == 0)
			*val = !!(m->status_fans & (PMBUS_FANS_FAN1_FAULT_BIT | PMBUS_FANS_FAN1_WARNING_BIT));
		else if (channel == 1)
			*val = !!(m->status_fans & (PMBUS_FANS_FAN2_FAULT_BIT | PMBUS_FANS_FAN2_WARNING_BIT));
		else if (channel == 2)
			*val = !!(m->status_fans_34 & (PMBUS_FANS_FAN3_FAULT_BIT | PMBUS_FANS_FAN3_WARNING_BIT));
		else if (channel == 3)
			*val = !!(m->status_fans_34 & (PMBUS_FANS_FAN4_FAULT_BIT | PMBUS_FANS_FAN4_WARNING_BIT));
		else
			return -EOPNOTSUPP;
		return 0;
	case hwmon_fan_label:
		return -EOPNOTSUPP; /* Handled by read_string */
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * =============================================================================
 * Per-Sensor-Type String Label Handler Functions
 * =============================================================================
 */

/**
 * psu_hwmon_read_string_in - Handle voltage (in) sensor labels
 */
static int psu_hwmon_read_string_in(int channel, const char **str)
{
	if (channel == 0) {
		*str = "vin";
		return 0;
	} else if (channel == 1) {
		*str = "vout";
		return 0;
	}
	return -EOPNOTSUPP;
}

/**
 * psu_hwmon_read_string_curr - Handle current (curr) sensor labels
 */
static int psu_hwmon_read_string_curr(int channel, const char **str)
{
	if (channel == 0) {
		*str = "iin";
		return 0;
	} else if (channel == 1) {
		*str = "iout";
		return 0;
	}
	return -EOPNOTSUPP;
}

/**
 * psu_hwmon_read_string_power - Handle power sensor labels
 */
static int psu_hwmon_read_string_power(int channel, const char **str)
{
	if (channel == 0) {
		*str = "pin";
		return 0;
	} else if (channel == 1) {
		*str = "pout";
		return 0;
	}
	return -EOPNOTSUPP;
}

/**
 * psu_hwmon_read_string_temp - Handle temperature sensor labels
 */
static int psu_hwmon_read_string_temp(int channel, const char **str,
				      const struct psu_data *psu_data)
{
	int max_sensors = PSU_TEMP_STD_SENSORS;

	/* Calculate actual sensor count including MFR-specific */
	if (psu_data->ops)
		max_sensors += psu_data->ops->mfr_temp_sensor_count;

	if (channel < 0 || channel >= max_sensors || channel >= PSU_TEMP_MAX_SENSORS)
		return -EOPNOTSUPP;

	/* Use device-specific temperature labels if available */
	if (psu_data->ops && psu_data->ops->get_temp_label) {
		*str = psu_data->ops->get_temp_label(channel);
	} else {
		/* Default labels if not provided by device */
		*str = default_temp_labels[channel];
	}
	return 0;
}

/**
 * psu_hwmon_read_string_fan - Handle fan sensor labels
 */
static int psu_hwmon_read_string_fan(int channel, const char **str)
{
	if (channel < 0 || channel >= 4)
		return -EOPNOTSUPP;

	*str = fan_labels[channel];
	return 0;
}

/*
 * =============================================================================
 * Main hwmon Read Callback
 * =============================================================================
 */

/**
 * psu_hwmon_read - hwmon read callback
 * @dev: Device pointer
 * @type: Sensor type
 * @attr: Attribute
 * @channel: Channel number
 * @val: Pointer to store value
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct psu_data *data = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&data->update_lock);

	/* Return NO DEVICE error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED) {
		mutex_unlock(&data->update_lock);
		return -ENODEV;
	}

	/* Return NO DATA error if measurements are not valid */
	if (!data->measurements.valid) {
		mutex_unlock(&data->update_lock);
		return -ENODATA;
	}

	/* Dispatch to per-sensor-type handler functions */
	switch (type) {
	case hwmon_in:
		ret = psu_hwmon_read_in(attr, channel, val, data);
		break;
	case hwmon_curr:
		ret = psu_hwmon_read_curr(attr, channel, val, data);
		break;
	case hwmon_power:
		ret = psu_hwmon_read_power(attr, channel, val, data);
		break;
	case hwmon_temp:
		ret = psu_hwmon_read_temp(attr, channel, val, data);
		break;
	case hwmon_fan:
		ret = psu_hwmon_read_fan(attr, channel, val, data);
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	mutex_unlock(&data->update_lock);
	return ret;
}

/**
 * psu_hwmon_read_string - hwmon string read callback for labels
 * @dev: Device pointer
 * @type: Sensor type
 * @attr: Attribute
 * @channel: Channel number
 * @str: Pointer to store string pointer
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_hwmon_read_string(struct device *dev,
				 enum hwmon_sensor_types type,
				 u32 attr, int channel, const char **str)
{
	struct psu_data *psu_data = dev_get_drvdata(dev);

	/* Dispatch to per-sensor-type string handler functions */
	switch (type) {
	case hwmon_in:
		return psu_hwmon_read_string_in(channel, str);
	case hwmon_curr:
		return psu_hwmon_read_string_curr(channel, str);
	case hwmon_power:
		return psu_hwmon_read_string_power(channel, str);
	case hwmon_temp:
		return psu_hwmon_read_string_temp(channel, str, psu_data);
	case hwmon_fan:
		return psu_hwmon_read_string_fan(channel, str);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * psu_hwmon_is_visible - Determine which attributes are visible
 * @data: PSU driver data
 * @type: Sensor type
 * @attr: Attribute
 * @channel: Channel number
 *
 * Returns: File permission mode if visible, 0 if hidden
 */
static umode_t psu_hwmon_is_visible(const void *data,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	const struct psu_data *psu_data = data;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			if (channel < 2) /* VIN and VOUT */
				return 0444;  /* Always visible */
			break;

		case hwmon_in_min_alarm:
		case hwmon_in_max_alarm:
		case hwmon_in_lcrit_alarm:
		case hwmon_in_crit_alarm:
			if (channel < 2)
				return 0444;  /* Alarms always visible */
			break;

		default:
			break;
		}
		break;

	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
		case hwmon_curr_label:
		case hwmon_curr_max_alarm:
		case hwmon_curr_crit_alarm:
			if (channel < 2) /* IIN and IOUT */
				return 0444;  /* Always visible */
			break;

		default:
			break;
		}
		break;

	case hwmon_power:
		switch (attr) {
		case hwmon_power_crit_alarm:
			if (channel == 1) /* only on POUT */
				return 0444;
			break;
		case hwmon_power_input:
		case hwmon_power_label:
		case hwmon_power_max_alarm:
			if (channel < 2) /* PIN and POUT */
				return 0444;  /* Always visible */
			break;

		default:
			break;
		}
		break;

	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			/* Standard sensors (0-2) always visible */
			if (channel < PSU_TEMP_STD_SENSORS)
				return 0444;
			/* MFR-specific sensors visible based on driver capability */
			if (psu_data->ops &&
			    channel < PSU_TEMP_STD_SENSORS + psu_data->ops->mfr_temp_sensor_count)
				return 0444;
			break;
		case hwmon_temp_min_alarm:
		case hwmon_temp_max_alarm:
		case hwmon_temp_lcrit_alarm:
		case hwmon_temp_crit_alarm:
			/* PMBus STATUS_TEMPERATURE is global - only expose on channel 0 */
			if (channel == 0)
				return 0444;
			break;

		default:
			break;
		}
		break;

	case hwmon_fan:
		/* Only show fan attributes if PSU supports this fan */
		if (psu_data->ops && channel < psu_data->ops->fan_count) {
			switch (attr) {
			case hwmon_fan_input:
			case hwmon_fan_label:
			case hwmon_fan_alarm:
				return 0444;
			default:
				break;
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

static const struct hwmon_ops psu_hwmon_ops = {
	.read = psu_hwmon_read,
	.read_string = psu_hwmon_read_string,
	.is_visible = psu_hwmon_is_visible,
};

/* Define hwmon chip configuration */
static const struct hwmon_channel_info *psu_hwmon_info[] = {
	HWMON_CHANNEL_INFO(in,
			   /* VIN - channel 0 */
			   HWMON_I_INPUT | HWMON_I_LABEL |
			   HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM |
			   HWMON_I_LCRIT_ALARM | HWMON_I_CRIT_ALARM,
			   /* VOUT - channel 1 */
			   HWMON_I_INPUT | HWMON_I_LABEL |
			   HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM |
			   HWMON_I_LCRIT_ALARM | HWMON_I_CRIT_ALARM),
	HWMON_CHANNEL_INFO(curr,
			   /* IIN - channel 0 */
			   HWMON_C_INPUT | HWMON_C_LABEL |
			   HWMON_C_MAX_ALARM | HWMON_C_CRIT_ALARM,
			   /* IOUT - channel 1 */
			   HWMON_C_INPUT | HWMON_C_LABEL |
			   HWMON_C_MAX_ALARM | HWMON_C_CRIT_ALARM),
	HWMON_CHANNEL_INFO(power,
			   /* PIN - channel 0 */
			   HWMON_P_INPUT | HWMON_P_LABEL |
			   HWMON_P_MAX_ALARM | HWMON_P_CRIT_ALARM,
			   /* POUT - channel 1 */
			   HWMON_P_INPUT | HWMON_P_LABEL |
			   HWMON_P_MAX_ALARM | HWMON_P_CRIT_ALARM),
	HWMON_CHANNEL_INFO(temp,
			   /* TEMP1 - channel 0: Standard PMBus (0x8D) */
			   HWMON_T_INPUT | HWMON_T_LABEL |
			   HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_LCRIT_ALARM | HWMON_T_CRIT_ALARM,
			   /* TEMP2 - channel 1: Standard PMBus (0x8E) */
			   HWMON_T_INPUT | HWMON_T_LABEL |
			   HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_LCRIT_ALARM | HWMON_T_CRIT_ALARM,
			   /* TEMP3 - channel 2: Standard PMBus (0x8F) */
			   HWMON_T_INPUT | HWMON_T_LABEL |
			   HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM |
			   HWMON_T_LCRIT_ALARM | HWMON_T_CRIT_ALARM,
			   /* TEMP4-8 - channels 3-7: MFR-specific (visibility controlled by is_visible) */
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ALARM,    /* FAN1 */
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ALARM,    /* FAN2 */
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ALARM,    /* FAN3 */
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ALARM),   /* FAN4 */
	NULL
};

static const struct hwmon_chip_info psu_hwmon_chip_info = {
	.ops = &psu_hwmon_ops,
	.info = psu_hwmon_info,
};

/**
 * psu_hwmon_register - Register PSU with hwmon subsystem
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_hwmon_register(struct psu_data *data)
{
	struct device *dev = &data->client->dev;
	struct device *hwmon_dev;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "psu_pmbus",
						      data, &psu_hwmon_chip_info,
						      NULL);
	if (IS_ERR(hwmon_dev)) {
		psu_err(data, "Failed to register hwmon device: %ld\n",
			PTR_ERR(hwmon_dev));
		return PTR_ERR(hwmon_dev);
	}

	data->hwmon_dev = hwmon_dev;
	psu_info(data, "Registered hwmon device\n");

	return 0;
}

/**
 * psu_hwmon_unregister - Clear hwmon device reference
 * @data: PSU driver data
 *
 * Note: With devm registration, actual device cleanup is handled
 * automatically when the parent device is removed.
 */
void psu_hwmon_unregister(struct psu_data *data)
{
	/* devm handles actual unregistration, just clear the pointer */
	data->hwmon_dev = NULL;
}

MODULE_DESCRIPTION("PSU PMBus Hwmon Integration");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
