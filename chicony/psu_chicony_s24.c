// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Chicony S24 / S4K0E001P (R24000W) PSU Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Device-specific driver for Chicony S24 series PSUs.
 * Handles quirks and special requirements for this PSU model.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include "psu_pmbus.h"
#include "psu_chicony_protocol.h"
#include "psu_trace.h"

/* Chicony S24-specific constants */
#define CHICONY_S24_FAN_COUNT 0

/**
 * chicony_s24_use_pec - Determine if PEC should be used for a command
 * @command: PMBus command code
 * @is_write: True if this is a write operation
 * @is_block: True if this is a block read/write
 *
 * Returns: true if PEC should be enabled, false otherwise
 *
 * Chicony S4K0E001P PSUs have PEC compatibility issues:
 * - PEC MUST BE OFF for all read operations (measurements fail with PEC on)
 * - PEC should be ON only for specific write operations
 * - Based on Rust code: chicony_s24/driver.rs lines 178-228
 */
static bool chicony_s24_use_pec(u8 command, bool is_write, bool is_block)
{
	/* PEC OFF for all reads */
	if (!is_write)
		return false;

	/* PEC ON only for specific write operations */
	switch (command) {
	case PMBUS_OPERATION_ADDR:       /* Enable/disable PSU */
	case PMBUS_VOUT_COMMAND_ADDR:    /* Set voltage */
	case PMBUS_CLEAR_FAULTS_ADDR:    /* Clear faults */
		return true;
	default:
		return false;
	}
}

/**
 * chicony_s24_init - Initialize Chicony S24 PSU
 * @client: I2C client device
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Chicony-specific initialization based on production Rust code.
 */
static int chicony_s24_init(struct i2c_client *client, struct psu_data *data)
{
	/*
	 * CRITICAL: Disable PEC for Chicony PSUs
	 * Some measurements fail with PEC enabled
	 */
	client->flags &= ~I2C_CLIENT_PEC;
	psu_info(data, "Chicony S24: PEC disabled\n");

	/*
	 * Chicony S24 uses fixed VOUT exponent of -9
	 * VOUT_MODE register is not supported
	 */
	data->vout_exponent = CHICONY_VOUT_EXPONENT;
	psu_info(data, "Chicony S24: Using fixed VOUT exponent: %d\n",
		 CHICONY_VOUT_EXPONENT);

	return 0;
}

/**
 * chicony_s24_get_limits - Get hardcoded PSU limits
 * @client: I2C client device
 * @limits: Pointer to limits structure to fill
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Chicony S4K0E001P (R24000W 4000W PSU) hardcoded limits from spec.
 * Use these as fallback if MFR_* limit registers are not available.
 */
static int chicony_s24_get_limits(struct i2c_client *client, struct psu_limits *limits)
{
	if (!limits)
		return -EINVAL;

	/* From Chicony spec for R24000W 4000W model */
	limits->vout_min_mv = CHICONY_S24_VOUT_MIN_MV;
	limits->vout_max_mv = CHICONY_S24_VOUT_MAX_MV;
	limits->iout_max_ma = CHICONY_S24_IOUT_MAX_MA;
	limits->pout_max_mw = CHICONY_S24_POUT_MAX_MW;
	limits->vin_min_mv = CHICONY_S24_VIN_MIN_MV;
	limits->vin_max_mv = CHICONY_S24_VIN_MAX_MV;
	limits->iin_max_ma = CHICONY_S24_IIN_MAX_MA;
	limits->pin_max_mw = CHICONY_S24_PIN_MAX_MW;

	return 0;
}

/**
 * chicony_s24_get_temp_label - Get temperature sensor label
 * @sensor_index: Temperature sensor index (0-2)
 *
 * Returns: Sensor label string
 *
 * Chicony temperature sensor mapping from spec:
 * - Sensor 0 (0x8D): Hotspot temperature
 * - Sensor 1 (0x8E): Exhaust temperature
 * - Sensor 2 (0x8F): Ambient temperature
 */
