/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only */
/*
 * PSU PMBus Driver - I2C Transaction Tracing
 *
 * Copyright (C) 2025 Block, Inc.
 *
 * Defines an ftrace tracepoint for capturing PMBus I2C transactions.
 * Used for PSU failure analysis — trace data can be exported as CSV
 * and handed off to PSU vendors (e.g., Chicony) for triage.
 *
 * Usage:
 *   echo 2 > /sys/bus/platform/devices/<psu-pmbus-dev>/trace_level
 *                                                       # 0=off, 1=errors, 2=all
 *   echo 1 > /sys/kernel/debug/tracing/events/psu_pmbus/psu_pmbus_xfer/enable
 *   cat /sys/kernel/debug/tracing/trace                 # read trace output
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM psu_pmbus

#if !defined(_PSU_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _PSU_TRACE_H

#include <linux/tracepoint.h>
#include <linux/types.h>

#define PSU_TRACE_MAX_DATA	32

/*
 * psu_pmbus_xfer - Trace a PMBus I2C transaction
 *
 * @psu_id:      PSU identifier (from device tree)
 * @i2c_addr:    7-bit I2C slave address
 * @reg:         PMBus register/command code
 * @op_type:     Operation type (0=RB, 1=RW, 2=RK, 3=WB, 4=WW, 5=SB)
 * @data:        Raw data bytes (read result or write payload)
 * @data_len:    Number of valid bytes in data
 * @result:      Return value (0/positive=success, negative=errno)
 * @retry_count: Number of retries consumed (0=first attempt succeeded)
 * @pec_enabled: Whether PEC was active for this transaction
 * @status_word: Cached PMBus STATUS_WORD snapshot
 */
TRACE_EVENT(psu_pmbus_xfer,

	TP_PROTO(u32 psu_id, u8 i2c_addr, u8 reg, u8 op_type,
		 const u8 *data, u8 data_len, s32 result,
		 u8 retry_count, u8 pec_enabled, u16 status_word),

	TP_ARGS(psu_id, i2c_addr, reg, op_type, data, data_len,
		result, retry_count, pec_enabled, status_word),

	TP_STRUCT__entry(
		__field(u32, psu_id)
		__field(u8, i2c_addr)
		__field(u8, reg)
		__field(u8, op_type)
		__field(u8, data_len)
		__field(s32, result)
		__field(u8, retry_count)
		__field(u8, pec_enabled)
		__field(u16, status_word)
		__array(u8, data, PSU_TRACE_MAX_DATA)
	),

	TP_fast_assign(
		__entry->psu_id = psu_id;
		__entry->i2c_addr = i2c_addr;
		__entry->reg = reg;
		__entry->op_type = op_type;
		__entry->data_len = data_len < PSU_TRACE_MAX_DATA ?
				    data_len : PSU_TRACE_MAX_DATA;
		__entry->result = result;
		__entry->retry_count = retry_count;
		__entry->pec_enabled = pec_enabled;
		__entry->status_word = status_word;
		memcpy(__entry->data, data, __entry->data_len);
		if (__entry->data_len < PSU_TRACE_MAX_DATA)
			memset(__entry->data + __entry->data_len, 0,
			       PSU_TRACE_MAX_DATA - __entry->data_len);
	),

	TP_printk("psu%u addr=0x%02x reg=0x%02x op=%s data=%s result=%d retries=%u pec=%u status=0x%04x",
		__entry->psu_id,
		__entry->i2c_addr,
		__entry->reg,
		__print_symbolic(__entry->op_type,
			{ 0, "RB" }, { 1, "RW" }, { 2, "RK" },
			{ 3, "WB" }, { 4, "WW" }, { 5, "SB" }),
		__print_hex(__entry->data, __entry->data_len),
		__entry->result,
		__entry->retry_count,
		__entry->pec_enabled,
		__entry->status_word)
);

#endif /* _PSU_TRACE_H */

/* This must be outside the header guard */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE psu_trace
#include <trace/define_trace.h>
