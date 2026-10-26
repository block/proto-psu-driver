// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Platform Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This driver probes multiple I2C addresses for PSU presence and dynamically
 * creates child PSU-PMBus devices for each detected PSU. It reads configuration
 * from device tree and passes it to each child instance.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/panic_notifier.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/delay.h>

#include "psu_pmbus.h"
#include "psu_pmbus_platform.h"
#include "psu_pmbus_protocol.h"
#include <libpsu/vendors/chicony.h>
#include <libpsu/vendors/boco.h>
#include "psu_trace.h"

#define DRIVER_NAME "psu-pmbus-platform"

/* Track if panic notifier was successfully registered */
static bool panic_notifier_registered;

/**
 * psu_type_from_address - Infer vendor-level PSU type from I2C address
 * @addr: 7-bit I2C address where PSU was detected
 *
 * When PSU is in bootloader mode, we cannot read MFR_MODEL. Map the I2C
 * address to a vendor-level type (without model distinction).
 */
static enum psu_type psu_type_from_address(u8 addr)
{
	switch (addr) {
	case CHICONY_PRIMARY_I2C_ADDR:
	case CHICONY_SECONDARY_I2C_ADDR:
		return PSU_TYPE_CHICONY_UNKNOWN;
	case BOCO_I2C_ADDR:
		return PSU_TYPE_BOCO_UNKNOWN;
	default:
		return PSU_TYPE_UNKNOWN;
	}
}

/**
 * psu_platform_parse_dt - Parse device tree properties
 * @pdev: Platform device
 * @platform: Platform driver data structure
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_platform_parse_dt(struct platform_device *pdev,
			       struct psu_platform_driver_data *platform)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *i2c_node;
	u32 val;
	int ret;
	int gpio_state;

	/* Parse PSU ID (required) */
	ret = of_property_read_u32(np, "psu-id", &val);
	if (ret) {
		dev_err(dev, "Missing 'psu-id' property\n");
		return -EINVAL;
	}
	platform->pdata.psu_id = val;
	dev_info(dev, "PSU ID: %u\n", platform->pdata.psu_id);

	/* Get I2C adapter from phandle */
	i2c_node = of_parse_phandle(np, "i2c-bus", 0);
	if (!i2c_node) {
		dev_err(dev, "Missing 'i2c-bus' property\n");
		return -EINVAL;
	}

	platform->adapter = of_get_i2c_adapter_by_node(i2c_node);
	of_node_put(i2c_node);

	if (!platform->adapter) {
		dev_err(dev, "Failed to get I2C adapter\n");
		return -EPROBE_DEFER;
	}

	dev_info(dev, "Using I2C adapter: %s (bus %d)\n",
		 platform->adapter->name, platform->adapter->nr);

	/* Parse initial voltage (required) */
	ret = of_property_read_u32(np, "initial-voltage-mv", &val);
	if (ret) {
		dev_err(dev, "Missing 'initial-voltage-mv' property\n");
		goto err_put_adapter;
	}
	platform->pdata.initial_voltage_mv = val;

	/* Parse update interval (optional, use default if not specified) */
	ret = of_property_read_u32(np, "update-interval-ms", &val);
	if (ret == 0) {
		platform->pdata.update_interval_ms = val;
	} else {
		platform->pdata.update_interval_ms = PSU_UPDATE_INTERVAL_MS;
	}

	/* Parse status check interval (optional, use default if not specified) */
	ret = of_property_read_u32(np, "status-check-interval-ms", &val);
	if (ret == 0) {
		platform->pdata.status_check_interval_ms = val;
	} else {
		platform->pdata.status_check_interval_ms = PSU_STATUS_CHECK_INTERVAL_MS;
	}

	/* Get GPIO descriptor for PSU enable
	 * Note: gpiod API expects property name without "-gpio" suffix
	 * Device tree has "psu-en-gpio" but we request "psu-en"
	 *
	 * Initialize GPIO LOW (disabled) at boot - child probe will enable after confirming PSU status
	 * This prevents missing or unstable PSUs from affecting I2C bus during initial probe
	 */
	platform->pdata.psu_en_gpio = devm_gpiod_get_optional(dev, "psu-en", GPIOD_OUT_LOW);
	if (IS_ERR(platform->pdata.psu_en_gpio)) {
		ret = PTR_ERR(platform->pdata.psu_en_gpio);
		dev_err(dev, "Failed to get PSU enable GPIO: %d (check if psu-en-gpio property exists in device tree)\n", ret);
		goto err_put_adapter;
	}

	if (platform->pdata.psu_en_gpio) {
		gpio_state = gpiod_get_value_cansleep(platform->pdata.psu_en_gpio);
		dev_info(dev, "PSU enable GPIO acquired successfully (state: %s)\n",
			 gpio_state ? "enabled" : "disabled");
	} else {
		dev_info(dev, "No PSU enable GPIO configured (psu-en-gpio property not found)\n");
	}

	/* Read hot-plug configuration */
	ret = of_property_read_u32(np, "reconnect-poll-interval-ms", &val);
	if (!ret) {
		platform->poll_interval_ms = val;
	}

	ret = of_property_read_u32(np, "disconnect-threshold", &val);
	if (!ret) {
		platform->pdata.disconnect_threshold = val;
	} else {
		platform->pdata.disconnect_threshold = 3; /* Default */
	}

	dev_info(dev, "Configuration: initial_voltage=%u mV, update_interval=%u ms, "
		 "status_check_interval=%u ms, gpio=%s, poll_interval=%u ms, "
		 "disconnect_threshold=%u\n",
		 platform->pdata.initial_voltage_mv,
		 platform->pdata.update_interval_ms,
		 platform->pdata.status_check_interval_ms,
		 platform->pdata.psu_en_gpio ? "present" : "none",
		 platform->poll_interval_ms,
		 platform->pdata.disconnect_threshold);

	return 0;

