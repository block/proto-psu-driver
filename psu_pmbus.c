// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Kernel Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This driver provides support for PMBus-compatible power supplies.
 * It exposes sensor data through the Linux hwmon subsystem and provides
 * control interfaces through custom sysfs attributes.
 *
 * Supported PSU models:
 * - Chicony S24 / S4K0E001P (R2, 4kW)
 * - BOCO BS402A17 (R2, 4kW)
 * - BOCO BS502A17 (R2, 5kW)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>

#include "psu_pmbus.h"
#include "psu_pmbus_platform.h"

#define DRIVER_NAME "psu-pmbus"
#define DRIVER_VERSION "1.0"

#define MFR_MODEL_BOCO_BS402A17		"BS402A17"
#define MFR_MODEL_BOCO_BS502A17		"BS502A17"
#define MFR_MODEL_CHICONY_S24		"S4K0E001P"

/* Module parameters */
static unsigned int update_interval = PSU_UPDATE_INTERVAL_MS;
module_param(update_interval, uint, 0644);
MODULE_PARM_DESC(update_interval, "Full measurement update interval in milliseconds");

static unsigned int status_check_interval = PSU_STATUS_CHECK_INTERVAL_MS;
module_param(status_check_interval, uint, 0644);
MODULE_PARM_DESC(status_check_interval, "Fast status word check interval in milliseconds");

bool debug = false;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output");

/* Global list of all PSU instances for panic handler */
static LIST_HEAD(psu_list);
static DEFINE_SPINLOCK(psu_list_lock);

/*
 * =============================================================================
 * I2C Communication Functions with Retry Logic
 * =============================================================================
 */

#define I2C_INTER_CMD_DELAY_MS	  2       /* Delay after successful I2C operation */

/**
 * psu_i2c_set_pec - Set I2C client PEC (Packet Error Checking) state
 * @client: I2C client device
 * @enable: true to enable PEC, false to disable
 */
static inline void psu_i2c_set_pec(struct i2c_client *client, bool enable)
{
	if (enable)
		client->flags |= I2C_CLIENT_PEC;
	else
		client->flags &= ~I2C_CLIENT_PEC;
}

/**
 * psu_handle_enxio_disconnect - Handle ENXIO error indicating physical disconnect
 * @data: PSU driver data
 *
 * Called when an I2C operation returns ENXIO. This indicates the PSU has been
 * physically disconnected. We immediately disable the GPIO and request removal
 * to prevent the disconnected PSU from jamming the I2C bus.
 *
 * IMPORTANT: This is called on the FIRST ENXIO - no retries needed since the
 * device is physically gone.
 */
static void psu_handle_enxio_disconnect(struct psu_data *data)
{
	if (data->state == PSU_STATE_DISCONNECTED)
		return;  /* Already handled */

	psu_err(data, "PSU physical disconnection detected (ENXIO) - triggering immediate removal\n");
	data->state = PSU_STATE_DISCONNECTED;
	data->consecutive_i2c_failures = data->disconnect_threshold;

	/* Disable GPIO IMMEDIATELY to release I2C bus */
	if (data->psu_en_gpio) {
		psu_warn(data, "Disabling PSU GPIO immediately to release I2C bus\n");
		psu_set_enable_gpio(data, false);
	}

	psu_child_request_removal(data->parent_platform);
}

