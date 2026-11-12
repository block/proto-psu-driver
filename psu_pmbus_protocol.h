/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PSU PMBus Protocol Definitions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This header re-exports portable PMBus definitions from libpsu and adds
 * kernel-specific function declarations for PSU I2C probing.
 */

#ifndef PSU_PMBUS_PROTOCOL_H
#define PSU_PMBUS_PROTOCOL_H

/* Portable PMBus protocol definitions (register addresses, status flags, format constants) */
#include <libpsu/pmbus_protocol.h>

/* Portable PMBus data format conversion functions */
#include <libpsu/pmbus_conversion.h>

/**
 * psu_i2c_probe_address - Probe an I2C address for PSU presence
 * @adapter: I2C adapter to use
 * @addr: 7-bit I2C address to probe
 *
 * Returns: D6 register value (>= 0) if PSU detected, negative error code otherwise.
 *          0x00 = normal mode, 0x01 = bootloader mode.
 *
 * This function creates a temporary I2C client and attempts to read
 * the PMBUS_MFR_FWUPLOAD_MODE_ADDR register to determine if a valid
 * PSU is present at the given address.
 */
int psu_i2c_probe_address(struct i2c_adapter *adapter, u8 addr);

#endif /* PSU_PMBUS_PROTOCOL_H */