err_put_adapter:
	i2c_put_adapter(platform->adapter);
	return ret;
}

static void psu_platform_trace_probe(struct psu_platform_driver_data *platform,
				     u8 addr, int ret)
{
	u8 probe_data;

	if (platform->trace_level == 0)
		return;
	/* Level 1 only records errors/retries/status changes/writes.
	 * Successful probes are routine polling traffic, so skip them.
	 */
	if (platform->trace_level == 1 && ret >= 0)
		return;

	probe_data = (ret >= 0) ? (u8)ret : 0;
	trace_psu_pmbus_xfer(platform->pdata.psu_id, addr,
			     PMBUS_MFR_FWUPLOAD_MODE_ADDR,
			     PSU_I2C_OP_READ_BYTE, &probe_data,
			     ret >= 0 ? 1 : 0,
			     ret >= 0 ? 0 : (s32)ret,
			     0, 0, 0);
}

/**
 * psu_platform_probe_and_create_child - Probe an I2C address and create child device
 * @platform: Parent driver data
 * @addr: I2C address to probe
 *
 * Returns: Pointer to created I2C client on success, NULL on failure
 */
static struct i2c_client *psu_platform_probe_and_create_child(
	struct psu_platform_driver_data *platform, u8 addr)
{
	struct device *dev = &platform->pdev->dev;
	struct i2c_board_info board_info;
	struct i2c_client *client;
	struct psu_platform_data *pdata;
	int ret;

	/* Probe the address to see if a PSU is present */
	ret = psu_i2c_probe_address(platform->adapter, addr);
	psu_platform_trace_probe(platform, addr, ret);

	if (ret < 0) {
		dev_dbg(dev, "No PSU found at address 0x%02x\n", addr);
		return NULL;
	}

	/* Check if PSU is in bootloader mode before creating child */
	if (ret == PMBUS_FWUPLOAD_MODE_BOOTLOADER_VAL) {
		dev_info(dev, "PSU at 0x%02x is in bootloader mode\n", addr);
		return ERR_PTR(-EBUSY);
	}

	dev_info(dev, "PSU detected at address 0x%02x\n", addr);