/* Generic I2C operation with retry, PEC handling, and ENXIO detection */
static int psu_i2c_with_retry(struct i2c_client *client, enum psu_i2c_op_type op,
			      struct psu_i2c_op_ctx *ctx)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret, retries = 0;
	int final_ret;
	bool old_pec, use_pec = false;
	bool is_write = psu_i2c_op_is_write(op);
	bool is_block = (op == PSU_I2C_OP_READ_BLOCK);

	if (data->state == PSU_STATE_DISCONNECTED)
		return -ENODEV;

	/* Determine PEC setting for this command */
	if (data->ops && data->ops->use_pec_for_command)
		use_pec = data->ops->use_pec_for_command(ctx->reg, is_write, is_block);

	old_pec = (client->flags & I2C_CLIENT_PEC) != 0;
	psu_i2c_set_pec(client, use_pec);

	for (retries = 0; retries < I2C_MAX_RETRIES; retries++) {
		switch (op) {
		case PSU_I2C_OP_READ_BYTE:
			ret = i2c_smbus_read_byte_data(client, ctx->reg);
			if (ret >= 0)
				*ctx->byte_val = (u8)ret;
			break;
		case PSU_I2C_OP_READ_WORD:
			ret = i2c_smbus_read_word_data(client, ctx->reg);
			if (ret >= 0)
				*ctx->word_val = (u16)ret;
			break;
		case PSU_I2C_OP_READ_BLOCK:
			ret = i2c_smbus_read_i2c_block_data(client, ctx->reg,
							    ctx->block.len, ctx->block.buf);
			break;
		case PSU_I2C_OP_WRITE_BYTE:
			ret = i2c_smbus_write_byte_data(client, ctx->reg, ctx->write_byte);
			break;
		case PSU_I2C_OP_WRITE_WORD:
			ret = i2c_smbus_write_word_data(client, ctx->reg, ctx->write_word);
			break;
		case PSU_I2C_OP_SEND_BYTE:
			ret = i2c_smbus_write_byte(client, ctx->reg);
			break;
		default:
			psu_i2c_set_pec(client, old_pec);
			final_ret = -EINVAL;
			goto trace_and_return;
		}

		/* Success: block returns byte count (>0), others return 0 */
		if (is_block ? (ret > 0) : (ret >= 0)) {
			atomic_long_inc(&data->i2c_total_count);
			atomic_long_add(retries, &data->i2c_retry_count);
			psu_i2c_set_pec(client, old_pec);
			msleep(I2C_INTER_CMD_DELAY_MS);
			final_ret = is_block ? ret : 0;
			goto trace_and_return;
		}

		/* ENXIO after retry confirms physical disconnect */
		if (ret == -ENXIO && retries > 0) {
			atomic_long_inc(&data->i2c_total_count);
			atomic_long_inc(&data->i2c_failure_count);
			atomic_long_add(retries, &data->i2c_retry_count);
			psu_i2c_set_pec(client, old_pec);
			psu_handle_enxio_disconnect(data);
			final_ret = ret;
			goto trace_and_return;
		}

		if (retries < I2C_MAX_RETRIES - 1)
			msleep(I2C_RETRY_DELAY_MS);
	}

	/* All retries exhausted */
	atomic_long_inc(&data->i2c_total_count);
	atomic_long_inc(&data->i2c_failure_count);
	atomic_long_add(I2C_MAX_RETRIES - 1, &data->i2c_retry_count);
	psu_i2c_set_pec(client, old_pec);
	final_ret = ret;
	retries = I2C_MAX_RETRIES - 1;

trace_and_return:
	psu_trace_xfer(data, op, ctx, final_ret, retries, use_pec);
	return final_ret;
}

static int psu_i2c_read_byte_with_retry(struct i2c_client *client, u8 reg, u8 *val)
{
	struct psu_i2c_op_ctx ctx = { .reg = reg, .byte_val = val };
	return psu_i2c_with_retry(client, PSU_I2C_OP_READ_BYTE, &ctx);
}

static int psu_i2c_read_word_with_retry(struct i2c_client *client, u8 reg, u16 *val)
{
	struct psu_i2c_op_ctx ctx = { .reg = reg, .word_val = val };
	return psu_i2c_with_retry(client, PSU_I2C_OP_READ_WORD, &ctx);
}

static int psu_i2c_read_block_with_retry(struct i2c_client *client, u8 reg,
					 u8 *buf, size_t len)
{
	struct psu_i2c_op_ctx ctx = { .reg = reg, .block = { .buf = buf, .len = len } };
	return psu_i2c_with_retry(client, PSU_I2C_OP_READ_BLOCK, &ctx);
}

static int psu_i2c_write_byte_with_retry(struct i2c_client *client, u8 reg, u8 val)
{
	struct psu_i2c_op_ctx ctx = { .reg = reg, .write_byte = val };
	return psu_i2c_with_retry(client, PSU_I2C_OP_WRITE_BYTE, &ctx);
}

static int psu_i2c_write_word_with_retry(struct i2c_client *client, u8 reg, u16 val)
{
	struct psu_i2c_op_ctx ctx = { .reg = reg, .write_word = val };
	return psu_i2c_with_retry(client, PSU_I2C_OP_WRITE_WORD, &ctx);
}

