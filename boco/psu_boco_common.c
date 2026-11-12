// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * BOCO PSU Common Functions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Shared implementation for all BOCO PSU models.
 * Contains common PEC logic, temperature reading, sysfs attributes
 * (firmware version + watchdog), and initialization.
 */

#include <linux/module.h>
#include <linux/i2c.h>

#include "psu_pmbus.h"
#include "psu_boco_protocol.h"

/*
 * =============================================================================
 * Common BOCO Functions
 * =============================================================================
 */

bool boco_common_use_pec(u8 command, bool is_write, bool is_block)
{
	/* Only block reads don't support PEC */
	return !is_block;
}

int boco_common_init(struct i2c_client *client, struct psu_data *data)
{
	client->flags |= I2C_CLIENT_PEC;
	psu_info(data, "BOCO: PEC enabled\n");

	/* Fixed VOUT exponent */
	data->vout_exponent = BOCO_VOUT_EXPONENT;
	psu_info(data, "BOCO: Using VOUT exponent: %d\n", BOCO_VOUT_EXPONENT);

	return 0;
}

int boco_common_read_mfr_temp(struct i2c_client *client, int sensor_index, s32 *temp_mc)
{
	u16 val;
	int ret;

	if (sensor_index != 0)  /* Only have 1 MFR temp sensor (LLC) */
		return -EINVAL;

	ret = psu_i2c_read_word(client, BOCO_LLC_TEMP_ADDR, &val);
	if (ret < 0)
		return ret;

	*temp_mc = pmbus_linear11_to_milli(val);
	return 0;
}

const char *boco_common_get_temp_label(unsigned int sensor_index)
{
	static const char *labels[] = {
		"ambient",   /* Sensor 0: PMBUS_READ_TEMPERATURE_1_ADDR (0x8D) */
		"sr",        /* Sensor 1: PMBUS_READ_TEMPERATURE_2_ADDR (0x8E) - Synchronous Rectifier */
		"pfc",       /* Sensor 2: PMBUS_READ_TEMPERATURE_3_ADDR (0x8F) - Power Factor Correction */
		"llc",       /* Sensor 3: BOCO_LLC_TEMP_ADDR (0xE9) - LLC resonant converter */
	};

	if (sensor_index < ARRAY_SIZE(labels))
		return labels[sensor_index];
	return "unknown";
}

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - Firmware Version
 *
 * Each register outputs raw 6-byte ASCII string as-is.
 * =============================================================================
 */

/**
 * boco_read_fw_revision - Read firmware revision with PEC disabled
 * @client: I2C client
 * @reg: Register address (APP, BL, or PRODUCT)
 * @buf: Output buffer for sysfs
 *
 * Returns: Number of bytes written to buf, or negative error code
 */
static ssize_t boco_read_fw_revision(struct i2c_client *client, u8 reg, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(client);
	u8 raw_buf[BOCO_FW_VERSION_LEN + 2];
	int ret;

	/* Use mutex to protect PEC flag modification from concurrent access */
	mutex_lock(&data->update_lock);

	/* Disable PEC for block reads - Boco doesn't support PEC on block ops */
	client->flags &= ~I2C_CLIENT_PEC;

	/* Read len+1 bytes: PMBus length prefix + 6 data bytes */
	ret = psu_i2c_read_block(client, reg, raw_buf, BOCO_FW_VERSION_LEN + 1);

	/* Restore PEC - Boco always uses PEC except for block reads */
	client->flags |= I2C_CLIENT_PEC;

	mutex_unlock(&data->update_lock);

	if (ret < 0)
		return ret;
	if (ret < BOCO_FW_VERSION_LEN + 1)
		return -EIO;

	/* Validate length byte from PMBus response */
	if (raw_buf[0] < BOCO_FW_VERSION_LEN)
		return -EIO;

	/* Output raw 6-byte ASCII string (skip length prefix at buf[0]) */
	return sysfs_emit(buf, "%.6s\n", &raw_buf[1]);
}

static ssize_t app_fw_revision_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));

	return boco_read_fw_revision(data->client, BOCO_APP_FW_REVISION_ADDR, buf);
}
static DEVICE_ATTR_RO(app_fw_revision);

static ssize_t bl_fw_revision_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));

	return boco_read_fw_revision(data->client, BOCO_BL_FW_REVISION_ADDR, buf);
}
static DEVICE_ATTR_RO(bl_fw_revision);

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - Watchdog Control
 * =============================================================================
 */

/**
 * wdg_enable_show - Read watchdog enable state
 */
static ssize_t wdg_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 val;
	int ret;

	ret = psu_i2c_read_byte(data->client, BOCO_WDG_ENABLE_ADDR, &val);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%u\n", val);
}

/**
 * wdg_enable_store - Write watchdog enable state
 */
static ssize_t wdg_enable_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val > 1)
		return -EINVAL;

	ret = psu_i2c_write_byte(data->client, BOCO_WDG_ENABLE_ADDR, (u8)val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(wdg_enable);

/**
 * wdg_time_show - Read watchdog timeout in seconds
 */
static ssize_t wdg_time_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, BOCO_SET_WDG_TIME_ADDR, &val);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%u\n", val);
}

/**
 * wdg_time_store - Write watchdog timeout in seconds
 */
static ssize_t wdg_time_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val > 65535)
		return -EINVAL;

	ret = psu_i2c_write_word(data->client, BOCO_SET_WDG_TIME_ADDR, (u16)val);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(wdg_time);

/**
 * wdg_status_show - Read watchdog status (0 = wake, 1 = sleep)
 */
