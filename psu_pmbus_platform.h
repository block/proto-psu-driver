/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PSU PMBus Parent Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This driver probes multiple I2C addresses for PSU presence and
 * dynamically creates child PSU devices for each detected PSU.
 */

#ifndef PSU_PMBUS_PLATFORM_H
#define PSU_PMBUS_PLATFORM_H

#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

#include "psu_pmbus.h"

/* Maximum number of I2C addresses to probe for PSUs */
#define MAX_PSU_ADDRESSES 8

/**
 * struct psu_platform_data - Configuration data passed from parent to child PSU drivers
 *
 * This structure contains all configuration parameters that the parent driver
 * reads from device tree and passes to each child PSU driver instance.
 */
struct psu_platform_data {
	/* PSU ID (0, 1, 2, ...) - from device tree psu-id property */
	u32 psu_id;

	/* Initial output voltage in millivolts */
	u32 initial_voltage_mv;

	/* Full measurement update interval in milliseconds */
	u32 update_interval_ms;

	/* Fast status check interval in milliseconds */
	u32 status_check_interval_ms;

	/* Consecutive I2C failure threshold for disconnection detection */
	u32 disconnect_threshold;

	/* GPIO descriptor for PSU enable control (optional, may be NULL) */
	struct gpio_desc *psu_en_gpio;

	/* Parent platform driver reference for callbacks */
	struct psu_platform_driver_data *parent_platform;
};

/**
 * struct psu_poll_state - Polling state for disconnected PSU
 *
 * Tracks polling state when PSU is disconnected. When polling is active,
 * all addresses from the probe list are checked for PSU presence.
 */
struct psu_poll_state {
	atomic_t polling_needed;         /* Non-zero if polling is active (lock-free access) */
	struct delayed_work poll_work;   /* Workqueue for polling */
	struct psu_platform_driver_data *platform;  /* Back pointer to platform */
};

/**
 * struct psu_platform_driver_data - Platform driver instance data
 *
 * Tracks all child PSU devices created by the platform driver for
 * proper cleanup during removal.
 */
struct psu_platform_driver_data {
	struct platform_device *pdev;
	struct i2c_adapter *adapter;
	struct psu_platform_data pdata;

	struct psu_poll_state poll_state;    /* Polling state for disconnected PSU */
	struct work_struct removal_work;     /* Deferred child removal work */
	struct i2c_client *child_client;     /* Single child client (only one PSU per platform) */
	u8 detected_address;                 /* Address where PSU was detected */
	u8 probe_addresses[MAX_PSU_ADDRESSES]; /* List of addresses to probe */
	int num_probe_addresses;             /* Number of addresses in probe list */
	unsigned int poll_interval_ms;       /* Reconnection poll interval */
	struct mutex lock;                   /* Protect child_client */
	bool in_update_mode;                 /* Track firmware update state */
	enum psu_type detected_psu_type;     /* PSU type: exact after probe, vendor-level in bootloader */
	u8 trace_level;                      /* PMBus trace: 0=off, 1=errors, 2=all */
};

/**
 * psu_child_request_removal - Child reports it's disconnected
 *
 * Called by child driver when it detects disconnection via consecutive
 * I2C failures. The platform driver will remove the child and start
 * polling that address for reconnection.
 *
 * @parent_platform: Pointer to the parent platform driver instance
 */
void psu_child_request_removal(void *parent_platform);
void psu_child_report_type(void *parent_platform, enum psu_type type);

#endif /* PSU_PMBUS_PLATFORM_H */
