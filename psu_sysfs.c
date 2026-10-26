// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Driver - sysfs Interface
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This file provides sysfs attributes for PSU control and information.
 * Attributes are exposed at /sys/bus/i2c/devices/X-00YY/
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/delay.h>

#include "psu_pmbus.h"

/*
 * =============================================================================
 * PSU Limits Attributes (Read-Only)
 * =============================================================================
 */

static ssize_t vin_min_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.vin_min_mv);
}
static DEVICE_ATTR_RO(vin_min);

static ssize_t vin_max_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.vin_max_mv);
}
static DEVICE_ATTR_RO(vin_max);

static ssize_t vout_min_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.vout_min_mv);
}
static DEVICE_ATTR_RO(vout_min);

static ssize_t vout_max_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.vout_max_mv);
}
static DEVICE_ATTR_RO(vout_max);

static ssize_t iin_max_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.iin_max_ma);
}
static DEVICE_ATTR_RO(iin_max);

static ssize_t iout_max_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.iout_max_ma);
}
static DEVICE_ATTR_RO(iout_max);

static ssize_t pin_max_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.pin_max_mw);
}
static DEVICE_ATTR_RO(pin_max);

static ssize_t pout_max_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->limits.pout_max_mw);
}
static DEVICE_ATTR_RO(pout_max);

static struct attribute *psu_limit_attrs[] = {
	&dev_attr_vin_min.attr,
	&dev_attr_vin_max.attr,
	&dev_attr_vout_min.attr,
	&dev_attr_vout_max.attr,
	&dev_attr_iin_max.attr,
	&dev_attr_iout_max.attr,
	&dev_attr_pin_max.attr,
	&dev_attr_pout_max.attr,
	NULL
};

static const struct attribute_group psu_limit_attr_group = {
	.name = "limits",
	.attrs = psu_limit_attrs,
};

/*
 * =============================================================================
 * PSU Device Info Attributes (Read-Only)
 * =============================================================================
 */

static ssize_t vendor_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%s\n", data->info.vendor);
}
static DEVICE_ATTR_RO(vendor);

static ssize_t model_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%s\n", data->info.model);
}
static DEVICE_ATTR_RO(model);

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%s\n", data->info.serial);
}
static DEVICE_ATTR_RO(serial_number);

static ssize_t revision_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%s\n", data->info.revision);
}
static DEVICE_ATTR_RO(revision);

static ssize_t psu_type_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));

	return sysfs_emit(buf, "%s\n", psu_type_name(data->info.type));
}
static DEVICE_ATTR_RO(psu_type);

static ssize_t psu_id_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->psu_id);
}
static DEVICE_ATTR_RO(psu_id);

static struct attribute *psu_info_attrs[] = {
	&dev_attr_vendor.attr,
	&dev_attr_model.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_revision.attr,
	NULL
};

static const struct attribute_group psu_info_attr_group = {
	.name = "info",
	.attrs = psu_info_attrs,
};

/*
 * =============================================================================
 * PSU Status Attributes (Read-Only)
 * =============================================================================
 */

static ssize_t status_word_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_word;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%04x\n", status);
}
static DEVICE_ATTR_RO(status_word);

static ssize_t status_vout_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_vout;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_vout);

static ssize_t status_iout_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_iout;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_iout);

static ssize_t status_input_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_input;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_input);

static ssize_t status_temperature_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_temp;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_temperature);

static ssize_t status_cml_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_cml;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_cml);

static ssize_t status_other_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_other;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_other);

static ssize_t status_mfr_specific_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 status;

	mutex_lock(&data->update_lock);
	status = data->measurements.status_mfr_specific;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "0x%02x\n", status);
}
static DEVICE_ATTR_RO(status_mfr_specific);

static ssize_t fault_count_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%lu\n", data->fault_count);
}
static DEVICE_ATTR_RO(fault_count);

static ssize_t i2c_failure_count_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%ld\n", atomic_long_read(&data->i2c_failure_count));
}
static DEVICE_ATTR_RO(i2c_failure_count);

static ssize_t i2c_total_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%ld\n", atomic_long_read(&data->i2c_total_count));
}
static DEVICE_ATTR_RO(i2c_total_count);

static ssize_t i2c_retry_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%ld\n", atomic_long_read(&data->i2c_retry_count));
}
static DEVICE_ATTR_RO(i2c_retry_count);

static ssize_t disconnect_threshold_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->disconnect_threshold);
}
static DEVICE_ATTR_RO(disconnect_threshold);

/* Root-level status/identity attributes (no subdirectory) */
static struct attribute *psu_root_attrs[] = {
	&dev_attr_psu_type.attr,
	&dev_attr_psu_id.attr,
	&dev_attr_fault_count.attr,
	&dev_attr_i2c_failure_count.attr,
	&dev_attr_i2c_total_count.attr,
	&dev_attr_i2c_retry_count.attr,
	&dev_attr_disconnect_threshold.attr,
	NULL
};

