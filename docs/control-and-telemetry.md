# Control and Telemetry

This document covers the hwmon sensor interface, sysfs control attributes, STATUS_WORD interpretation, and fault tracking.

## Hwmon Interface

The driver exposes all PSU measurements through the standard Linux hwmon subsystem.

### Sensor Types and Units

| Type        | hwmon channels                  | Units                     | Description               |
| ----------- | ------------------------------- | ------------------------- | ------------------------- |
| Voltage     | `in0` (VIN), `in1` (VOUT)       | millivolts (mV)           | Input/output voltage      |
| Current     | `curr1` (IIN), `curr2` (IOUT)   | milliamps (mA)            | Input/output current      |
| Power       | `power1` (PIN), `power2` (POUT) | microwatts (µW)           | Input/output power        |
| Temperature | `temp1`, `temp2`, `temp3`       | millidegrees Celsius (mC) | PSU temperatures          |
| Fan         | `fan1`, `fan2`, `fan3`, `fan4`  | RPM                       | Fan speeds (if supported) |

### Voltage Attributes

**Input Voltage (in0 = VIN):**
```bash
in0_input         # Current VIN in mV
in0_label         # "vin"
in0_min_alarm     # 1 if VIN undervoltage warning
in0_lcrit_alarm   # 1 if VIN undervoltage fault
in0_max_alarm     # 1 if VIN overvoltage warning
in0_crit_alarm    # 1 if VIN overvoltage fault
```

**Output Voltage (in1 = VOUT):**
```bash
in1_input         # Current VOUT in mV
in1_label         # "vout"
in1_min_alarm     # 1 if VOUT undervoltage warning
in1_lcrit_alarm   # 1 if VOUT undervoltage fault
in1_max_alarm     # 1 if VOUT overvoltage warning
in1_crit_alarm    # 1 if VOUT overvoltage fault
```

**Alarm Semantics:**
- `min_alarm` / `max_alarm`: **Warning** - PSU is approaching limits but still operating
- `lcrit_alarm` / `crit_alarm`: **Fault** - PSU has shut down or will shut down imminently

### Current Attributes

**Input Current (curr1 = IIN):**
```bash
curr1_input       # Current IIN in mA
curr1_label       # "iin"
curr1_max_alarm   # 1 if IIN overcurrent warning
curr1_crit_alarm  # 1 if IIN overcurrent fault
```

**Output Current (curr2 = IOUT):**
```bash
curr2_input       # Current IOUT in mA
curr2_label       # "iout"
curr2_max_alarm   # 1 if IOUT overcurrent warning
curr2_crit_alarm  # 1 if IOUT overcurrent fault
```

### Power Attributes

The driver follows the hwmon specification, returning power in **microwatts (µW)**.

**Input Power (power1 = PIN):**
```bash
power1_input       # Current PIN in µW
power1_label       # "pin"
power1_max_alarm   # 1 if PIN overpower warning
power1_crit_alarm  # Always 0 (no specific fault bit)
```

**Output Power (power2 = POUT):**
```bash
power2_input       # Current POUT in µW
power2_label       # "pout"
power2_max_alarm   # 1 if POUT overpower warning
power2_crit_alarm  # 1 if POUT overpower fault
```

**Example:** A 4kW PSU at full load reports `power2_input = 4000000000` (4000W in µW).

**32-bit System Protection:**
- On 32-bit systems, power values exceeding ~2.147 KW would overflow INT32_MAX
- The driver returns `-ERANGE` if this occurs
- Use the milliwatt sysfs attributes (below) if you need guaranteed compatibility

### Power in Milliwatts (Custom sysfs)

For convenience and guaranteed compatibility across architectures, the driver also exposes power values in milliwatts via custom sysfs attributes:

```bash
cat /sys/bus/i2c/devices/6-0008/power1_input_mw    # Input power in mW (e.g., 1035000 = 1035W)
cat /sys/bus/i2c/devices/6-0008/power2_input_mw    # Output power in mW (e.g., 1024000 = 1024W)
```

**When to use:**
- **hwmon (`power1_input`, `power2_input`)**: Standard interface, compatible with `sensors` command and monitoring tools
- **sysfs (`power1_input_mw`, `power2_input_mw`)**: Direct access, guaranteed to work on 32-bit systems, simpler integer values

