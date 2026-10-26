// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * PSU PMBus Driver - I2C Transaction Tracing
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Provides psu_trace_xfer() helper that extracts data from I2C operation
 * contexts and fires the psu_pmbus_xfer ftrace tracepoint. Supports
 * per-PSU trace level filtering via sysfs.
 */

#define CREATE_TRACE_POINTS
#include "psu_trace.h"

#include <linux/i2c.h>
#include <linux/string.h>
#include <asm/unaligned.h>

#include "psu_pmbus.h"

/* The op-type literals in psu_trace.h's TP_printk (__print_symbolic) are
 * hard-coded as 0..5 because the enum is not visible inside the trace
 * event header. These asserts pin the enum values so the trace output
 * cannot silently mis-label ops if the enum is ever reordered.
 */
static_assert(PSU_I2C_OP_READ_BYTE  == 0, "trace op code mismatch");
static_assert(PSU_I2C_OP_READ_WORD  == 1, "trace op code mismatch");
static_assert(PSU_I2C_OP_READ_BLOCK == 2, "trace op code mismatch");
static_assert(PSU_I2C_OP_WRITE_BYTE == 3, "trace op code mismatch");
static_assert(PSU_I2C_OP_WRITE_WORD == 4, "trace op code mismatch");
static_assert(PSU_I2C_OP_SEND_BYTE  == 5, "trace op code mismatch");

/**
 * psu_trace_xfer - Record a PMBus I2C transaction to ftrace
 * @data:        PSU driver data (for psu_id, trace_level, status_word)
 * @op:          I2C operation type
 * @ctx:         I2C operation context (register, data pointers)
 * @result:      Operation return value
 * @retry_count: Number of retries consumed
 * @pec_enabled: Whether PEC was active
 *
 * Called from psu_i2c_with_retry() after every I2C operation.
 * Respects trace_level: 0=off, 1=errors/retries/status changes/writes, 2=all.
 */
void psu_trace_xfer(struct psu_data *data, enum psu_i2c_op_type op,
		    struct psu_i2c_op_ctx *ctx, int result,
		    int retry_count, bool pec_enabled)
{
	u8 buf[PSU_TRACE_MAX_DATA];
	u8 len = 0;
	u16 status_word;
	bool is_write;

	if (data->trace_level == 0)
		return;

	status_word = data->measurements.status_word;

	/* Level 1 records only errors, retries, status changes, and writes.
	 * prev_trace_status_word holds the status_word from the last emitted
	 * trace; atomic_xchg keeps read-then-update consistent against the
	 * fast-poll path, which also writes this field. Concurrent readers
	 * may briefly observe an out-of-sync value, producing at most a few
	 * extra emits before self-correcting.
	 */
	if (data->trace_level == 1) {
		u16 prev;

		is_write = psu_i2c_op_is_write(op);

		prev = (u16)atomic_xchg(&data->prev_trace_status_word,
					status_word);
		if (!is_write && result >= 0 && retry_count == 0 &&
		    status_word == prev)
			return;
	}

	switch (op) {
	case PSU_I2C_OP_READ_BYTE:
		if (result >= 0) {
			buf[0] = *ctx->byte_val;
			len = 1;
		}
		break;
	case PSU_I2C_OP_READ_WORD:
		if (result >= 0) {
			put_unaligned_le16(*ctx->word_val, buf);
			len = 2;
		}
		break;
	case PSU_I2C_OP_READ_BLOCK:
		if (result > 0) {
			len = result < PSU_TRACE_MAX_DATA ?
			      result : PSU_TRACE_MAX_DATA;
			memcpy(buf, ctx->block.buf, len);
		}
		break;
	case PSU_I2C_OP_WRITE_BYTE:
		buf[0] = ctx->write_byte;
		len = 1;
		break;
	case PSU_I2C_OP_WRITE_WORD:
		put_unaligned_le16(ctx->write_word, buf);
		len = 2;
		break;
	case PSU_I2C_OP_SEND_BYTE:
		break;
	}

	trace_psu_pmbus_xfer(data->psu_id, data->client->addr, ctx->reg,
			     (u8)op, buf, len, (s32)result,
			     (u8)retry_count, pec_enabled ? 1 : 0,
			     status_word);
}
EXPORT_SYMBOL_GPL(psu_trace_xfer);

MODULE_DESCRIPTION("PSU PMBus I2C Transaction Tracing");
MODULE_AUTHOR("Block Inc <btcm-sw-team@squareup.com>");
MODULE_LICENSE("GPL");