static const struct attribute_group psu_root_attr_group = {
	.attrs = psu_root_attrs,
};

/* Raw status attributes (under raw/ subdirectory) */
static struct attribute *psu_raw_attrs[] = {
	&dev_attr_status_word.attr,
	&dev_attr_status_vout.attr,
	&dev_attr_status_iout.attr,
	&dev_attr_status_input.attr,
	&dev_attr_status_temperature.attr,
	&dev_attr_status_cml.attr,
	&dev_attr_status_other.attr,
	&dev_attr_status_mfr_specific.attr,
	NULL
};

static const struct attribute_group psu_raw_attr_group = {
	.name = "raw",
	.attrs = psu_raw_attrs,
};

/*
 * =============================================================================
 * PSU Measurement Attributes (Read-Only)
 * =============================================================================
 */

/**
 * power1_input_mw - Input power in milliwatts
 */
static ssize_t power1_input_mw_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	int pin_mw;

	mutex_lock(&data->update_lock);

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED) {
		mutex_unlock(&data->update_lock);
		return -ENODEV;
	}

	/* Return error if measurements are not valid */
	if (!data->measurements.valid) {
		mutex_unlock(&data->update_lock);
		return -ENODATA;
	}

	pin_mw = data->measurements.pin;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "%d\n", pin_mw);
}
static DEVICE_ATTR_RO(power1_input_mw);

/**
 * power2_input_mw - Output power in milliwatts
 */
static ssize_t power2_input_mw_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	int pout_mw;

	mutex_lock(&data->update_lock);

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED) {
		mutex_unlock(&data->update_lock);
		return -ENODEV;
	}

	/* Return error if measurements are not valid */
	if (!data->measurements.valid) {
		mutex_unlock(&data->update_lock);
		return -ENODATA;
	}

	pout_mw = data->measurements.pout;
	mutex_unlock(&data->update_lock);

	return sprintf(buf, "%d\n", pout_mw);
}
static DEVICE_ATTR_RO(power2_input_mw);

static struct attribute *psu_measurement_attrs[] = {
	&dev_attr_power1_input_mw.attr,
	&dev_attr_power2_input_mw.attr,
	NULL
};

static const struct attribute_group psu_measurement_attr_group = {
	.attrs = psu_measurement_attrs,
};

/*
 * =============================================================================
 * PSU Control Attributes (Read/Write)
 * =============================================================================
 */

/**
 * psu_enable - Enable/disable PSU output
 */
static ssize_t psu_enable_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	struct i2c_client *client = data->client;
	bool enabled;
	int ret;

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED)
		return -ENODEV;

	ret = psu_get_enable(client, &enabled);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", enabled ? 1 : 0);
}

static ssize_t psu_enable_store(struct device *dev,
			        struct device_attribute *attr,
			        const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	struct i2c_client *client = data->client;
	bool enable;
	int ret;

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED)
		return -ENODEV;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	/* Control PSU output via PMBus OPERATION command only */
	/* GPIO remains HIGH (asserted) - it's a hardware precondition, not output control */
	ret = psu_set_enable(client, enable);
	if (ret < 0) {
		return ret;
	}

	/* Update state */
	data->state = enable ? PSU_STATE_ENABLED : PSU_STATE_DISABLED;

	/* Sync cached setpoint with PSU hardware after enabling */
	if (enable) {
		u32 psu_setpoint_mv;
		if (!psu_get_output_voltage_setpoint(client, &psu_setpoint_mv)) {
			data->setpoint_mv = psu_setpoint_mv;
		}
		/* On read failure, keep existing cached value */
	}

	psu_info(data, "PSU output %s via PMBus OPERATION command\n",
		 enable ? "enabled" : "disabled");

	return count;
}
static DEVICE_ATTR_RW(psu_enable);

/**
 * output_voltage - Set/get PSU output voltage
 */
static ssize_t output_voltage_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	return sprintf(buf, "%u\n", data->setpoint_mv);
}