	/* Allocate platform data for this child
	 * Note: Use kmemdup() instead of devm_kmemdup() because the platform data
	 * needs to be freed during hot-plug removal cycles. Using devm_kmemdup()
	 * with the parent device would cause memory leaks since the parent lives
	 * longer than the child devices.
	 */
	pdata = kmemdup(&platform->pdata, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "Failed to allocate platform data for PSU at 0x%02x\n", addr);
		return NULL;
	}

	/* Store reference to parent platform for callbacks */
	pdata->parent_platform = platform;

	dev_info(dev, "Creating child device at address 0x%02x with GPIO %s\n",
		 addr, pdata->psu_en_gpio ? "present" : "absent");

	/* Initialize board info for child device */
	memset(&board_info, 0, sizeof(board_info));
	strscpy(board_info.type, "psu-pmbus", I2C_NAME_SIZE);
	board_info.addr = addr;
	board_info.platform_data = pdata;

	/* Create the I2C client device */
	client = i2c_new_client_device(platform->adapter, &board_info);
	if (IS_ERR(client)) {
		dev_err(dev, "Failed to create I2C device at 0x%02x: %ld\n",
			addr, PTR_ERR(client));
		kfree(pdata);
		return NULL;
	}

	/* If driver didn't bind, clean up and check why. Re-read D6 to
	 * distinguish a bootloader race from an ordinary probe failure */
	if (!client->dev.driver) {
		dev_warn(dev, "PSU driver failed to bind at 0x%02x\n", addr);
		pdata = client->dev.platform_data;
		i2c_unregister_device(client);
		kfree(pdata);

		ret = psu_i2c_probe_address(platform->adapter, addr);
		psu_platform_trace_probe(platform, addr, ret);

		if (ret == PMBUS_FWUPLOAD_MODE_BOOTLOADER_VAL)
			return ERR_PTR(-EBUSY);
		return NULL;
	}

	dev_info(dev, "Created PSU child device at I2C address 0x%02x\n", addr);
	return client;
}


/**
 * psu_address_poll_work - Workqueue handler for polling disconnected PSU address
 * @work: Work struct embedded in poll_state
 *
 * Polls a disconnected PSU address to detect reconnection. When a PSU is
 * detected, creates a new child device and stops polling.
 */
static void psu_address_poll_work(struct work_struct *work)
{
	struct psu_poll_state *state = container_of(to_delayed_work(work),
						    struct psu_poll_state,
						    poll_work);
	struct psu_platform_driver_data *platform = state->platform;
	struct device *dev = &platform->pdev->dev;
	struct i2c_client *client = NULL;
	int i;

	/* Don't poll during firmware update mode */
	if (platform->in_update_mode)
		return;

	/* Check if polling is still needed (lock-free atomic read) */
	if (!atomic_read(&state->polling_needed))
		return;

	/* Try to detect PSU at any of the probe addresses */
	for (i = 0; i < platform->num_probe_addresses; i++) {
		u8 addr = platform->probe_addresses[i];

		client = psu_platform_probe_and_create_child(platform, addr);
		if (IS_ERR(client)) {
			/* PSU in bootloader — enter update mode, stop polling */
			atomic_set(&state->polling_needed, 0);

			mutex_lock(&platform->lock);
			platform->in_update_mode = true;
			platform->detected_address = addr;
			platform->detected_psu_type = psu_type_from_address(addr);
			mutex_unlock(&platform->lock);

			dev_warn(dev, "PSU at 0x%02x in bootloader - entering update mode\n", addr);
			return;
		}
		if (client) {
			/* PSU detected and child created */
			dev_info(dev, "PSU reconnected at address 0x%02x\n", addr);

			/* Stop polling - PSU is back (lock-free atomic write) */
			atomic_set(&state->polling_needed, 0);

			mutex_lock(&platform->lock);
			platform->child_client = client;
			platform->detected_address = addr;
			mutex_unlock(&platform->lock);

			return;
		}
	}

	/* No PSU found - keep polling all addresses (lock-free atomic read) */
	if (atomic_read(&state->polling_needed)) {
		schedule_delayed_work(&state->poll_work,
				     msecs_to_jiffies(platform->poll_interval_ms));
	}
}