### Temperature Attributes

**Standard Temperature Sensors (temp1, temp2, temp3):**
```bash
temp[1-3]_input       # Current temperature in mC (millidegrees C)
temp[1-3]_label       # Device-specific label (see below)
```

**Temperature Alarms (temp1 only):**
```bash
temp1_min_alarm       # 1 if undertemperature warning
temp1_lcrit_alarm     # 1 if undertemperature fault
temp1_max_alarm       # 1 if overtemperature warning
temp1_crit_alarm      # 1 if overtemperature fault
```

**Note:** PMBus STATUS_TEMPERATURE is a global register, so alarm attributes are only exposed on `temp1`. All temperature channels share a single set of alarm bits.

**Temperature Sensor Mapping (Device-Specific):**

*Chicony S24:*
- `temp1`: hotspot - Hotspot temperature
- `temp2`: exhaust - Exhaust air temperature
- `temp3`: ambient - Ambient air temperature

*BOCO BS402A17 / BS502A17:*
- `temp1`: ambient - Ambient air temperature
- `temp2`: sr - Synchronous Rectifier temperature
- `temp3`: pfc - Power Factor Correction temperature
- `temp4`: llc - LLC resonant converter temperature (MFR-specific)

**Note:** Temperature limits are NOT available from PSUs. The alarms indicate PSU internal thresholds, not driver-configured limits.

### Fan Attributes

**Fan Speeds (fan1, fan2, fan3, fan4):**
```bash
fan[1-4]_input   # Current fan speed in RPM
fan[1-4]_label   # "fan1" through "fan4"
fan[1-4]_alarm   # 1 if fan fault or warning
```

**Visibility:** Fan attributes only appear if the PSU supports fans (determined by `psu_ops->fan_count`).
- Chicony S24: No fans
- BOCO BS402A17 / BS502A17: No fans

### Using the sensors Command

```bash
# View all PSU sensors
sensors psu_pmbus-*

# Example output (Chicony S24):
psu_pmbus-i2c-6-08
Adapter: i2c-6-mux (chan_id 0)
vin:         247.25 V
vout:         12.00 V
iin:           4.50 A
iout:         85.00 A
pin:        1035.00 W
pout:       1024.00 W
hotspot:      +45.0°C
exhaust:      +35.0°C
ambient:      +27.0°C
```

### Finding the hwmon Device

```bash
# Method 1: Search by name
cat /sys/class/hwmon/hwmon*/name | grep -n psu_pmbus

# Method 2: From I2C device path
ls /sys/bus/i2c/devices/6-0008/hwmon/
# Output: hwmon0 -> ../../../../../../class/hwmon/hwmon0

# Method 3: Automated search
PSU_HWMON=$(find /sys/class/hwmon -name "hwmon*" -type d | \
    xargs -I{} sh -c 'cat {}/name 2>/dev/null | grep -q psu_pmbus && echo {}')
echo $PSU_HWMON
```

## Sysfs Control Interface

The driver exposes attributes on two device types:

- **I2C child device:** `/sys/bus/i2c/devices/X-YYYY/` (e.g., `6-0008`) — per-PSU telemetry and control, organized into subdirectories (`control/`, `info/`, `limits/`, `raw/`)
- **Platform device:** `/sys/bus/platform/devices/psu-pmbus-*/` — connection state, update mode, GPIO control

### psu_enable (read/write)

Controls PSU output via PMBus OPERATION command.

Located at: `/sys/bus/i2c/devices/X-YYYY/control/psu_enable`

```bash
# Read current state
cat /sys/bus/i2c/devices/6-0008/control/psu_enable
# Output: 0 (disabled) or 1 (enabled)

# Enable PSU output
echo 1 > /sys/bus/i2c/devices/6-0008/control/psu_enable

# Disable PSU output
echo 0 > /sys/bus/i2c/devices/6-0008/control/psu_enable
```

**Behavior:**
- Write `1`: Sets OPERATION register to device-specific ON value
- Write `0`: Sets OPERATION register to device-specific OFF value

**Safety:** PSU will not output voltage until both GPIO is asserted AND OPERATION is set to ON.

### output_voltage (read/write)

Sets the PSU output voltage setpoint in millivolts.