static const char *chicony_s24_get_temp_label(unsigned int sensor_index)
{
	static const char *labels[] = {
		"hotspot",   /* Sensor 0: 0x8D */
		"exhaust",   /* Sensor 1: 0x8E */
		"ambient",   /* Sensor 2: 0x8F */
	};

	if (sensor_index < ARRAY_SIZE(labels))
		return labels[sensor_index];
	return "unknown";
}

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes
 * =============================================================================
 */

/**
 * otp_limit_show - Read Over-Temperature Protection limit
 */
static ssize_t otp_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, CHICONY_OTP_LIMIT_ADDR, &val);
	if (ret < 0)
		return ret;

	/* Convert LINEAR11 to millidegrees Celsius */
	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}

/**
 * otp_limit_store - Write Over-Temperature Protection limit
 */
static ssize_t otp_limit_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	u16 linear11_val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* Convert millidegrees C to LINEAR11 */
	linear11_val = pmbus_milli_to_linear11(val);

	ret = psu_i2c_write_word(data->client, CHICONY_OTP_LIMIT_ADDR, linear11_val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(otp_limit);

/**
 * ocp_limit_show - Read Over-Current Protection limit
 */
static ssize_t ocp_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, CHICONY_OCP_LIMIT_ADDR, &val);
	if (ret < 0)
		return ret;

	/* Convert LINEAR11 to milliamps */
	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}

/**
 * ocp_limit_store - Write Over-Current Protection limit
 */
static ssize_t ocp_limit_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	u16 linear11_val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* Convert milliamps to LINEAR11 */
	linear11_val = pmbus_milli_to_linear11(val);

	ret = psu_i2c_write_word(data->client, CHICONY_OCP_LIMIT_ADDR, linear11_val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(ocp_limit);

/**
 * ovp_limit_show - Read Over-Voltage Protection limit
 */
static ssize_t ovp_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, CHICONY_OVP_LIMIT_ADDR, &val);
	if (ret < 0)
		return ret;

	/* Convert LINEAR11 to millivolts */
	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}

/**
 * ovp_limit_store - Write Over-Voltage Protection limit
 */
static ssize_t ovp_limit_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	u16 linear11_val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* Convert millivolts to LINEAR11 */
	linear11_val = pmbus_milli_to_linear11(val);

	ret = psu_i2c_write_word(data->client, CHICONY_OVP_LIMIT_ADDR, linear11_val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(ovp_limit);

/**
 * uv_limit_show - Read Under-Voltage limit
 */
static ssize_t uv_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, CHICONY_UV_LIMIT_ADDR, &val);
	if (ret < 0)
		return ret;

	/* Convert LINEAR11 to millivolts */
	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}

/**
 * uv_limit_store - Write Under-Voltage limit
 */
static ssize_t uv_limit_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	u16 linear11_val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* Convert millivolts to LINEAR11 */
	linear11_val = pmbus_milli_to_linear11(val);

	ret = psu_i2c_write_word(data->client, CHICONY_UV_LIMIT_ADDR, linear11_val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(uv_limit);

/*
 * =============================================================================
 * Power Module Status Attributes (STATUS_MFR_SPECIFIC 0x80)
 * =============================================================================
 */

/**
 * chicony_mfr_status_bit_show - Helper to read a single bit from STATUS_MFR_SPECIFIC
 * @dev: Device structure
 * @bit_mask: Bit mask to extract from register
 * @buf: Output buffer for sysfs
 *
 * Returns: Number of bytes written or negative error code
 */
static ssize_t chicony_mfr_status_bit_show(struct device *dev, u8 bit_mask, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 val;
	int ret;

	ret = psu_i2c_read_byte(data->client, PMBUS_STATUS_MFR_SPECIFIC_ADDR, &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(val & bit_mask));
}

#define CHICONY_MFR_STATUS_ATTR_RO(_name, _bit)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	return chicony_mfr_status_bit_show(dev, _bit, buf);		\
}									\
static DEVICE_ATTR_RO(_name)