static ssize_t wdg_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u8 val;
	int ret;

	ret = psu_i2c_read_byte(data->client, BOCO_WDG_EN_STATUS_ADDR, &val);
	if (ret < 0)
		return ret;

	/* 0x00 = wake, 0x01 = sleep */
	return sprintf(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(wdg_status);

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - OTP Temperature Thresholds
 *
 * Reads OTP (Over Temperature Protection) threshold limits from
 * MFR-specific registers (0xC0-0xC2). Linear-11 encoded.
 * Returns values in millidegrees Celsius.
 * =============================================================================
 */

/**
 * max_temp_ambient_show - Read OTP ambient (T1) temperature threshold
 */
static ssize_t max_temp_ambient_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, BOCO_MFR_MAX_TEMP_1_ADDR, &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}
static DEVICE_ATTR_RO(max_temp_ambient);

/**
 * max_temp_sr_show - Read OTP SR (T2) temperature threshold
 */
static ssize_t max_temp_sr_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, BOCO_MFR_MAX_TEMP_2_ADDR, &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}
static DEVICE_ATTR_RO(max_temp_sr);

/**
 * max_temp_pfc_show - Read OTP PFC (T3) temperature threshold
 */
static ssize_t max_temp_pfc_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));
	u16 val;
	int ret;

	ret = psu_i2c_read_word(data->client, BOCO_MFR_MAX_TEMP_3_ADDR, &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));
}
static DEVICE_ATTR_RO(max_temp_pfc);

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - Per-Input V/I/P
 *
 * Reads per-input voltage (mV), current (mA), and power (mW) from MFR-specific
 * registers 0xE0-0xE8. All values are Linear-11 encoded.
 * =============================================================================
 */

#define BOCO_LINEAR11_ATTR_RO(name, reg_addr)					\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr, char *buf)		\
{									\
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));	\
	u16 val;							\
	int ret;							\
	ret = psu_i2c_read_word(data->client, reg_addr, &val);		\
	if (ret < 0)							\
		return ret;						\
	return sysfs_emit(buf, "%d\n", pmbus_linear11_to_milli(val));	\
}									\
static DEVICE_ATTR_RO(name)

BOCO_LINEAR11_ATTR_RO(vin1, BOCO_READ_VIN1_ADDR);
BOCO_LINEAR11_ATTR_RO(vin2, BOCO_READ_VIN2_ADDR);
BOCO_LINEAR11_ATTR_RO(vin3, BOCO_READ_VIN3_ADDR);
BOCO_LINEAR11_ATTR_RO(iin1, BOCO_READ_IIN1_ADDR);
BOCO_LINEAR11_ATTR_RO(iin2, BOCO_READ_IIN2_ADDR);
BOCO_LINEAR11_ATTR_RO(iin3, BOCO_READ_IIN3_ADDR);
BOCO_LINEAR11_ATTR_RO(pin1, BOCO_READ_PIN1_ADDR);
BOCO_LINEAR11_ATTR_RO(pin2, BOCO_READ_PIN2_ADDR);
BOCO_LINEAR11_ATTR_RO(pin3, BOCO_READ_PIN3_ADDR);

/*
 * =============================================================================
 * Vendor-Specific Sysfs Attributes - VSB Status
 *
 * Reads VSB (standby) output voltage/current status from MFR-specific
 * registers 0xEB-0xEC (From datasheet V1.3+). Exposes raw status byte as hex.
 * =============================================================================
 */

#define BOCO_STATUS_BYTE_ATTR_RO(name, reg_addr)				\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr, char *buf)		\
{									\
	struct psu_data *data = i2c_get_clientdata(to_i2c_client(dev));	\
	u8 val;								\
	int ret;							\
	ret = psu_i2c_read_byte(data->client, reg_addr, &val);		\
	if (ret < 0)							\
		return ret;						\
	return sysfs_emit(buf, "0x%02x\n", val);			\
}									\
static DEVICE_ATTR_RO(name)

BOCO_STATUS_BYTE_ATTR_RO(status_vsb_vout, BOCO_STATUS_VSB_VOUT_ADDR);
BOCO_STATUS_BYTE_ATTR_RO(status_vsb_iout, BOCO_STATUS_VSB_IOUT_ADDR);

/* BOCO vendor attribute group */
static struct attribute *boco_vendor_attrs[] = {
	&dev_attr_app_fw_revision.attr,
	&dev_attr_bl_fw_revision.attr,
	&dev_attr_wdg_enable.attr,
	&dev_attr_wdg_time.attr,
	&dev_attr_wdg_status.attr,
	&dev_attr_max_temp_ambient.attr,
	&dev_attr_max_temp_sr.attr,
	&dev_attr_max_temp_pfc.attr,
	/* per-input voltage (mV) */
	&dev_attr_vin1.attr,
	&dev_attr_vin2.attr,
	&dev_attr_vin3.attr,
	/* per-input current (mA) */
	&dev_attr_iin1.attr,
	&dev_attr_iin2.attr,
	&dev_attr_iin3.attr,
	/* per-input power (mW) */
	&dev_attr_pin1.attr,
	&dev_attr_pin2.attr,
	&dev_attr_pin3.attr,
	/* VSB status (raw byte) */
	&dev_attr_status_vsb_vout.attr,
	&dev_attr_status_vsb_iout.attr,
	NULL
};

static const struct attribute_group boco_vendor_attr_group = {
	.name = "boco",  /* Creates /sys/bus/i2c/devices/X-00YY/boco/ subdirectory */
	.attrs = boco_vendor_attrs,
};

const struct attribute_group *boco_common_vendor_attr_groups[] = {
	&boco_vendor_attr_group,
	NULL
};

MODULE_DESCRIPTION("BOCO PSU Common Functions");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