static int psu_i2c_send_byte_with_retry(struct i2c_client *client, u8 command)
{
	struct psu_i2c_op_ctx ctx = { .reg = command };
	return psu_i2c_with_retry(client, PSU_I2C_OP_SEND_BYTE, &ctx);
}

/* Public I2C wrappers with locking */
int psu_i2c_read_byte(struct i2c_client *client, u8 reg, u8 *val)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_read_byte_with_retry(client, reg, val);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

int psu_i2c_read_word(struct i2c_client *client, u8 reg, u16 *val)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_read_word_with_retry(client, reg, val);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

int psu_i2c_read_block(struct i2c_client *client, u8 reg, u8 *buf, size_t len)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_read_block_with_retry(client, reg, buf, len);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

int psu_i2c_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_write_byte_with_retry(client, reg, val);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

int psu_i2c_write_word(struct i2c_client *client, u8 reg, u16 val)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_write_word_with_retry(client, reg, val);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

int psu_i2c_send_byte(struct i2c_client *client, u8 command)
{
	struct psu_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = psu_i2c_send_byte_with_retry(client, command);
	mutex_unlock(&data->i2c_lock);
	return ret;
}

/*
 * =============================================================================
 * GPIO Control Functions
 * =============================================================================
 */

/**
 * psu_set_enable_gpio - Control PSU_EN GPIO
 * @data: PSU driver data
 * @enable: true to enable PSU, false to disable
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Note: GPIO is active-low (LOW = enabled, HIGH = disabled)
 * The gpiod API handles polarity based on device tree GPIO_ACTIVE_LOW flag
 */
int psu_set_enable_gpio(struct psu_data *data, bool enable)
{
	int ret;
	u8 readback;

	if (!data->psu_en_gpio) {
		psu_warn(data, "PSU enable GPIO not configured\n");
		return -ENODEV;
	}

	psu_dbg(data, "Setting PSU_EN GPIO to %s\n",
		enable ? "enabled" : "disabled");

	/* Set GPIO */
	gpiod_set_value_cansleep(data->psu_en_gpio, enable ? 1 : 0);

	/* Wait for GPIO to settle */
	msleep(PSU_EN_ASSERT_HOLD_MS);

	/* Verify GPIO state */
	ret = gpiod_get_value_cansleep(data->psu_en_gpio);
	readback = (ret > 0) ? 1 : 0;

	if (readback == (enable ? 1 : 0)) {
		return 0;
	} else {
		psu_err(data, "GPIO readback mismatch, expected %d but got %d (ret=%d)\n",
			enable ? 1 : 0, readback, ret);
		return -EIO;
	}
}
EXPORT_SYMBOL_GPL(psu_set_enable_gpio);

/*
 * =============================================================================
 * PMBus Protocol Functions - See psu-pmbus-protocol.c
 * =============================================================================
 */

/*
 * =============================================================================
 * PSU Type Detection
 * =============================================================================
 */

enum psu_type psu_detect_type(const char *model)
{
	if (strnstr(model, MFR_MODEL_CHICONY_S24, PMBUS_NAME_SIZE))
		return PSU_TYPE_CHICONY_S24;
	else if (strnstr(model, MFR_MODEL_BOCO_BS402A17, PMBUS_NAME_SIZE))
		return PSU_TYPE_BOCO_BS402A17;
	else if (strnstr(model, MFR_MODEL_BOCO_BS502A17, PMBUS_NAME_SIZE))
		return PSU_TYPE_BOCO_BS502A17;

	return PSU_TYPE_UNKNOWN;
}

const char *psu_type_name(enum psu_type type)
{
	switch (type) {
	case PSU_TYPE_CHICONY_S24:	return "chicony_s24";
	case PSU_TYPE_BOCO_BS402A17:	return "boco_bs402a17";
	case PSU_TYPE_BOCO_BS502A17:	return "boco_bs502a17";
	case PSU_TYPE_CHICONY_UNKNOWN:	return "chicony_unknown";
	case PSU_TYPE_BOCO_UNKNOWN:	return "boco_unknown";
	default:			return "unknown";
	}
}
EXPORT_SYMBOL_GPL(psu_type_name);

/*
 * =============================================================================
 * Device-Specific Operations Registration
 * =============================================================================
 */