Located at: `/sys/bus/i2c/devices/X-YYYY/control/output_voltage`

```bash
# Read current setpoint
cat /sys/bus/i2c/devices/6-0008/control/output_voltage
# Output: 15500 (15.5V in mV)

# Set voltage to 16V
echo 16000 > /sys/bus/i2c/devices/6-0008/control/output_voltage
```

**Validation (in order):**

1. **Minimum change delta** (Chicony firmware bug workaround):
   - Changes < 55mV from current setpoint are silently ignored
   - Prevents Chicony PSU fault when setting voltage too close to current value

2. **PSU-specific limits** (if available):
   - Enforces `vout_min` and `vout_max` limits read from PSU
   - Chicony S24: 15000-19000 mV
   - BOCO BS402A17 / BS502A17: Varies by model

3. **Write to VOUT_COMMAND**:
   - Converts mV to LINEAR16 format using device-specific exponent

**Example workflow:**
```bash
# Check current limits
cat /sys/bus/i2c/devices/6-0008/limits/vout_min  # 15000
cat /sys/bus/i2c/devices/6-0008/limits/vout_max  # 19000

# Set voltage within range
echo 15500 > /sys/bus/i2c/devices/6-0008/control/output_voltage  # Success

# Try to set below minimum
echo 12000 > /sys/bus/i2c/devices/6-0008/control/output_voltage  # Error: -EINVAL

# Try small change (< 55mV delta) - silently ignored
cat /sys/bus/i2c/devices/6-0008/control/output_voltage  # 15500
echo 15520 > /sys/bus/i2c/devices/6-0008/control/output_voltage
```

### clear_faults (write-only)

Clears all PSU faults via PMBus command and GPIO power cycle.

Located at: `/sys/bus/i2c/devices/X-YYYY/control/clear_faults`

```bash
# Clear all faults
echo 1 > /sys/bus/i2c/devices/6-0008/control/clear_faults
```

**Behavior:**
1. Sends PMBus CLEAR_FAULTS command (0x03)
2. If GPIO is available:
   - Deasserts PSU_EN GPIO (disable PSU)
   - Waits 50ms
   - Reasserts PSU_EN GPIO (enable PSU)
3. Resets driver's `fault_count` to 0

**Why GPIO power cycle?** Some latched faults require falling edge of PSU_EN GPIO to fully clear.

## Platform Device Attributes

These attributes are on the **platform device** (`/sys/bus/platform/devices/psu-pmbus-*/`), not the I2C child device. They remain available even when the I2C child is unregistered (e.g., during firmware update mode).

### state (read-only)

Shows the current PSU connection state.

```bash
cat /sys/bus/platform/devices/psu-pmbus-0/state
# Output: "connected", "disconnected", or "update_mode"
```

### update_mode (read/write)

Enables firmware update mode for exclusive I2C access. See [Firmware Update](firmware-update.md) for details.

```bash
cat /sys/bus/platform/devices/psu-pmbus-0/update_mode   # 0 (normal) or 1 (update mode)
echo 1 > /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Enter update mode
echo 0 > /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Exit update mode
```

### psu_id (read-only)

PSU identifier from device tree.

```bash
cat /sys/bus/platform/devices/psu-pmbus-0/psu_id
```

### psu_type (read-only)

Detected PSU type. Shows exact model when the I2C child is bound, vendor-level type in bootloader mode, or `unknown` when disconnected.

```bash
cat /sys/bus/platform/devices/psu-pmbus-0/psu_type
# Output: "chicony_s24", "boco_bs402a17", "boco_bs502a17",
#         "chicony_unknown", "boco_unknown", or "unknown"
```

### psu_gpio_enable (read/write)

**Warning:** Direct GPIO control bypassing PMBus. For testing/emergency only.

```bash
# Read GPIO state
cat /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable
# Output: 0 (LOW) or 1 (HIGH)

# Assert GPIO (enable PSU hardware precondition)
echo 1 > /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable

# Deassert GPIO (disable PSU hardware)
echo 0 > /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable
```

**Use cases:**
- Emergency PSU shutdown (faster than PMBus)
- Hardware-level testing
- Fault recovery when PMBus is unresponsive

**Normal operation:** Use `psu_enable` instead.

## I2C Child Attributes (continued)