/**
 * psu_deferred_removal_work - Deferred work handler for child removal
 * @work: Work structure
 *
 * This work runs on the system workqueue to remove the child device.
 */
static void psu_deferred_removal_work(struct work_struct *work)
{
	struct psu_platform_driver_data *platform = container_of(work,
						struct psu_platform_driver_data,
						removal_work);
	struct device *dev = &platform->pdev->dev;
	struct i2c_client *client_to_remove;
	void *pdata_to_free;
	u8 address;

	mutex_lock(&platform->lock);
	client_to_remove = platform->child_client;
	platform->child_client = NULL;
	platform->detected_psu_type = PSU_TYPE_UNKNOWN;
	mutex_unlock(&platform->lock);

	if (client_to_remove) {
		address = client_to_remove->addr;
		/* Save platform_data pointer before unregistering (it gets cleared) */
		pdata_to_free = client_to_remove->dev.platform_data;
		i2c_unregister_device(client_to_remove);
		/* Free the platform data we allocated in psu_platform_probe_and_create_child() */
		kfree(pdata_to_free);
		dev_info(dev, "PSU at 0x%02x removed, polling for reconnection\n", address);

		/* Start polling all probe addresses for reconnection (lock-free atomic write) */
		atomic_set(&platform->poll_state.polling_needed, 1);

		schedule_delayed_work(&platform->poll_state.poll_work,
				     msecs_to_jiffies(platform->poll_interval_ms));
	}
}

/**
 * psu_child_request_removal - Child reports it's disconnected
 * @parent_platform: Pointer to parent platform driver data
 *
 * Called by child driver when it detects disconnection via consecutive
 * I2C failures. We schedule deferred work to actually remove the child
 * to avoid deadlock (can't cancel a workqueue from within that workqueue).
 */
void psu_child_request_removal(void *parent_platform)
{
	struct psu_platform_driver_data *platform = parent_platform;

	if (!platform)
		return;

	/* Ignore during update mode - disconnection is expected */
	if (platform->in_update_mode)
		return;

	schedule_work(&platform->removal_work);
}
EXPORT_SYMBOL_GPL(psu_child_request_removal);

/**
 * psu_child_report_type - Child reports its exact PSU type after detection
 * @parent_platform: Pointer to parent platform driver data
 * @type: Exact PSU type determined from MFR_MODEL
 */
void psu_child_report_type(void *parent_platform, enum psu_type type)
{
	struct psu_platform_driver_data *platform = parent_platform;

	if (!platform)
		return;

	mutex_lock(&platform->lock);
	platform->detected_psu_type = type;
	mutex_unlock(&platform->lock);
}
EXPORT_SYMBOL_GPL(psu_child_report_type);

/**
 * state_show - Show current PSU connection state
 * @dev: Device
 * @attr: Device attribute
 * @buf: Output buffer
 *
 * Returns: Number of bytes written
 */
static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	const char *state;

	mutex_lock(&platform->lock);
	if (platform->in_update_mode) {
		state = "update_mode";
	} else if (platform->child_client) {
		state = "connected";
	} else {
		state = "disconnected";
	}
	mutex_unlock(&platform->lock);

	return sprintf(buf, "%s\n", state);
}

/**
 * update_mode_show - Show current update mode status
 * @dev: Device
 * @attr: Device attribute
 * @buf: Output buffer
 *
 * Returns: Number of bytes written
 */
static ssize_t update_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", platform->in_update_mode ? 1 : 0);
}

/**
 * update_mode_store - Set update mode for firmware updates
 * @dev: Device
 * @attr: Device attribute
 * @buf: Input buffer
 * @count: Number of bytes
 *
 * When set to 1, unbinds the child device to allow userspace I2C access.
 * When set to 0, recreates the child device after firmware update.
 *
 * Returns: Number of bytes consumed or negative error
 */