/* External device-specific operations from separate files */
extern struct psu_ops chicony_s24_ops;
extern struct psu_ops boco_bs402a17_ops;
extern struct psu_ops boco_bs502a17_ops;

/**
 * psu_get_ops - Get device-specific operations for PSU type
 * @type: PSU type enumeration
 *
 * Returns: Pointer to psu_ops structure, or NULL if no device-specific ops
 */
struct psu_ops *psu_get_ops(enum psu_type type)
{
	switch (type) {
	case PSU_TYPE_CHICONY_S24:
		return &chicony_s24_ops;
	case PSU_TYPE_BOCO_BS402A17:
		return &boco_bs402a17_ops;
	case PSU_TYPE_BOCO_BS502A17:
		return &boco_bs502a17_ops;
	default:
		return NULL;  /* Generic PSU, no special ops */
	}
}


/*
 * =============================================================================
 * Workqueues for Periodic Updates
 * =============================================================================
 */

/**
 * psu_update_work_handler - Workqueue handler for periodic updates
 * @work: Work structure
 */
static void psu_update_work_handler(struct work_struct *work)
{
	struct psu_data *data = container_of(to_delayed_work(work),
					     struct psu_data, update_work);
	int ret;

	/* Check if we're disconnected BEFORE trying to read */
	if (data->state == PSU_STATE_DISCONNECTED) {
		psu_dbg(data, "PSU disconnected - stopping measurement updates\n");
		return;  /* Don't reschedule */
	}

	/* Update measurements */
	ret = psu_update_measurements(data);
	if (ret < 0) {
		psu_err(data, "Failed to update measurements: %d\n", ret);
		/* Check again if we became disconnected during measurements */
		if (data->state == PSU_STATE_DISCONNECTED) {
			psu_dbg(data, "PSU disconnected during measurements - stopping updates\n");
			return;  /* Don't reschedule */
		}
	}

	/* Schedule next update only if still connected */
	if (data->state != PSU_STATE_DISCONNECTED) {
		schedule_delayed_work(&data->update_work,
				      msecs_to_jiffies(data->config.update_interval_ms));
	}
}

/**
 * psu_status_check_work_handler - Workqueue handler for fast status checks
 * @work: Work structure
 *
 * This handler runs more frequently than the full measurement update
 * to provide quick fault detection and response.
 */
