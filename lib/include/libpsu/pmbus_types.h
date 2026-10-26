/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PMBus Portable Type Definitions
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Portable integer typedefs and utility macros for PMBus protocol code.
 * This header abstracts away platform-specific type definitions so the
 * library can be used in both Linux kernel modules and bare-metal firmware.
 */

#ifndef LIBPSU_PMBUS_TYPES_H
#define LIBPSU_PMBUS_TYPES_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;
#endif

/* Portable s32 maximum (0x7fffffff). The kernel has S32_MAX in <linux/limits.h>
 * and userspace has INT32_MAX in <stdint.h>, but neither name is portable across
 * both builds, so libpsu defines its own. */
#define LIBPSU_S32_MAX ((s32)0x7fffffff)

/*
 * Portable BIT() macro.
 *
 * The Linux kernel provides BIT(n) via <linux/bits.h>. For non-kernel
 * consumers we define a portable equivalent.
 */
#ifndef PSU_BIT
#ifdef __KERNEL__
#include <linux/bits.h>
#define PSU_BIT(n) BIT(n)
#else
#define PSU_BIT(n) (1U << (n))
#endif
#endif

#endif /* LIBPSU_PMBUS_TYPES_H */