static ssize_t output_voltage_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	struct i2c_client *client = data->client;
	u32 voltage_mv;
	int ret;

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED)
		return -ENODEV;

	ret = kstrtou32(buf, 10, &voltage_mv);
	if (ret < 0)
		return ret;

	/* Check minimum voltage change delta (Chicony PSU firmware bug workaround) */
	if (data->info.type == PSU_TYPE_CHICONY_S24 &&
	    data->setpoint_mv > 0 && !data->force_voltage_write) {
		u32 delta = (voltage_mv > data->setpoint_mv) ?
			    (voltage_mv - data->setpoint_mv) :
			    (data->setpoint_mv - voltage_mv);

		if (delta < MINIMUM_VOLTAGE_CHANGE_DELTA_MV) {
			psu_dbg(data, "Ignoring voltage change: %u mV too close to current setpoint %u mV (delta=%u mV < %u mV minimum)\n",
				voltage_mv, data->setpoint_mv, delta, MINIMUM_VOLTAGE_CHANGE_DELTA_MV);
			return count;  /* Silently succeed without changing voltage */
		}
	}

	/* Validate against PSU-specific limits if available */
	if (data->limits.vout_min_mv > 0 && voltage_mv < data->limits.vout_min_mv) {
		psu_err(data, "Voltage %u mV below minimum %u mV\n",
			voltage_mv, data->limits.vout_min_mv);
		return -EINVAL;
	}

	if (data->limits.vout_max_mv > 0 && voltage_mv > data->limits.vout_max_mv) {
		psu_err(data, "Voltage %u mV above maximum %u mV\n",
			voltage_mv, data->limits.vout_max_mv);
		return -EINVAL;
	}

	/* Set voltage */
	ret = psu_set_output_voltage(client, voltage_mv, data->vout_exponent);
	if (ret < 0)
		return ret;

	/* Update setpoint */
	data->setpoint_mv = voltage_mv;
	data->force_voltage_write = false;

	psu_dbg(data, "Output voltage set to %u mV via sysfs\n", voltage_mv);

	return count;
}
static DEVICE_ATTR_RW(output_voltage);

/**
 * clear_faults - Clear PSU faults (write-only)
 */
static ssize_t clear_faults_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	struct i2c_client *client = data->client;
	int ret;
	unsigned long val;

	/* Return error if PSU is disconnected */
	if (data->state == PSU_STATE_DISCONNECTED)
		return -ENODEV;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val != 1)
		return -EINVAL;

	/* Clear faults via PMBus command and GPIO power cycle */
	ret = psu_clear_faults(client);
	if (ret < 0)
		return ret;

	psu_info(data, "Faults cleared via sysfs\n");

	/* Reset fault counter */
	data->fault_count = 0;

	/* Re-read hardware setpoint so output_voltage_show() stays accurate.
	 * Set force flag to bypass the minimum-delta guard on the next write,
	 * since the PSU may have lost its programmed VOUT_COMMAND. */
	psu_get_output_voltage_setpoint(client, &data->setpoint_mv);
	data->force_voltage_write = true;

	return count;
}
static DEVICE_ATTR_WO(clear_faults);

static struct attribute *psu_control_attrs[] = {
	&dev_attr_psu_enable.attr,
	&dev_attr_output_voltage.attr,
	&dev_attr_clear_faults.attr,
	NULL
};

static const struct attribute_group psu_control_attr_group = {
	.name = "control",
	.attrs = psu_control_attrs,
};

/*
 * =============================================================================
 * Attribute Group Registration
 * =============================================================================
 */

static const struct attribute_group *psu_attr_groups[] = {
	&psu_root_attr_group,       /* psu_type, psu_id, fault_count, etc. at root */
	&psu_info_attr_group,       /* info/ subdirectory */
	&psu_control_attr_group,    /* control/ subdirectory */
	&psu_limit_attr_group,      /* limits/ subdirectory */
	&psu_raw_attr_group,        /* raw/ subdirectory (PMBus status registers) */
	&psu_measurement_attr_group, /* power1_input_mw, power2_input_mw at root */
	NULL
};

/**
 * psu_sysfs_create - Create sysfs attributes
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_sysfs_create(struct psu_data *data)
{
	struct device *dev = &data->client->dev;
	int ret;

	/* Create standard attribute groups */
	ret = sysfs_create_groups(&dev->kobj, psu_attr_groups);
	if (ret < 0) {
		psu_err(data, "Failed to create sysfs attributes: %d\n", ret);
		return ret;
	}

	/* Create vendor-specific attribute groups if available */
	if (data->ops && data->ops->vendor_attr_groups) {
		ret = sysfs_create_groups(&dev->kobj, data->ops->vendor_attr_groups);
		if (ret < 0) {
			psu_err(data, "Failed to create vendor sysfs attributes: %d\n", ret);
			sysfs_remove_groups(&dev->kobj, psu_attr_groups);
			return ret;
		}
		psu_info(data, "Created vendor-specific sysfs attributes\n");
	}

	psu_info(data, "Created sysfs control interface\n");
	return 0;
}

/**
 * psu_sysfs_remove - Remove sysfs attributes
 * @data: PSU driver data
 */
void psu_sysfs_remove(struct psu_data *data)
{
	struct device *dev = &data->client->dev;

	/* Remove vendor-specific groups first */
	if (data->ops && data->ops->vendor_attr_groups) {
		sysfs_remove_groups(&dev->kobj, data->ops->vendor_attr_groups);
	}

	/* Remove standard groups */
	sysfs_remove_groups(&dev->kobj, psu_attr_groups);
}

MODULE_DESCRIPTION("PSU PMBus sysfs Interface");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