static void psu_status_check_work_handler(struct work_struct *work)
{
	struct psu_data *data = container_of(to_delayed_work(work),
					     struct psu_data, status_check_work);
	int ret;
	u32 cached_vin;

	/* Perform fast status check
	 * STATUS_WORD is the primary indicator for PSU presence:
	 * - It's a mandatory PMBus register at address 0x79
	 * - PSUs must always respond to STATUS_WORD reads when powered
	 * - Single register read minimizes false positives
	 * - Fast polling (250ms) ensures quick disconnection detection
	 */
	ret = psu_fast_status_check(data);
	if (ret < 0) {
		/* ENXIO (-6) means device physically disconnected - immediate removal
		 * This is critical for PSUs (like Chicony) that hold the I2C bus
		 * after unplug while their capacitors drain. Immediate GPIO disable
		 * accelerates capacitor drain from 40+ seconds to ~2.5 seconds.
		 */
		if (ret == -ENXIO) {
			psu_handle_enxio_disconnect(data);
			return;  /* Don't reschedule */
		}

		/* VIN-based disconnect detection: If we had a valid VIN reading before and
		 * VIN is now low/zero, this indicates PSU power loss. Trigger immediate removal.
		 * This catches cases where Chicony PSU goes straight to ETIMEDOUT without ENXIO.
		 *
		 * VIN threshold: 50V (50,000 mV) - way below normal operation (180-264V AC)
		 * Prevents false positives while catching genuine power loss.
		 *
		 * Uses atomic VIN cache for lock-free access - no mutex needed. This prevents
		 * blocking on update_lock when measurement update is stuck on I2C retries.
		 *
		 * U32_MAX sentinel means VIN was never read successfully - don't trigger on that.
		 */
		cached_vin = atomic_read(&data->cached_vin_mv);
		psu_dbg(data, "Cached VIN: %u mV\n", cached_vin);
		if (cached_vin != U32_MAX && cached_vin < 50000) {
			psu_err(data, "PSU power loss detected (VIN=%u mV) with I2C failure (err=%d) - triggering immediate removal\n",
				cached_vin, ret);
			data->state = PSU_STATE_DISCONNECTED;
			data->consecutive_i2c_failures = data->disconnect_threshold; /* For statistics */

			/* Disable GPIO IMMEDIATELY to force capacitor drain - don't wait for removal
			 * This is CRITICAL: psu_remove() takes 20+ seconds to run because it has to
			 * cancel workqueues that are stuck on I2C timeouts. By disabling GPIO now,
			 * the bus recovers in ~2.5s instead of 40+ seconds.
			 */
			if (data->psu_en_gpio) {
				psu_warn(data, "Disabling PSU GPIO immediately to release I2C bus\n");
				psu_set_enable_gpio(data, false);
			}

			/* Request removal from platform driver */
			psu_child_request_removal(data->parent_platform);
			return;  /* Don't reschedule */
		}

		/* Other I2C errors: use threshold-based detection for transient failures */
		data->consecutive_i2c_failures++;
		if (debug) {
			psu_info(data, "STATUS_WORD read failed: %d (consecutive failures: %u)\n",
				 ret, data->consecutive_i2c_failures);
		}

		/* Check if we've exceeded the disconnection threshold */
		if (data->consecutive_i2c_failures >= data->disconnect_threshold) {
			if (data->state != PSU_STATE_DISCONNECTED) {
				psu_err(data, "PSU disconnected after %u consecutive STATUS_WORD read failures\n",
					data->consecutive_i2c_failures);
				data->state = PSU_STATE_DISCONNECTED;

				/* Disable GPIO IMMEDIATELY to release I2C bus - don't wait for psu_remove() */
				if (data->psu_en_gpio) {
					psu_warn(data, "Disabling PSU GPIO immediately to release I2C bus\n");
					psu_set_enable_gpio(data, false);
				}

				/* Request removal from platform driver */
				psu_child_request_removal(data->parent_platform);
				return;  /* Don't reschedule */
			}
		}
	} else {
		/* Reset failure counter on successful read */
		if (data->consecutive_i2c_failures > 0) {
			psu_dbg(data, "STATUS_WORD read OK (clearing %u failures)\n",
				data->consecutive_i2c_failures);
			psu_info(data, "PSU I2C communication restored\n");
		}
		data->consecutive_i2c_failures = 0;
	}

	/* Schedule next status check - keep normal rate to ensure fast detection */
	schedule_delayed_work(&data->status_check_work,
			      msecs_to_jiffies(data->config.status_check_interval_ms));
}

/*
 * =============================================================================
 * PSU Initialization Functions
 * =============================================================================
 *
 * Note: hwmon integration code is in psu-hwmon.c
 */

/**
 * psu_init_device - Initialize PSU device
 * @data: PSU driver data
 *
 * Returns: 0 on success, negative error code on failure
 */
