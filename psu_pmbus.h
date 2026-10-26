/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PSU PMBus Kernel Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This driver provides support for PMBus-compatible power supplies,
 * exposing sensor data through the hwmon subsystem and control
 * interfaces through custom sysfs attributes.
 */

#ifndef PSU_PMBUS_H
#define PSU_PMBUS_H

#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>
#include <linux/atomic.h>
#include "psu_pmbus_protocol.h"

/*
 * PSU Types - Identified by MFR_MODEL string
 */
enum psu_type {
	PSU_TYPE_UNKNOWN = 0,
	PSU_TYPE_CHICONY_S24,
	PSU_TYPE_BOCO_BS402A17,
	PSU_TYPE_BOCO_BS502A17,
	PSU_TYPE_CHICONY_UNKNOWN,	/* address-inferred, bootloader mode */
	PSU_TYPE_BOCO_UNKNOWN,		/* address-inferred, bootloader mode */
};

/*
 * PSU State Machine
 */
enum psu_state {
	PSU_STATE_DISABLED = 0,
	PSU_STATE_ENABLED,
	PSU_STATE_ERROR,
	PSU_STATE_DISCONNECTED,  /* PSU not responding on I2C */
};

/*
 * I2C Timing Constants
 */
#define I2C_RETRY_DELAY_MS	  30      /* PMBus retry delay - matches Rust code */
#define I2C_MAX_RETRIES		  3       /* Max I2C retries for transient errors */

/*
 * I2C Operation Types for generic retry handler
 */
enum psu_i2c_op_type {
	PSU_I2C_OP_READ_BYTE,
	PSU_I2C_OP_READ_WORD,
	PSU_I2C_OP_READ_BLOCK,
	PSU_I2C_OP_WRITE_BYTE,
	PSU_I2C_OP_WRITE_WORD,
	PSU_I2C_OP_SEND_BYTE,   /* SMBus Send Byte: command code only, no data byte */
};

static inline bool psu_i2c_op_is_write(enum psu_i2c_op_type op)
{
	return op == PSU_I2C_OP_WRITE_BYTE ||
	       op == PSU_I2C_OP_WRITE_WORD ||
	       op == PSU_I2C_OP_SEND_BYTE;
}

/*
 * I2C Operation Context - holds parameters for any I2C operation
 */
struct psu_i2c_op_ctx {
	u8 reg;
	union {
		u8 *byte_val;		/* For read_byte */
		u16 *word_val;		/* For read_word */
		struct {		/* For read_block */
			u8 *buf;
			size_t len;
		} block;
		u8 write_byte;		/* For write_byte */
		u16 write_word;		/* For write_word */
	};
};

/*
 * Temperature Sensor Counts
 *
 * Standard PMBus defines 3 temperature registers (0x8D, 0x8E, 0x8F).
 * Manufacturer-specific temperatures can be added via psu_ops callbacks.
 */
#define PSU_TEMP_STD_SENSORS	3	/* Standard PMBus: 0x8D, 0x8E, 0x8F */
#define PSU_TEMP_MAX_SENSORS	8	/* Max supported (3 std + up to 5 MFR-specific) */

/*
 * Fault Types
 */
enum psu_fault_type {
	PSU_FAULT_NONE = 0,
	PSU_FAULT_OVER_VOLTAGE,
	PSU_FAULT_UNDER_VOLTAGE,
	PSU_FAULT_OVER_CURRENT,
	PSU_FAULT_OVER_POWER,
	PSU_FAULT_OVER_TEMPERATURE,
	PSU_FAULT_UNDER_TEMPERATURE,
	PSU_FAULT_INPUT,
	PSU_FAULT_OUTPUT,
	PSU_FAULT_FANS,
	PSU_FAULT_COMMS,
	PSU_FAULT_POWER_GOOD,
	PSU_FAULT_OFF,
	PSU_FAULT_BUSY,
	PSU_FAULT_UNKNOWN,
};

/*
 * Fault Level
 */
enum psu_fault_level {
	PSU_FAULT_LEVEL_NONE = 0,
	PSU_FAULT_LEVEL_WARNING,
	PSU_FAULT_LEVEL_FAULT,
};

/*
 * PSU Device Information
 */
struct psu_device_info {
	char vendor[PMBUS_NAME_SIZE];
	char model[PMBUS_NAME_SIZE];
	char serial[PMBUS_NAME_SIZE];
	char revision[PMBUS_NAME_SIZE];
	char location[PMBUS_NAME_SIZE];
	char date[PMBUS_NAME_SIZE];
	char firmware_version[PMBUS_NAME_SIZE];
	enum psu_type type;
};

/*
 * PSU Measurement Cache
 *
 * All values cached to avoid excessive I2C traffic.
 * Updated periodically by workqueue.
 */
struct psu_measurements {
	/* Voltages (mV) */
	u32 vin;
	u32 vout;