static ssize_t update_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	void *pdata_to_free = NULL;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable && !platform->in_update_mode) {
		/* Entering update mode - unbind child to free I2C address */
		mutex_lock(&platform->lock);

		if (!platform->child_client) {
			mutex_unlock(&platform->lock);
			dev_info(dev, "No PSU child present to unbind\n");
			return count;
		}

		dev_info(dev, "Entering update mode at 0x%02x\n",
			 platform->detected_address);

		mutex_unlock(&platform->lock);

		/* Stop any reconnection polling and pending removals (lock-free atomic write) */
		atomic_set(&platform->poll_state.polling_needed, 0);

		/* Cancel workqueues without holding mutex to avoid deadlock */
		cancel_delayed_work_sync(&platform->poll_state.poll_work);
		cancel_work_sync(&platform->removal_work);

		mutex_lock(&platform->lock);
		/* Unregister child device */
		if (platform->child_client) {
			/* Save platform_data pointer before unregistering */
			pdata_to_free = platform->child_client->dev.platform_data;
			i2c_unregister_device(platform->child_client);
			platform->child_client = NULL;
		}
		platform->in_update_mode = true;
		mutex_unlock(&platform->lock);

		/* Free platform data outside of lock */
		kfree(pdata_to_free);

	} else if (!enable && platform->in_update_mode) {
		struct i2c_client *client;

		/* Exiting update mode - recreate child */
		dev_info(dev, "Exiting update mode at 0x%02x\n",
			 platform->detected_address);

		/* Clear update mode before child creation so that disconnect
		 * detection during probe is not silently dropped */
		mutex_lock(&platform->lock);
		platform->in_update_mode = false;
		mutex_unlock(&platform->lock);

		/* Create child outside mutex to avoid deadlock — child probe
		 * runs synchronously inside i2c_new_client_device() */
		client = psu_platform_probe_and_create_child(platform,
							platform->detected_address);
		if (IS_ERR(client)) {
			/* ERR_PTR(-EBUSY) only returned when D6 confirms bootloader.
			 * Disconnected/comms failures return NULL and fall through
			 * to the polling path below. */
			mutex_lock(&platform->lock);
			platform->in_update_mode = true;
			mutex_unlock(&platform->lock);
			dev_warn(dev, "PSU still in bootloader after update attempt\n");
			return -EBUSY;
		}

		mutex_lock(&platform->lock);
		platform->child_client = client;

		if (!client) {
			platform->detected_psu_type = PSU_TYPE_UNKNOWN;
			dev_err(dev, "Failed to recreate child after update\n");
			/* Start polling all probe addresses to detect PSU (lock-free atomic write) */
			atomic_set(&platform->poll_state.polling_needed, 1);
			schedule_delayed_work(&platform->poll_state.poll_work,
					     msecs_to_jiffies(platform->poll_interval_ms));
		}
		mutex_unlock(&platform->lock);
	}

	return count;
}

/**
 * psu_id_show - Show PSU ID
 * @dev: Device
 * @attr: Device attribute
 * @buf: Output buffer
 *
 * Returns: Number of bytes written
 */
static ssize_t psu_id_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	return sprintf(buf, "%u\n", platform->pdata.psu_id);
}

/**
 * psu_gpio_enable_show - Show PSU_EN GPIO state
 * @dev: Device
 * @attr: Device attribute
 * @buf: Output buffer
 *
 * Reads the PSU_EN GPIO directly from the platform driver data.
 * This attribute is available even when the I2C child is unregistered
 * (e.g., during firmware update mode).
 *
 * Returns: Number of bytes written, or negative error
 */
static ssize_t psu_gpio_enable_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	int value;

	if (!platform->pdata.psu_en_gpio)
		return -ENODEV;

	value = gpiod_get_value_cansleep(platform->pdata.psu_en_gpio);
	if (value < 0)
		return value;

	return sysfs_emit(buf, "%d\n", value);
}