int psu_init_device(struct psu_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	/* Read manufacturer information */
	ret = psu_read_manufacturer_info(client, &data->info);
	if (ret < 0) {
		psu_err(data, "Failed to read manufacturer info: %d\n", ret);
		return ret;
	}

	/* Get device-specific operations */
	data->ops = psu_get_ops(data->info.type);

	/*
	 * Call device-specific initialization
	 * This handles PEC configuration, fixed exponents, and other quirks
	 */
	if (data->ops && data->ops->init) {
		psu_dbg(data, "Initializing device-specific driver: %s\n",
			data->ops->name);
		ret = data->ops->init(client, data);
		if (ret < 0) {
			psu_err(data, "Device-specific init failed: %d\n", ret);
			return ret;
		}
	} else {
		psu_dbg(data, "No device-specific driver, using generic PMBus\n");
	}

	/* Read VOUT_MODE to get exponent (unless device uses fixed exponent) */
	if (data->ops && data->ops->skip_vout_mode) {
		/* Device-specific init already set the exponent */
		psu_dbg(data, "Using fixed VOUT exponent: %d\n", data->vout_exponent);
	} else {
		ret = psu_read_vout_mode(client, &data->vout_exponent);
		if (ret < 0) {
			psu_err(data, "Failed to read VOUT_MODE: %d\n", ret);
			/* Use device-specific fixed exponent as fallback */
			if (data->ops && data->ops->fixed_vout_exponent != -1) {
				psu_dbg(data, "Using fallback fixed exponent: %d\n",
					data->ops->fixed_vout_exponent);
				data->vout_exponent = data->ops->fixed_vout_exponent;
			} else {
				return ret;
			}
		}
	}

	/* Read PSU hardware limits from MFR_* registers (0xA0-0xA7) */
	ret = psu_read_limits(client, &data->limits);
	if (ret < 0) {
		psu_warn(data, "Failed to read PSU limits: %d\n", ret);
		/* Non-fatal, continue */
	}

	/* Ensure PSU starts in disabled state (OPERATION = OFF) */
	ret = psu_set_enable(client, false);
	if (ret < 0) {
		psu_warn(data, "Failed to set OPERATION to OFF: %d\n", ret);
		/* Continue anyway - this is not fatal */
	}
	data->state = PSU_STATE_DISABLED;
	psu_dbg(data, "PSU OPERATION set to OFF\n");

	/* Configure output voltage from device tree */
	if (data->config.initial_voltage_mv > 0) {
		ret = psu_set_output_voltage(client, data->config.initial_voltage_mv,
					     data->vout_exponent);
		if (ret < 0) {
			psu_err(data, "Failed to set initial voltage to %u mV: %d\n",
				data->config.initial_voltage_mv, ret);
			return ret;
		}
		data->setpoint_mv = data->config.initial_voltage_mv;
		psu_dbg(data, "Set output voltage to %u mV from device tree\n",
			data->config.initial_voltage_mv);
	} else {
		psu_warn(data, "No initial voltage specified in device tree\n");
	}

	/* Clear any faults that occurred during initialization (e.g., from block reads) */
	psu_dbg(data, "Clearing PSU faults after initialization...\n");
	ret = psu_clear_faults(client);
	if (ret < 0) {
		psu_warn(data, "Failed to clear faults: %d (continuing anyway)\n", ret);
		/* Non-fatal, continue */
	} else {
		psu_dbg(data, "PSU faults cleared successfully\n");
	}

	/* Reset fault counter */
	data->fault_count = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(psu_init_device);

/*
 * =============================================================================
 * Probe and Remove Functions
 * =============================================================================
 */

/**
 * psu_probe - I2C device probe function
 * @client: I2C client device
 * @id: I2C device ID
 *
 * Returns: 0 on success, negative error code on failure
 */
#ifdef PSU_RPI_BUILD
static int psu_probe(struct i2c_client *client)
#else
static int psu_probe(struct i2c_client *client, const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct psu_data *data;
	int ret;
	int gpio_state;

	dev_info(dev, "Probing PSU at I2C address 0x%02x\n", client->addr);

	/* Check if adapter supports required operations.
	 * We need byte and word data plus block transfers. Accept either native
	 * SMBus block data or I2C-level block operations (which the kernel's
	 * SMBus emulation layer can use to perform SMBus block transfers).
	 * BCM2835 (RPi) lacks native SMBus Block Read but has I2C Block Read.
	 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(dev, "Adapter doesn't support required I2C operations\n");
		return -ENODEV;
	}
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BLOCK_DATA) &&
	    !i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(dev, "Adapter supports neither SMBus block nor I2C block operations\n");
		return -ENODEV;
	}

	/* Allocate driver data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);

	/* Initialize mutexes */
	mutex_init(&data->update_lock);
	mutex_init(&data->i2c_lock);

	/* Initialize atomic VIN cache to U32_MAX (sentinel for "never valid") */
	atomic_set(&data->cached_vin_mv, U32_MAX);

	/* Add to global PSU list for panic handler */
	spin_lock(&psu_list_lock);
	list_add(&data->list, &psu_list);
	spin_unlock(&psu_list_lock);

	/* Get configuration from platform_data (provided by parent platform driver) */
	if (!dev->platform_data) {
		dev_err(dev, "Missing platform_data - this driver must be instantiated by the platform driver\n");
		ret = -EINVAL;
		goto err_remove_from_list;
	}

	{
		struct psu_platform_data *pdata = dev->platform_data;

		data->psu_id = pdata->psu_id;
		data->config.initial_voltage_mv = pdata->initial_voltage_mv;
		data->config.update_interval_ms = pdata->update_interval_ms;
		data->config.status_check_interval_ms = pdata->status_check_interval_ms;
		data->psu_en_gpio = pdata->psu_en_gpio;
		data->disconnect_threshold = pdata->disconnect_threshold;
		data->parent_platform = pdata->parent_platform;
		data->trace_level = pdata->parent_platform ?
			pdata->parent_platform->trace_level : 0;

		if (data->psu_en_gpio) {
			gpio_state = gpiod_get_value_cansleep(data->psu_en_gpio);
			psu_info(data, "PSU enable GPIO provided by parent (state=%s)\n", gpio_state ? "enabled" : "disabled");
		} else {
			psu_warn(data, "No PSU enable GPIO provided by parent\n");
		}
	}

	/* Set defaults if not specified */
	if (data->config.update_interval_ms == 0)
		data->config.update_interval_ms = update_interval;
	if (data->config.status_check_interval_ms == 0)
		data->config.status_check_interval_ms = status_check_interval;
	if (data->disconnect_threshold == 0)
		data->disconnect_threshold = 3;

	/* Defense-in-depth: check bootloader mode in case PSU transitioned
	 * after the platform driver's pre-creation check */
	{
		u8 bootloader_status;
		ret = psu_i2c_read_byte(client, PMBUS_MFR_FWUPLOAD_MODE_ADDR, &bootloader_status);
		if (ret == 0) {
			if (bootloader_status == PMBUS_FWUPLOAD_MODE_BOOTLOADER_VAL) {
				psu_info(data, "PSU is in bootloader mode\n");
				ret = -ENODEV;
				goto err_remove_from_list;
			}
		}
	}

	/* Initialize PSU device */
	ret = psu_init_device(data);
	if (ret < 0) {
		psu_err(data, "Failed to initialize PSU: %d\n", ret);
		goto err_remove_from_list;
	}

	/* Report exact PSU type to platform driver */
	psu_child_report_type(data->parent_platform, data->info.type);

	/* Mark measurements as invalid initially */
	data->measurements.valid = false;

	/* Initialize workqueues for periodic updates and fast status checks */
	INIT_DELAYED_WORK(&data->update_work, psu_update_work_handler);
	INIT_DELAYED_WORK(&data->status_check_work, psu_status_check_work_handler);

	/* Do an initial measurement update */
	ret = psu_update_measurements(data);
	if (ret < 0) {
		psu_warn(data, "Initial measurement update failed: %d\n", ret);
		/* Non-fatal, continue */
	}

	/* Do an initial status check */
	ret = psu_fast_status_check(data);
	if (ret < 0) {
		psu_warn(data, "Initial status check failed: %d\n", ret);
		/* Non-fatal, continue */
	}

	/* Enable PSU via GPIO after successful initialization
	 * This ensures GPIO is only enabled when PSU is confirmed functional.
	 * On first boot: GPIO already enabled by platform driver (GPIOD_OUT_HIGH)
	 * On reconnect: GPIO was disabled during disconnect, re-enable it as needed
	 */
	if (data->psu_en_gpio) {
		gpio_state = gpiod_get_value_cansleep(data->psu_en_gpio);
		if (gpio_state == 0) {
			ret = psu_set_enable_gpio(data, true);
			if (ret < 0) {
				psu_err(data, "Failed to enable PSU GPIO: %d\n", ret);
				goto err_remove_from_list;
			}
		}
	}

	/* Start periodic updates */
	psu_dbg(data, "Starting periodic updates every %u ms\n",
		data->config.update_interval_ms);
	schedule_delayed_work(&data->update_work,
			      msecs_to_jiffies(data->config.update_interval_ms));

	/* Start fast status checks */
	psu_dbg(data, "Starting fast status checks every %u ms\n",
		data->config.status_check_interval_ms);
	schedule_delayed_work(&data->status_check_work,
			      msecs_to_jiffies(data->config.status_check_interval_ms));

	/* Register with hwmon subsystem */
	ret = psu_hwmon_register(data);
	if (ret < 0) {
		psu_err(data, "Failed to register hwmon device: %d\n", ret);
		cancel_delayed_work_sync(&data->update_work);
		cancel_delayed_work_sync(&data->status_check_work);
		goto err_remove_from_list;
	}

	/* Create sysfs control interface */
	ret = psu_sysfs_create(data);
	if (ret < 0) {
		psu_err(data, "Failed to create sysfs attributes: %d\n", ret);
		psu_hwmon_unregister(data);
		cancel_delayed_work_sync(&data->update_work);
		cancel_delayed_work_sync(&data->status_check_work);
		goto err_remove_from_list;
	}

	/* Sync cached setpoint with PSU hardware */
	if (psu_get_output_voltage_setpoint(client, &data->setpoint_mv) < 0) {
		data->setpoint_mv = 0;  /* Will force first voltage write */
	}

	psu_info(data, "PSU probe successful\n");

	return 0;

err_remove_from_list:
	/* Remove from global PSU list */
	spin_lock(&psu_list_lock);
	list_del(&data->list);
	spin_unlock(&psu_list_lock);
	return ret;
}

/**
 * psu_remove - I2C device remove function
 * @client: I2C client device
 *
 * Returns: 0 on success (RPI), void (non-RPI)
 */
#ifdef PSU_RPI_BUILD
static void psu_remove(struct i2c_client *client)
#else
static int psu_remove(struct i2c_client *client)
#endif
{
	struct psu_data *data = i2c_get_clientdata(client);

	psu_info(data, "Removing PSU driver\n");

	/* Remove from global PSU list */
	spin_lock(&psu_list_lock);
	list_del(&data->list);
	spin_unlock(&psu_list_lock);

	/* Remove sysfs control interface */
	psu_sysfs_remove(data);

	/* Unregister from hwmon subsystem */
	psu_hwmon_unregister(data);

	/* Cancel any pending work */
	cancel_delayed_work_sync(&data->update_work);
	cancel_delayed_work_sync(&data->status_check_work);

	/* Disable PSU via GPIO when driver unloads */
	if (data->psu_en_gpio) {
		psu_info(data, "Disabling PSU via GPIO on driver removal\n");
		psu_set_enable_gpio(data, false);
	}

#ifndef PSU_RPI_BUILD
	return 0;
#endif
}

/*
 * =============================================================================
 * I2C Driver Structure
 * =============================================================================
 */

/* Device ID table for I2C devices */
static const struct i2c_device_id psu_pmbus_id[] = {
	{ "psu-pmbus", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, psu_pmbus_id);

/* Device tree match table */
static const struct of_device_id psu_pmbus_of_match[] = {
	{ .compatible = "block,psu-pmbus" },
	{ }
};
MODULE_DEVICE_TABLE(of, psu_pmbus_of_match);

/* I2C driver structure */
struct i2c_driver psu_pmbus_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = psu_pmbus_of_match,
	},
	.probe = psu_probe,
	.remove = psu_remove,
	.id_table = psu_pmbus_id,
};