	/* Currents (mA) */
	u32 iin;
	u32 iout;

	/* Power (mW) */
	u32 pin;
	u32 pout;

	/* Temperatures (mC - millidegrees Celsius) */
	s32 temp[PSU_TEMP_MAX_SENSORS];

	/* Fan speeds (RPM) */
	u32 fan_speed[4];
	u8 fan_count;

	/* Status */
	u16 status_word;
	u8 status_vout;
	u8 status_iout;
	u8 status_input;
	u8 status_temp;
	u8 status_cml;
	u8 status_other;
	u8 status_mfr_specific;
	u8 status_fans;     /* STATUS_FANS_1_2 register */
	u8 status_fans_34;  /* STATUS_FANS_3_4 register */

	u16 prev_status_word;             /* Fast-poll change detection */
	u16 prev_classified_status_word;  /* psu_parse_faults change detection */

	/* Timestamp */
	unsigned long last_update;  /* jiffies */
	bool valid;
};

/*
 * PSU Limits (from PSU hardware)
 */
struct psu_limits {
	u32 vout_min_mv;
	u32 vout_max_mv;
	u32 iout_max_ma;
	u32 pout_max_mw;
	u32 vin_min_mv;
	u32 vin_max_mv;
	u32 iin_max_ma;
	u32 pin_max_mw;
};

/*
 * PSU Configuration (from device tree)
 */
struct psu_config {
	u32 initial_voltage_mv;
	u32 update_interval_ms;       /* Full measurement update interval */
	u32 status_check_interval_ms; /* Fast status word check interval */
};

/* Forward declaration for psu_ops callbacks */
struct psu_data;

/*
 * PSU-specific operations
 *
 * Different PSU models require different quirks and behaviors.
 * This structure allows device-specific drivers to customize behavior
 * while sharing the core PMBus implementation.
 */
struct psu_ops {
	/* Device name for logging */
	const char *name;

	/* Initialization - called during probe after device info read */
	int (*init)(struct i2c_client *client, struct psu_data *data);

	/* PEC (Packet Error Checking) control per-command */
	bool (*use_pec_for_command)(u8 command, bool is_write, bool is_block);

	/* Feature flags */
	u8 fan_count;            /* Number of fans (0 = no fan support) */
	bool skip_vout_mode;     /* Don't read VOUT_MODE, use fixed exponent */
	s8 fixed_vout_exponent;  /* Fixed exponent if skip_vout_mode=true */
	bool supports_mfr_specific; /* PSU supports STATUS_MFR_SPECIFIC (0x80) */

	/* OPERATION command values (device-specific) */
	u8 operation_on_value;   /* Value to write to OPERATION for ON (default: 0x80) */
	u8 operation_off_value;  /* Value to write to OPERATION for OFF (default: 0x00) */

	/* Post-initialization (optional) */
	int (*post_init)(struct i2c_client *client, struct psu_data *data);

	/* Custom limits (optional) - if PSU doesn't support standard MFR_* commands */
	int (*get_limits)(struct i2c_client *client, struct psu_limits *limits);

	/* Temperature sensor label (optional) - return sensor name for hwmon */
	const char *(*get_temp_label)(unsigned int sensor_index);

	/* MFR-specific temperature sensors (optional) */
	u8 mfr_temp_sensor_count;  /* Number of MFR-specific temp sensors */

	/* Read MFR-specific temperature (optional) - return millidegrees Celsius */
	int (*read_mfr_temp)(struct i2c_client *client, int sensor_index, s32 *temp_mc);

	/* Vendor-specific sysfs attribute groups (optional) */
	const struct attribute_group **vendor_attr_groups;
};

/*
 * Main PSU Driver Data Structure
 */
struct psu_data {
	struct i2c_client *client;
	struct device *hwmon_dev;
	struct mutex update_lock;  /* Protects measurements */
	struct mutex i2c_lock;     /* Protects I2C bus access */
	struct delayed_work update_work;        /* Full measurement updates */
	struct delayed_work status_check_work;  /* Fast status word checks */
	struct list_head list;                  /* For global PSU list (panic handler) */

	/* PSU ID (from platform driver) */
	u32 psu_id;

	/* Device information */
	struct psu_device_info info;
	struct psu_config config;
	struct psu_limits limits;
	struct psu_ops *ops;

	/* Measurements */
	struct psu_measurements measurements;

	/* State */
	enum psu_state state;

	/* Note: update_mode and bootloader_mode removed - functionality moved to platform driver */

	/* Voltage control */
	u32 setpoint_mv;
	bool force_voltage_write;  /* Bypass delta guard once after clear_faults */
	s8 vout_exponent;  /* From VOUT_MODE register */
	bool voltage_configuration_disabled;

	/* GPIO */
	struct gpio_desc *psu_en_gpio;