/**
 * psu_gpio_enable_store - Set PSU_EN GPIO state
 * @dev: Device
 * @attr: Device attribute
 * @buf: Input buffer
 * @count: Number of bytes
 *
 * Directly controls the PSU_EN GPIO from the platform driver.
 * Available even when the I2C child is unregistered.
 *
 * Returns: Number of bytes consumed or negative error
 */
static ssize_t psu_gpio_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (!platform->pdata.psu_en_gpio)
		return -ENODEV;

	ret = kstrtoul(buf, 10, &enable);
	if (ret < 0)
		return ret;

	if (enable > 1)
		return -EINVAL;

	gpiod_set_value_cansleep(platform->pdata.psu_en_gpio,
				 enable ? 1 : 0);

	dev_dbg(dev, "PSU_EN GPIO set to %s via platform sysfs\n",
		enable ? "HIGH (enabled)" : "LOW (disabled)");

	return count;
}

static ssize_t psu_type_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	enum psu_type type;

	mutex_lock(&platform->lock);
	type = platform->detected_psu_type;
	mutex_unlock(&platform->lock);

	return sysfs_emit(buf, "%s\n", psu_type_name(type));
}

static DEVICE_ATTR_RO(state);
static DEVICE_ATTR_RW(update_mode);
static DEVICE_ATTR_RO(psu_id);
static DEVICE_ATTR_RW(psu_gpio_enable);
static DEVICE_ATTR_RO(psu_type);

/**
 * trace_level - Get/set PMBus I2C transaction trace level
 *
 * Controls ftrace recording of PMBus I2C transactions for FA triage.
 * 0 = off (default), 1 = errors/retries/status changes/writes, 2 = all
 *
 * Tracing is opt-in to avoid spamming the global ftrace ring buffer on
 * empty-slot systems where polling never finds a PSU. Can be set before
 * a PSU is connected so that probe and init transactions are captured
 * when the PSU appears.
 */
static ssize_t trace_level_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", platform->trace_level);
}

static ssize_t trace_level_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct psu_platform_driver_data *platform = dev_get_drvdata(dev);
	unsigned int level;
	int ret;

	ret = kstrtouint(buf, 10, &level);
	if (ret < 0)
		return ret;

	if (level > 2)
		return -EINVAL;

	platform->trace_level = level;

	/* Propagate to child if connected */
	mutex_lock(&platform->lock);
	if (platform->child_client) {
		struct psu_data *data = i2c_get_clientdata(platform->child_client);
		data->trace_level = level;
	}
	mutex_unlock(&platform->lock);

	return count;
}
static DEVICE_ATTR_RW(trace_level);

static struct attribute *psu_platform_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_update_mode.attr,
	&dev_attr_psu_id.attr,
	&dev_attr_psu_gpio_enable.attr,
	&dev_attr_psu_type.attr,
	&dev_attr_trace_level.attr,
	NULL
};
ATTRIBUTE_GROUPS(psu_platform);

/**
 * psu_platform_probe - Platform driver probe function
 * @pdev: Platform device
 *
 * Returns: 0 on success, negative error code on failure
 */