### status_word (read-only)

Raw PMBus STATUS_WORD register value.

Located at: `/sys/bus/i2c/devices/X-YYYY/raw/status_word`

```bash
cat /sys/bus/i2c/devices/6-0008/raw/status_word
# Output: 0x0842 (hexadecimal)
```

**Bit definitions:** See PMBus specification. Commonly used bits:
- Bit 0: NONE/OK
- Bit 1: CML (communication fault)
- Bit 2: TEMPERATURE
- Bit 4: IOUT_OC_FAULT
- Bit 5: VOUT_OV_FAULT
- Bit 10: FANS
- Bit 11: POWER_GOOD_N

**For debugging only.** Normal applications should use hwmon alarms instead.

### Device Information Attributes (read-only)

Located at: `/sys/bus/i2c/devices/X-YYYY/info/`

```bash
cat /sys/bus/i2c/devices/6-0008/info/vendor          # "R24000W_" (Chicony)
cat /sys/bus/i2c/devices/6-0008/info/model           # "S4K0E001P"
cat /sys/bus/i2c/devices/6-0008/info/serial_number   # "F3B8612521000030"
cat /sys/bus/i2c/devices/6-0008/info/revision        # "V2.0"
cat /sys/bus/i2c/devices/6-0008/psu_type             # "chicony_s24" (at root level)
```

**PSU types:** `chicony_s24`, `boco_bs402a17`, `boco_bs502a17`, `chicony_unknown`, `boco_unknown`, `unknown`

### Limit Attributes (read-only)

All limits in appropriate units (mV, mA, mW).

Located at: `/sys/bus/i2c/devices/X-YYYY/limits/`

```bash
# Input limits
cat /sys/bus/i2c/devices/6-0008/limits/vin_min   # 208000 (208V)
cat /sys/bus/i2c/devices/6-0008/limits/vin_max   # 240000 (240V)
cat /sys/bus/i2c/devices/6-0008/limits/iin_max   # 12000 (12A)
cat /sys/bus/i2c/devices/6-0008/limits/pin_max   # 4000000 (4000W)

# Output limits
cat /sys/bus/i2c/devices/6-0008/limits/vout_min  # 15000 (15V)
cat /sys/bus/i2c/devices/6-0008/limits/vout_max  # 19000 (19V)
cat /sys/bus/i2c/devices/6-0008/limits/iout_max  # 235250 (235.25A)
cat /sys/bus/i2c/devices/6-0008/limits/pout_max  # 4000000 (4000W)
```

**Source:** Read from PSU's MFR_* registers (0xA0-0xA7). If not available, uses device-specific hardcoded limits.

### Root-Level Attributes (read-only)

```bash
cat /sys/bus/i2c/devices/6-0008/fault_count           # Total faults since driver load
cat /sys/bus/i2c/devices/6-0008/psu_type              # PSU model type
cat /sys/bus/i2c/devices/6-0008/psu_id                # PSU identifier
cat /sys/bus/i2c/devices/6-0008/disconnect_threshold  # Consecutive I2C failures before disconnect
cat /sys/bus/i2c/devices/6-0008/i2c_total_count       # Total I2C transactions since driver load
cat /sys/bus/i2c/devices/6-0008/i2c_failure_count     # Cumulative I2C failures since driver load
cat /sys/bus/i2c/devices/6-0008/i2c_retry_count       # Total I2C retry attempts since driver load
```

## Fault Tracking and Change Detection

The driver tracks fault state changes to avoid log spam:

```c
u16 prev_status_word;  // Previous status for comparison
u16 new_faults = current_status & ~prev_status;      // Just appeared
u16 cleared_faults = prev_status & ~current_status;  // Just cleared
```

**Benefits:**
- Faults logged only when they appear
- Fault clearance also logged for diagnostics
- `fault_count` tracks total number of fault events
- Clean logs during normal operation

### Monitoring Faults

```bash
# Check fault count
cat /sys/bus/i2c/devices/6-0008/fault_count

# Watch for fault events
dmesg -w | grep -E "FAULT|CLEARED"

# Check all alarm states
find /sys/class/hwmon/hwmon* -name "*alarm" -exec sh -c 'echo -n "$1: "; cat "$1"' _ {} \;
```