	/* Statistics */
	atomic_long_t i2c_retry_count;
	atomic_long_t i2c_total_count;
	atomic_long_t i2c_failure_count;
	unsigned long fault_count;        /* Number of faults detected */

	/* Hot-plug support */
	unsigned int consecutive_i2c_failures;
	unsigned int disconnect_threshold;  /* From platform data, default 5 */
	enum psu_type last_psu_type;        /* For vendor attr management */
	atomic_t cached_vin_mv;             /* Lock-free VIN cache; U32_MAX = never valid */

	/* Parent platform driver reference for callbacks */
	void *parent_platform;

	/* PMBus trace level: 0=off, 1=errors/retries/status changes/writes, 2=all */
	u8 trace_level;
	/* For level-1 status-change detection. Tracks the status_word value
	 * at the time of the last emitted trace event. Updated via atomic_xchg
	 * from both the fast poll and normal I2C trace paths.
	 */
	atomic_t prev_trace_status_word;
};

/*
 * External variables
 */
extern bool debug;

/*
 * I2C Communication Functions
 */
int psu_i2c_read_byte(struct i2c_client *client, u8 reg, u8 *val);
int psu_i2c_read_word(struct i2c_client *client, u8 reg, u16 *val);
int psu_i2c_read_block(struct i2c_client *client, u8 reg, u8 *buf, size_t len);
int psu_i2c_write_byte(struct i2c_client *client, u8 reg, u8 val);
int psu_i2c_write_word(struct i2c_client *client, u8 reg, u16 val);
int psu_i2c_send_byte(struct i2c_client *client, u8 command);

/*
 * GPIO Control Functions
 */
int psu_set_enable_gpio(struct psu_data *data, bool enable);

/*
 * PMBus Protocol Functions
 */
int psu_read_manufacturer_info(struct i2c_client *client,
			       struct psu_device_info *info);
int psu_read_vout_mode(struct i2c_client *client, s8 *exponent);
int psu_read_limits(struct i2c_client *client, struct psu_limits *limits);
int psu_clear_faults(struct i2c_client *client);
int psu_set_enable(struct i2c_client *client, bool enable);
int psu_get_enable(struct i2c_client *client, bool *enabled);
int psu_set_output_voltage(struct i2c_client *client, u32 voltage_mv, s8 exponent);
int psu_get_output_voltage_setpoint(struct i2c_client *client, u32 *voltage_mv);
int psu_get_output_voltage(struct i2c_client *client, u32 *voltage_mv);

/*
 * Measurement Update Functions
 */
int psu_update_measurements(struct psu_data *data);
int psu_read_status(struct psu_data *data);
int psu_parse_faults(struct psu_data *data);
int psu_fast_status_check(struct psu_data *data);

/*
 * PSU Type Detection
 */
enum psu_type psu_detect_type(const char *model);
const char *psu_type_name(enum psu_type type);
struct psu_ops *psu_get_ops(enum psu_type type);

/*
 * Device-specific operations
 */
extern struct psu_ops chicony_s24_ops;
extern struct psu_ops boco_bs402a17_ops;
extern struct psu_ops boco_bs502a17_ops;

/*
 * Platform driver hot-plug support
 */
extern void psu_child_request_removal(void *parent_platform);
extern void psu_child_report_type(void *parent_platform, enum psu_type type);

/*
 * hwmon Interface
 */
int psu_hwmon_register(struct psu_data *data);
void psu_hwmon_unregister(struct psu_data *data);

/*
 * Sysfs Interface
 */
int psu_sysfs_create(struct psu_data *data);
void psu_sysfs_remove(struct psu_data *data);

/*
 * I2C Transaction Tracing
 */
void psu_trace_xfer(struct psu_data *data, enum psu_i2c_op_type op,
		    struct psu_i2c_op_ctx *ctx, int result,
		    int retry_count, bool pec_enabled);

/*
 * Device Initialization
 */
int psu_init_device(struct psu_data *data);

/*
 * Panic Handler - Exported for parent driver registration
 */
extern struct notifier_block psu_panic_nb;

/*
 * I2C Driver - Exported for parent driver registration
 */
extern struct i2c_driver psu_pmbus_driver;

/*
 * Utility Macros
 */
#define psu_dev(data)		(&(data)->client->dev)
#define psu_name(data)		((data)->info.model)

#define psu_err(data, fmt, ...)	\
	dev_err(psu_dev(data), fmt, ##__VA_ARGS__)
#define psu_warn(data, fmt, ...) \
	dev_warn(psu_dev(data), fmt, ##__VA_ARGS__)
#define psu_info(data, fmt, ...) \
	dev_info(psu_dev(data), fmt, ##__VA_ARGS__)
#define psu_dbg(data, fmt, ...) \
	do { if (debug) dev_info(psu_dev(data), fmt, ##__VA_ARGS__); } while (0)

#endif /* PSU_PMBUS_H */
