/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * Chicony PSU Protocol Definitions (Kernel Driver)
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * This header re-exports portable Chicony constants from libpsu.
 * Kernel-specific declarations (init functions, ops structs) remain
 * in psu_pmbus.h and psu_chicony_s24.c.
 */

#ifndef PSU_CHICONY_PROTOCOL_H
#define PSU_CHICONY_PROTOCOL_H

/* Portable Chicony PSU constants from libpsu */
#include <libpsu/vendors/chicony.h>

#endif /* PSU_CHICONY_PROTOCOL_H */