static int psu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct psu_platform_driver_data *platform;
	struct device_node *np = dev->of_node;
	u8 probe_addresses[MAX_PSU_ADDRESSES];
	int num_addresses;
	int i;
	int ret;

	dev_info(dev, "PSU PMBus platform driver probing\n");

	/* Allocate platform driver data */
	platform = devm_kzalloc(dev, sizeof(*platform), GFP_KERNEL);
	if (!platform)
		return -ENOMEM;

	platform->pdev = pdev;
	platform_set_drvdata(pdev, platform);

	/* Initialize hot-plug support */
	mutex_init(&platform->lock);
	platform->poll_interval_ms = 5000; /* Default 5s polling */
	platform->poll_state.platform = platform;
	atomic_set(&platform->poll_state.polling_needed, 0);
	INIT_DELAYED_WORK(&platform->poll_state.poll_work, psu_address_poll_work);
	INIT_WORK(&platform->removal_work, psu_deferred_removal_work);

	/* Parse device tree properties */
	ret = psu_platform_parse_dt(pdev, platform);
	if (ret) {
		dev_err(dev, "Failed to parse device tree: %d\n", ret);
		return ret;
	}

	/* Read probe-addresses array from device tree */
	num_addresses = of_property_count_u8_elems(np, "probe-addresses");
	if (num_addresses <= 0) {
		dev_err(dev, "Missing or empty 'probe-addresses' property\n");
		ret = -EINVAL;
		goto err_put_adapter;
	}

	if (num_addresses > MAX_PSU_ADDRESSES) {
		dev_warn(dev, "Too many probe addresses (%d), limiting to %d\n",
			 num_addresses, MAX_PSU_ADDRESSES);
		num_addresses = MAX_PSU_ADDRESSES;
	}

	ret = of_property_read_u8_array(np, "probe-addresses",
					probe_addresses, num_addresses);
	if (ret) {
		dev_err(dev, "Failed to read probe-addresses: %d\n", ret);
		goto err_put_adapter;
	}

	/* Store probe addresses for polling */
	memcpy(platform->probe_addresses, probe_addresses, num_addresses);
	platform->num_probe_addresses = num_addresses;

	/* Probe each address and create child device */
	dev_info(dev, "Probing %d I2C addresses for PSUs\n", num_addresses);
	for (i = 0; i < num_addresses; i++) {
		struct i2c_client *client;

		client = psu_platform_probe_and_create_child(platform, probe_addresses[i]);
		if (IS_ERR(client)) {
			/* PSU in bootloader mode - enter update mode */
			platform->in_update_mode = true;
			platform->detected_address = probe_addresses[i];
			platform->detected_psu_type = psu_type_from_address(probe_addresses[i]);
			dev_warn(dev, "PSU at 0x%02x in bootloader - entering update mode\n",
				 probe_addresses[i]);
			break;
		}
		if (client) {
			platform->child_client = client;
			platform->detected_address = probe_addresses[i];
			break; /* Only create ONE child */
		}
	}

	if (platform->in_update_mode) {
		/* Bootloader mode detected — already logged, nothing else to do */
	} else if (!platform->child_client) {
		dev_warn(dev, "No PSUs detected at any of the specified addresses\n");
		/* Start polling all probe addresses - PSU might appear later */
		atomic_set(&platform->poll_state.polling_needed, 1);
		schedule_delayed_work(&platform->poll_state.poll_work,
				     msecs_to_jiffies(platform->poll_interval_ms));
		dev_info(dev, "Starting polling for PSU at addresses: ");
		for (i = 0; i < platform->num_probe_addresses; i++) {
			pr_cont("0x%02x%s", platform->probe_addresses[i],
				i < platform->num_probe_addresses - 1 ? ", " : "\n");
		}
	} else {
		dev_info(dev, "Successfully initialized PSU at address 0x%02x\n",
			 platform->detected_address);
	}

	return 0;

err_put_adapter:
	i2c_put_adapter(platform->adapter);
	return ret;
}

/**
 * psu_platform_remove - Platform driver remove function
 * @pdev: Platform device
 *
 * Returns: 0 on success (RPI), void (non-RPI)
 */
#ifdef PSU_RPI_BUILD
static void psu_platform_remove(struct platform_device *pdev)
#else
static int psu_platform_remove(struct platform_device *pdev)
#endif
{
	struct psu_platform_driver_data *platform = platform_get_drvdata(pdev);
	struct i2c_client *client_to_remove;
	void *pdata_to_free = NULL;

	/* Cancel any pending workqueues */
	atomic_set(&platform->poll_state.polling_needed, 0);
	cancel_delayed_work_sync(&platform->poll_state.poll_work);
	cancel_work_sync(&platform->removal_work);

	/* Unregister child I2C device if present
	 * IMPORTANT: We must NOT hold platform->lock during i2c_unregister_device()
	 * because the child's remove function will cancel workqueues, and those
	 * workqueues may be trying to acquire platform->lock, causing deadlock.
	 */
	mutex_lock(&platform->lock);
	client_to_remove = platform->child_client;
	platform->child_client = NULL;  /* Clear pointer before unlocking */
	mutex_unlock(&platform->lock);

	if (client_to_remove) {
		dev_info(&pdev->dev, "Removing PSU at address 0x%02x\n",
			 client_to_remove->addr);
		/* Save platform_data pointer before unregistering */
		pdata_to_free = client_to_remove->dev.platform_data;
		i2c_unregister_device(client_to_remove);
		/* Free platform data allocated in psu_platform_probe_and_create_child() */
		kfree(pdata_to_free);
	}

	/* Release I2C adapter reference */
	if (platform->adapter)
		i2c_put_adapter(platform->adapter);

#ifndef PSU_RPI_BUILD
	return 0;
#endif
}