/*
 * =============================================================================
 * Panic Notifier for Safe Shutdown
 * =============================================================================
 */

/**
 * psu_panic_notifier - Disable all PSUs on kernel panic
 * @nb: Notifier block
 * @event: Panic event
 * @data: Unused
 *
 * Returns: NOTIFY_DONE
 *
 * This function is called during kernel panic to safely disable all PSUs
 * by setting their enable GPIOs to HIGH (off). This prevents potential
 * damage from uncontrolled PSU operation during system crash.
 */
static int psu_panic_notifier(struct notifier_block *nb,
			      unsigned long event, void *data)
{
	struct psu_data *psu;
	unsigned long flags;

	pr_emerg("PSU PMBus: Kernel panic detected, disabling all PSUs\n");

	/* Iterate through all registered PSUs and disable them */
	spin_lock_irqsave(&psu_list_lock, flags);
	list_for_each_entry(psu, &psu_list, list) {
		/* Disable PSU via GPIO if available */
		if (psu->psu_en_gpio) {
			/* Deassert PSU_EN GPIO (disable the PSU) */
			gpiod_set_value(psu->psu_en_gpio, 0);  /* false = disabled */

			/* Only access model name if it appears valid */
			if (psu->info.model[0] != '\0') {
				pr_emerg("PSU PMBus: Disabled PSU %s via GPIO\n",
					 psu->info.model);
			} else {
				pr_emerg("PSU PMBus: Disabled PSU via GPIO\n");
			}
		}
	}
	spin_unlock_irqrestore(&psu_list_lock, flags);

	return NOTIFY_DONE;
}

struct notifier_block psu_panic_nb = {
	.notifier_call = psu_panic_notifier,
	.priority = 0,  /* Default priority */
};
EXPORT_SYMBOL_GPL(psu_panic_nb);