CHICONY_MFR_STATUS_ATTR_RO(module1_vin_uv_fault, CHICONY_MFR_MODULE1_VIN_UV_FAULT_BIT);
CHICONY_MFR_STATUS_ATTR_RO(module1_pfc_nok, CHICONY_MFR_MODULE1_PFC_NOK_BIT);
CHICONY_MFR_STATUS_ATTR_RO(module1_power_no_good, CHICONY_MFR_MODULE1_POWER_NO_GOOD_BIT);
CHICONY_MFR_STATUS_ATTR_RO(module2_vin_uv_fault, CHICONY_MFR_MODULE2_VIN_UV_FAULT_BIT);
CHICONY_MFR_STATUS_ATTR_RO(module2_pfc_nok, CHICONY_MFR_MODULE2_PFC_NOK_BIT);
CHICONY_MFR_STATUS_ATTR_RO(module2_power_no_good, CHICONY_MFR_MODULE2_POWER_NO_GOOD_BIT);

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - Firmware Version
 * =============================================================================
 *
 * Only exposed on PRIMARY module (0x08). Reads both modules' firmware:
 * - fw_revision_m1: Module 1 firmware, 7 bytes as hex string
 * - fw_revision_m2: Module 2 firmware, 7 bytes as hex string (read via i2c_transfer)
 */

static ssize_t fw_revision_m1_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 raw_buf[CHICONY_FW_REVISION_LEN + 1];
	int ret, i;
	ssize_t len = 0;

	/* Read len+1 bytes: PMBus length prefix + 7 data bytes */
	ret = psu_i2c_read_block(data->client, CHICONY_MFR_FW_REVISION_ADDR,
				 raw_buf, CHICONY_FW_REVISION_LEN + 1);
	if (ret < 0)
		return ret;
	if (ret < CHICONY_FW_REVISION_LEN + 1)
		return -EIO;

	/* Validate length byte from PMBus response */
	if (raw_buf[0] < CHICONY_FW_REVISION_LEN)
		return -EIO;

	/* Output 7 data bytes as hex string (skip length prefix at buf[0]) */
	for (i = 1; i <= CHICONY_FW_REVISION_LEN; i++)
		len += sysfs_emit_at(buf, len, "%02X", raw_buf[i]);
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}
static DEVICE_ATTR_RO(fw_revision_m1);

static ssize_t fw_revision_m2_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	struct i2c_adapter *adapter = data->client->adapter;
	struct i2c_msg msgs[2];
	u8 reg = CHICONY_MFR_FW_REVISION_ADDR;
	u8 raw_buf[CHICONY_FW_REVISION_LEN + 1];
	int ret, i;
	ssize_t len = 0;

	/* Write register address to module 2 */
	msgs[0].addr = CHICONY_SECONDARY_I2C_ADDR;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	/* Read data from module 2 - request len+1 for PMBus length prefix */
	msgs[1].addr = CHICONY_SECONDARY_I2C_ADDR;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = CHICONY_FW_REVISION_LEN + 1;
	msgs[1].buf = raw_buf;

	ret = i2c_transfer(adapter, msgs, 2);

	/* Trace the raw i2c_transfer to secondary module */
	if (data->trace_level >= 2 ||
	    (data->trace_level == 1 && ret != 2)) {
		s32 trace_result = (ret == 2) ? (s32)(CHICONY_FW_REVISION_LEN + 1) :
				   (ret < 0) ? (s32)ret : (s32)-EIO;

		trace_psu_pmbus_xfer(data->psu_id, CHICONY_SECONDARY_I2C_ADDR,
				     CHICONY_MFR_FW_REVISION_ADDR,
				     PSU_I2C_OP_READ_BLOCK,
				     raw_buf,
				     ret == 2 ? CHICONY_FW_REVISION_LEN + 1 : 0,
				     trace_result, 0, 0,
				     data->measurements.status_word);
	}

	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	/* Validate length byte from PMBus response */
	if (raw_buf[0] < CHICONY_FW_REVISION_LEN)
		return -EIO;

	/* Output 7 data bytes as hex string (skip length prefix at buf[0]) */
	for (i = 1; i <= CHICONY_FW_REVISION_LEN; i++)
		len += sysfs_emit_at(buf, len, "%02X", raw_buf[i]);
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}
static DEVICE_ATTR_RO(fw_revision_m2);

