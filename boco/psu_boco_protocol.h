/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * BOCO PSU Protocol Definitions (Kernel Driver)
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This header re-exports portable BOCO constants from libpsu and adds
 * kernel-specific function declarations for BOCO driver operations.
 */

#ifndef PSU_BOCO_PROTOCOL_H
#define PSU_BOCO_PROTOCOL_H

/* Portable BOCO PSU constants from libpsu */
#include <libpsu/vendors/boco.h>

/* Forward declarations for kernel types */
struct i2c_client;
struct psu_data;
struct attribute_group;

/*
 * BOCO Common Functions (implemented in psu_boco_common.c)
 */

bool boco_common_use_pec(u8 command, bool is_write, bool is_block);
int boco_common_init(struct i2c_client *client, struct psu_data *data);
int boco_common_read_mfr_temp(struct i2c_client *client, int sensor_index, s32 *temp_mc);
const char *boco_common_get_temp_label(unsigned int sensor_index);

/* Shared vendor sysfs attribute groups (firmware version + watchdog) */
extern const struct attribute_group *boco_common_vendor_attr_groups[];

#endif /* PSU_BOCO_PROTOCOL_H */