static const struct of_device_id psu_platform_of_match[] = {
	{ .compatible = "block,psu-pmbus" },
	{ }
};
MODULE_DEVICE_TABLE(of, psu_platform_of_match);

static struct platform_driver psu_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = psu_platform_of_match,
		.dev_groups = psu_platform_groups,  /* Register sysfs attributes */
	},
	.probe = psu_platform_probe,
	.remove = psu_platform_remove,
};

/* Forward declaration from psu_pmbus.c */
extern struct i2c_driver psu_pmbus_driver;

/**
 * psu_pmbus_module_init - Module initialization
 *
 * Registers both the I2C child driver and the platform driver.
 * The I2C driver must be registered first so it's available when the
 * platform driver creates child I2C devices during probe.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init psu_pmbus_module_init(void)
{
	int ret;

	/* Register panic notifier first - must be active before any PSU probes
	 * Note: This is optional - if registration fails, driver continues without
	 * panic shutdown capability. panic_notifier_list may not be available
	 * on all kernel configurations.
	 */
	ret = atomic_notifier_chain_register(&panic_notifier_list, &psu_panic_nb);
	if (ret) {
		pr_warn("psu-pmbus: Failed to register panic notifier: %d (continuing without panic shutdown)\n", ret);
		panic_notifier_registered = false;
	} else {
		panic_notifier_registered = true;
	}

	/* Register I2C driver (child) */
	ret = i2c_add_driver(&psu_pmbus_driver);
	if (ret) {
		pr_err("psu-pmbus: Failed to register I2C driver: %d\n", ret);
		if (panic_notifier_registered)
			atomic_notifier_chain_unregister(&panic_notifier_list, &psu_panic_nb);
		return ret;
	}

	/* Register platform driver */
	pr_info("psu-pmbus: Registering platform driver for compatible=\"%s\"\n",
		psu_platform_of_match[0].compatible);
	ret = platform_driver_register(&psu_platform_driver);
	if (ret) {
		pr_err("psu-pmbus: Failed to register platform driver: %d\n", ret);
		i2c_del_driver(&psu_pmbus_driver);
		if (panic_notifier_registered)
			atomic_notifier_chain_unregister(&panic_notifier_list, &psu_panic_nb);
		return ret;
	}

	pr_info("psu-pmbus: Module initialized (version %s) - waiting for platform devices\n",
		PSU_PMBUS_VERSION);
	return 0;
}

/**
 * psu_pmbus_module_exit - Module cleanup
 *
 * Unregisters drivers in reverse order of registration.
 */
static void __exit psu_pmbus_module_exit(void)
{
	/* Unregister in reverse order of init */
	platform_driver_unregister(&psu_platform_driver);
	i2c_del_driver(&psu_pmbus_driver);
	if (panic_notifier_registered)
		atomic_notifier_chain_unregister(&panic_notifier_list, &psu_panic_nb);
	pr_info("psu-pmbus: Module unloaded\n");
}

module_init(psu_pmbus_module_init);
module_exit(psu_pmbus_module_exit);

MODULE_DESCRIPTION("PSU PMBus Parent Driver");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_VERSION(PSU_PMBUS_VERSION);
MODULE_LICENSE("GPL");
