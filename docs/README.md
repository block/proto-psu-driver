# PSU PMBus Kernel Driver

## Overview

The PSU PMBus kernel driver provides standardized control and monitoring for PMBus-compatible power supplies in Block's Bitcoin mining systems. It replaces previous userspace implementations with an efficient, kernel-space solution that integrates with Linux's hardware monitoring infrastructure.

### Key Features

- **Standard Linux sensor interface** via hwmon subsystem (`/sys/class/hwmon/`)
- **Comprehensive alarm monitoring** for all sensor types (voltage, current, power, temperature, fans)
- **Device-specific quirk handling** for multiple PSU models
- **Dual workqueue design** for efficient monitoring (full updates + fast status checks)
- **Safe firmware update mode** for exclusive I2C access
- **Kernel panic handler** automatically disables all PSUs on system crash
- **Hot-plug support** with automatic detection and recovery

### Supported PSU Models

| Manufacturer | Model           | Detection String     | Notes                                |
| ------------ | --------------- | -------------------- | ------------------------------------ |
| Chicony      | S4K0E001P       | "S4K0E001P"          | 4000W, no fans                       |
| BOCO         | BS402A17        | "BS402A17"           | 4000W, no fans                       |
| BOCO         | BS502A17        | "BS502A17"           | 5000W, no fans                       |

## Documentation Index

| Document | Description |
|----------|-------------|
| [Control and Telemetry](control-and-telemetry.md) | Hwmon sensor interface, sysfs control attributes, STATUS_WORD, fault counters |
| [Firmware Update](firmware-update.md) | Bootloader detection, update mode workflow, step-by-step instructions |
| [Platform and Probe](platform-probe.md) | Platform driver probe flow, hot-plug detection, GPIO handling, panic handler, device tree config |

## Architecture

### Component Overview

```
┌──────────────────────────────────────────────────────────────────┐
│ Linux Platform Driver: psu_pmbus_platform.c                     │
│ - Device tree parsing, GPIO enable/fault handling               │
│ - Hot-plug scan/recover and panic notifier                      │
└──────────────────────────────┬───────────────────────────────────┘
                               │ creates/manages i2c_client + psu_data
┌──────────────────────────────▼───────────────────────────────────┐
│ PSU Core Driver: psu_pmbus.c                                    │
│ - Probe/remove lifecycle and model detection                    │
│ - Measurement cache + locking                                   │
│ - Full-update + fast-status workqueues                          │
└───────────────┬─────────────────────┬──────────────────┬─────────┘
                │                     │                  │
      ┌─────────▼─────────┐ ┌─────────▼────────┐ ┌──────▼────────────────┐
      │ hwmon Glue        │ │ sysfs Controls   │ │ Vendor Ops (psu_ops)  │
      │ psu_hwmon.c       │ │ psu_sysfs.c      │ │ chicony/*.c, boco/*.c │
      └─────────┬─────────┘ └─────────┬────────┘ └──────┬────────────────┘
                │                     │                  │
                └──────────────┬──────┴──────────────┬───┘
                               │                     │
                ┌──────────────▼─────────────────────▼──────────────┐
                │ PMBus Protocol + Conversion Layer                 │
                │ psu_pmbus_protocol.c + lib/src/pmbus_conversion.c │
                │ - SMBus transactions, retries, PEC policy         │
                │ - LINEAR11/LINEAR16 encode/decode                 │
                └──────────────────────────┬─────────────────────────┘
                                           │
                                 ┌─────────▼─────────┐
                                 │ Linux I2C/SMBus   │
                                 │ subsystem          │
                                 └───────────────────┘
```

### Device-Specific Operations (psu_ops)

Each PSU model implements `struct psu_ops` to handle manufacturer quirks:

```c
struct psu_ops {
    const char *name;                    // Device name for logging
    int (*init)(...)                     // Device-specific initialization
    bool (*use_pec_for_command)(...)     // Per-command PEC control
    u8 fan_count;                        // Number of fans (0 = no support)
    bool skip_vout_mode;                 // Use fixed exponent instead
    s8 fixed_vout_exponent;              // Fixed exponent if skip_vout_mode=true
    u8 operation_on_value;               // Device-specific ON value
    u8 operation_off_value;              // Device-specific OFF value
    int (*get_limits)(...)               // Custom limit reading
    const char *(*get_temp_label)(...)   // Temperature sensor names
};
```

