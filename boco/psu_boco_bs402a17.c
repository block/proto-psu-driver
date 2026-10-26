// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * BOCO BS402A17 PSU Driver
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Device-specific driver for BOCO BS402A17 PSUs.
 * All common BOCO logic is in psu_boco_common.c.
 */

#include <linux/module.h>

#include "psu_pmbus.h"
#include "psu_boco_protocol.h"

/*
 * BOCO BS402A17 Operations Structure
 */
struct psu_ops boco_bs402a17_ops = {
	.name = "BOCO BS402A17",
	.init = boco_common_init,
	.use_pec_for_command = boco_common_use_pec,
	.fan_count = 0,              /* Fan registers are reserved (not supported) */
	.skip_vout_mode = true,      /* Skip reading VOUT_MODE; use fixed exponent (VOUT_MODE always returns 0x19) */
	.fixed_vout_exponent = BOCO_VOUT_EXPONENT,
	.operation_on_value = PMBUS_OPERATION_ON_VAL,   /* Standard PMBus ON value */
	.operation_off_value = PMBUS_OPERATION_OFF_VAL, /* Standard PMBus OFF value */
	.post_init = NULL,
	.get_limits = NULL,
	.get_temp_label = boco_common_get_temp_label,
	.mfr_temp_sensor_count = 1,
	.read_mfr_temp = boco_common_read_mfr_temp,
	.vendor_attr_groups = boco_common_vendor_attr_groups,
};
EXPORT_SYMBOL_GPL(boco_bs402a17_ops);

MODULE_DESCRIPTION("BOCO BS402A17 PSU Driver");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