/**
 * chicony_vendor_attr_is_visible - Control visibility of vendor attributes
 * @kobj: Kobject for the device
 * @attr: Attribute to check
 * @n: Index in attribute array
 *
 * Returns: Attribute mode if visible, 0 to hide
 *
 * FW version attributes are only visible on the primary module (0x08).
 */
static umode_t chicony_vendor_attr_is_visible(struct kobject *kobj,
					      struct attribute *attr, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct i2c_client *client = to_i2c_client(dev);

	/* FW version attrs only visible on primary module */
	if (attr == &dev_attr_fw_revision_m1.attr ||
	    attr == &dev_attr_fw_revision_m2.attr) {
		return client->addr == CHICONY_PRIMARY_I2C_ADDR ? attr->mode : 0;
	}

	return attr->mode;
}

/* Chicony vendor attribute group */
static struct attribute *chicony_vendor_attrs[] = {
	/* Firmware version (primary 0x08 only - controlled by is_visible) */
	&dev_attr_fw_revision_m1.attr,
	&dev_attr_fw_revision_m2.attr,
	/* Protection limits */
	&dev_attr_otp_limit.attr,
	&dev_attr_ocp_limit.attr,
	&dev_attr_ovp_limit.attr,
	&dev_attr_uv_limit.attr,
	/* Power module status (STATUS_MFR_SPECIFIC 0x80) */
	&dev_attr_module1_vin_uv_fault.attr,
	&dev_attr_module1_pfc_nok.attr,
	&dev_attr_module1_power_no_good.attr,
	&dev_attr_module2_vin_uv_fault.attr,
	&dev_attr_module2_pfc_nok.attr,
	&dev_attr_module2_power_no_good.attr,
	NULL
};

static const struct attribute_group chicony_vendor_attr_group = {
	.name = "chicony",  /* Creates /sys/bus/i2c/devices/X-00YY/chicony/ subdirectory */
	.attrs = chicony_vendor_attrs,
	.is_visible = chicony_vendor_attr_is_visible,
};

static const struct attribute_group *chicony_vendor_attr_groups[] = {
	&chicony_vendor_attr_group,
	NULL
};

/*
 * Chicony S24 Operations Structure
 * This is referenced by the core driver when a Chicony PSU is detected.
 */
struct psu_ops chicony_s24_ops = {
	.name = "Chicony S24 (S4K0E001P)",
	.init = chicony_s24_init,
	.use_pec_for_command = chicony_s24_use_pec,
	.fan_count = CHICONY_S24_FAN_COUNT,             /* Chicony PSUs have no fans - use system fans */
	.skip_vout_mode = true,      					/* Don't read VOUT_MODE */
	.fixed_vout_exponent = CHICONY_VOUT_EXPONENT,   /* Fixed exponent */
	.supports_mfr_specific = true,                  /* Chicony supports STATUS_MFR_SPECIFIC */
	.operation_on_value = CHICONY_OPERATION_ON_VAL,
	.operation_off_value = CHICONY_OPERATION_OFF_VAL,
	.post_init = NULL,
	.get_limits = chicony_s24_get_limits,  			/* Use standard MFR_* commands */
	.get_temp_label = chicony_s24_get_temp_label,   /* Temperature sensor labels */
	.vendor_attr_groups = chicony_vendor_attr_groups,  /* Chicony-specific protection limits */
};
EXPORT_SYMBOL_GPL(chicony_s24_ops);

MODULE_DESCRIPTION("Chicony S24 PSU Driver");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