**Why this design?**
- Centralizes quirk handling per PSU model
- Allows core driver to remain generic
- Enables runtime feature detection (fans, PEC, etc.)
- Supports future PSU models without core changes

### Workqueue Design

The driver uses two separate delayed workqueues for efficient monitoring:

#### Full Measurement Update (5 seconds default)
- Reads all sensors: voltage, current, power, temperature, fans
- Performs complete status register scan
- Parses and logs fault state changes
- Updates cached measurements with mutex protection
- Configurable via `update_interval` parameter (100-10000 ms)

#### Fast Status Check (250ms default)
- Reads only STATUS_WORD register (~5ms I2C transaction)
- Provides quick fault detection
- Only logs when faults appear or clear (change detection)
- Configurable via `status_check_interval` parameter
- Suspended during firmware update mode

**Why two workqueues?**
- Full updates are I2C-intensive (~20 transactions, ~100ms total)
- Fast checks catch critical faults quickly (4x per second)
- Reduces I2C bus congestion while maintaining responsiveness
- Matches production Rust implementation behavior

### Mutex Protection Strategy

All measurement access is protected by `data->update_lock`:
- Workqueue handlers acquire lock before updating measurements
- hwmon read callbacks acquire lock before returning values
- Sysfs control functions acquire lock when checking `update_mode`
- Prevents race conditions between workqueues and userspace reads

**Lock-free operations:**
- Workqueue scheduling/cancellation (to avoid deadlocks)
- Device-specific operations (stateless callbacks)
- I2C transactions themselves (I2C core has its own locking)

## Quick Start

### Loading the Module

```bash
# Load the driver
modprobe psu_pmbus

# Verify it loaded
lsmod | grep psu_pmbus

# Check if PSU detected
ls /sys/bus/i2c/devices/6-0008/  # Replace with your I2C address
```

### Reading Sensors

```bash
# Using sensors command
sensors psu_pmbus-*

# Direct hwmon access
cat /sys/class/hwmon/hwmon*/name | grep -n psu_pmbus
cat /sys/class/hwmon/hwmon0/in1_input  # VOUT in mV
```

### Basic Control

```bash
# Check PSU info
cat /sys/bus/i2c/devices/6-0008/info/vendor
cat /sys/bus/i2c/devices/6-0008/info/model
cat /sys/bus/i2c/devices/6-0008/info/serial_number

# Control PSU output
echo 1 > /sys/bus/i2c/devices/6-0008/control/psu_enable   # Enable
echo 0 > /sys/bus/i2c/devices/6-0008/control/psu_enable   # Disable

# Set output voltage
echo 15500 > /sys/bus/i2c/devices/6-0008/control/output_voltage  # 15.5V
```

## License

The kernel driver and shared portable library are dual licensed under
Apache-2.0 OR GPL-2.0-only. Kernel builds select the GPL alternative and retain
`MODULE_LICENSE("GPL")` for access to GPL-only kernel APIs.

See [../LICENSE](../LICENSE) for Apache-2.0 and
[../LICENSES/GPL-2.0-only.txt](../LICENSES/GPL-2.0-only.txt) for GPL-2.0-only.

## Support

For issues and questions:
- **Email:** mining@proto.xyz
- **Logs:** Always include `dmesg` output when reporting issues
- **System info:** Include PSU model, I2C bus/address, kernel version

## References

### Specifications
- PMBus Power System Management Protocol Specification v1.2
- Chicony R2_4000W PMBus Communication Protocol V2
- BOCO PSU PMBus Communication Specification v1.1

### Kernel Documentation
- [Linux hwmon Subsystem](https://www.kernel.org/doc/html/latest/hwmon/hwmon-kernel-api.html)
- [Linux I2C Subsystem](https://www.kernel.org/doc/html/latest/i2c/writing-clients.html)
- [Device Tree Bindings](https://www.kernel.org/doc/Documentation/devicetree/bindings/)
