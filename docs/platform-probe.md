# Platform Driver and Probe Flow

This document covers the platform driver architecture, hot-plug detection, GPIO handling, panic handler, device tree configuration, debugging, and known limitations.

## Hot-Plug Detection and Bus Recovery

The driver implements a triple-strategy disconnection detection system optimized for fast response to physical disconnects while tolerating transient I2C errors.

### Strategy 1: Immediate Removal on ENXIO

**ENXIO (-6) Detection:**
- ENXIO (error code -6) means "No such device or address"
- Returned by I2C controller when device physically disappears
- **Triggers immediate removal** - bypasses threshold counter
- Critical for preventing bus lockup from PSUs with large capacitors

**Why This Matters:**
Some PSU models (notably Chicony S24) can hold the I2C bus stuck after physical unplug:
1. **Physical unplug** at T+0ms - PSU disconnected from connector
2. **ENXIO detected** at T+250ms - First status check fails with ENXIO
3. **GPIO disabled** at T+250ms - **IMMEDIATELY**, deasserts PSU_EN GPIO
4. **Bus recovers** at T+2500ms - PSU capacitors drain, bus released
5. **Other PSUs resume** at T+2600ms - Remaining PSUs on shared I2C bus functional
6. **Device removed** at T+25000ms - Driver cleanup completes (takes 20+ seconds due to workqueue cancellation)

**Critical: Immediate GPIO Disable**
The GPIO is disabled in the STATUS_WORD check handler immediately upon detection, NOT in the device removal handler. This is critical because device removal takes 20+ seconds (must cancel workqueues that are stuck on I2C timeouts). By disabling GPIO immediately, the bus recovers in ~2.5s instead of 40+ seconds.

### Strategy 2: VIN-Based Power Loss Detection

**Power Loss Detection via VIN Monitoring:**
- Driver monitors input voltage (VIN) from periodic measurement updates (1s interval)
- VIN is cached atomically for lock-free access by status check handler
- When VIN drops below 50V (50,000 mV) AND I2C error occurs → immediate removal
- Catches cases where PSU goes straight to ETIMEDOUT without ENXIO

**VIN Threshold Selection:**
- Normal operation: 180-264V AC (180,000-264,000 mV)
- Brownout tolerance: ~150V minimum (150,000 mV)
- **Detection threshold: 50V (50,000 mV)** - huge safety margin
- No legitimate operating condition has VIN this low

**Atomic VIN Caching:**
- VIN is cached in `atomic_t cached_vin_mv` field (updated every 1s by measurement work)
- STATUS_WORD handler reads atomic cache with `atomic_read()` - **no mutex locking**
- **Critical for performance**: Prevents blocking when measurement update is stuck on I2C retries

### Strategy 3: Threshold-Based Detection for Other Errors

**For non-ENXIO errors** (timeouts, bus busy, etc.):
- Uses configurable threshold counter (default: 5 failures)
- STATUS_WORD checked every 250ms (configurable)
- Default detection time: 250ms × 5 = 1.25 seconds
- Tolerates transient bus errors without false disconnection

**Error types using threshold:**
- `-ETIMEDOUT` (-110): I2C transaction timeout
- `-EBUSY` (-16): I2C bus busy
- `-EIO` (-5): Generic I2C communication error
- Other I2C errors (CRC failures, arbitration lost, etc.)

### Detection Timing

| Error Type | Strategy | Threshold | Interval | Detection + GPIO | Bus Recovery | Use Case |
|------------|----------|-----------|----------|------------------|--------------|----------|
| ENXIO      | Strategy 1 | N/A | 250ms | ~250ms | ~2.5s | Physical unplug (I2C detects absence) |
| VIN < 50V + I2C error | Strategy 2 | N/A | 250ms | ~250ms | ~2.5s | Power loss (Chicony direct to timeout) |
| ETIMEDOUT  | Strategy 3 | 5 | 250ms | 1.25s | ~3.75s | Bus stuck / transient errors |

**Detection Priority (checked in order):**
1. ENXIO error → immediate removal (Strategy 1)
2. VIN < 50V with any I2C error → immediate removal (Strategy 2)
3. Other errors → threshold-based (Strategy 3)

### Reconnection Detection

After PSU removal, the platform driver polls all configured addresses:
- Default polling interval: 5000ms (5 seconds)
- Configurable via `reconnect-poll-interval-ms` device tree property
- Polls all addresses from `probe-addresses` array
- Creates new child device when PSU detected

### GPIO Lifecycle During Hot-Plug

1. **System Boot**: Platform driver acquires GPIO with `GPIOD_OUT_LOW` → PSU **disabled**
2. **Initial Probe**: Child device created, inherits GPIO descriptor
   - Reads bootloader status and manufacturer info with GPIO **disabled**
   - After successful initialization, GPIO **enabled** → PSU main output turns on
3. **Normal Operation**: GPIO remains enabled, PSU provides power
4. **Disconnect Detection**: GPIO disabled immediately in detection handler → forces faster capacitor drain
5. **Reconnection Polling**: GPIO remains disabled while polling for PSU presence
6. **Reconnect Probe**: New child device created, GPIO re-enabled after successful init
7. **Ready**: PSU fully operational with GPIO enabled

**Safety Design:**
- GPIO starts disabled at boot - PSU must prove stability before being enabled
- GPIO only enabled after PSU proves it's stable (responds to I2C, passes init)
- If PSU unstable or unplugged during probe, init fails and GPIO stays disabled

## Panic Handler

The driver registers a kernel panic notifier that automatically disables all PSUs on system crash.

**Behavior:**
1. Kernel panic occurs
2. Panic notifier iterates through global PSU list
3. Each PSU's GPIO is set to disabled (deasserted)
4. Emergency messages logged to console

**Why?** Prevents uncontrolled PSU operation during system crash, protecting hardware.

**Implementation:**
- Uses spinlock for atomic list access
- Direct GPIO manipulation (no time for verification)
- Minimal logging (system is crashing)
- Uses non-sleeping GPIO calls (`gpiod_set_value`, not `gpiod_set_value_cansleep`)

## Safety Features

### GPIO Power Cycle for Fault Clearing

Some PSU faults require hardware reset to fully clear.

**clear_faults behavior:**
1. Deassert GPIO (turn off PSU) for 50ms
2. Reassert GPIO (turn on PSU)
3. Send PMBus CLEAR_FAULTS command

**Why?** PMBus command alone may not clear latched hardware faults. GPIO power cycle first ensures hardware faults are unlatched before the PMBus clear.

### Minimum Voltage Change Delta

The driver rejects voltage changes smaller than 55mV to work around a Chicony PSU firmware bug.

**Behavior:**
- If new voltage is within 55mV of current setpoint: Change is silently ignored
- Logs info message explaining the rejection
- Returns success (not error) - intentional suppression

**Why?** Chicony PSUs have a firmware bug where setting voltage within ~50mV of the current value can trigger a fault.

### Update Mode Mutex Protection

Firmware update mode uses mutex to prevent race conditions:

```c
mutex_lock(&data->update_lock);
if (mode && !data->update_mode) {
    data->update_mode = true;     // Set flag atomically
    data->measurements.valid = false;
}
mutex_unlock(&data->update_lock);

// Cancel workqueues AFTER releasing mutex to avoid deadlock
cancel_delayed_work_sync(&data->update_work);
```

## Device Tree Configuration

### Basic Configuration

```dts
&i2c6 {
    status = "okay";
    clock-frequency = <400000>;  /* 400 kHz I2C bus speed */

    psu@08 {
        compatible = "block,psu-pmbus";
        reg = <0x08>;
        psu-en-gpios = <&gpio3 12 GPIO_ACTIVE_HIGH>;
        initial-voltage-mv = <15500>;
        update-interval-ms = <5000>;
        status-check-interval-ms = <250>;
    };
};
```

### Property Reference

| Property                   | Type         | Required | Default | Description                           |
| -------------------------- | ------------ | -------- | ------- | ------------------------------------- |
| `compatible`               | string       | Yes      | N/A     | Must be "block,psu-pmbus"             |
| `reg`                      | uint32       | Yes      | N/A     | I2C slave address (7-bit)             |
| `psu-en-gpios`             | phandle+gpio | No       | None    | GPIO for PSU enable control           |
| `initial-voltage-mv`       | uint32       | No       | None    | Initial voltage setpoint (mV)         |
| `update-interval-ms`       | uint32       | No       | 5000    | Full measurement update interval (ms) |
| `status-check-interval-ms` | uint32       | No       | 250     | Fast status check interval (ms)       |
| `disconnect-threshold`     | uint32       | No       | 3       | Failures before removal               |
| `reconnect-poll-interval-ms` | uint32     | No       | 5000    | Reconnection polling interval (ms)    |

### Multiple PSU Configuration

```dts
&i2c6 {
    status = "okay";

    psu1: psu@08 {
        compatible = "block,psu-pmbus";
        reg = <0x08>;
        psu-en-gpios = <&gpio3 12 GPIO_ACTIVE_HIGH>;
        initial-voltage-mv = <15500>;
    };

    psu2: psu@09 {
        compatible = "block,psu-pmbus";
        reg = <0x09>;
        psu-en-gpios = <&gpio3 13 GPIO_ACTIVE_HIGH>;
        initial-voltage-mv = <15500>;
    };
};
```

### GPIO Configuration Notes

**Active-High vs Active-Low:**
- Use `GPIO_ACTIVE_HIGH` if PSU enables when GPIO is HIGH
- Use `GPIO_ACTIVE_LOW` if PSU enables when GPIO is LOW
- The driver uses `gpiod_*` API which handles polarity automatically

**Without GPIO:**
- Omit `psu-en-gpios` property
- Driver will still work via PMBus OPERATION command only
- Fault clearing will not perform GPIO power cycle

## Debugging

### Module Parameters

```bash
# Load with debug enabled
insmod psu-pmbus.ko debug=1

# Load with custom update intervals
insmod psu-pmbus.ko update_interval=2000 status_check_interval=100

# View current parameters
cat /sys/module/psu_pmbus/parameters/debug
cat /sys/module/psu_pmbus/parameters/update_interval
cat /sys/module/psu_pmbus/parameters/status_check_interval

# Change debug at runtime
echo 1 > /sys/module/psu_pmbus/parameters/debug
```

**debug=1 enables:**
- I2C transaction details (register, value, retry count)
- Measurement update logs with full sensor values
- Status check results
- Error details for failed optional reads

### Monitoring Logs

```bash
# Real-time monitoring
dmesg -w | grep psu-pmbus

# Recent logs
dmesg | grep psu-pmbus | tail -50

# Fault-related logs only
dmesg | grep -E "psu-pmbus.*FAULT"

# Filter by specific PSU
dmesg | grep "psu-pmbus 6-0008"
```

### Common Debug Scenarios

**Scenario 1: No measurements appearing**
```bash
# Check if driver loaded
lsmod | grep psu_pmbus

# Check if PSU detected
ls /sys/bus/i2c/devices/6-0008/

# Enable debug and watch update cycle
echo 1 > /sys/module/psu_pmbus/parameters/debug
dmesg -w | grep psu-pmbus
```

**Scenario 2: Frequent faults**
```bash
# Check fault count
cat /sys/bus/i2c/devices/6-0008/fault_count

# Check current status
cat /sys/bus/i2c/devices/6-0008/raw/status_word
printf "Decimal: %d\n" $(cat /sys/bus/i2c/devices/6-0008/raw/status_word)

# Check alarm states
find /sys/class/hwmon/hwmon* -name "*alarm" -exec sh -c 'echo -n "$1: "; cat "$1"' _ {} \;
```

**Scenario 3: I2C communication issues**
```bash
# Test I2C communication directly
i2cdetect -y 6  # Replace 6 with your bus number

# Read MFR_MODEL register
i2cget -y 6 0x08 0x9A i  # Block read

# Enable debug for detailed I2C logs
echo 1 > /sys/module/psu_pmbus/parameters/debug
```

## Testing

### Testing GPIO Control

```bash
# Verify GPIO is working
cat /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable  # Should be 1 after driver load

# Test GPIO disable/enable cycle
echo 0 > /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable
sleep 1
cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input  # VOUT should be ~0
echo 1 > /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable
sleep 2
cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input  # VOUT should return

# Test PMBus enable/disable (GPIO stays HIGH)
echo 0 > /sys/bus/i2c/devices/6-0008/control/psu_enable  # OPERATION=OFF
cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input  # VOUT should be ~0
cat /sys/bus/platform/devices/psu-pmbus-0/psu_gpio_enable  # Should still be 1
echo 1 > /sys/bus/i2c/devices/6-0008/control/psu_enable  # OPERATION=ON
cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input  # VOUT should return
```

### Testing Update Mode

```bash
# Verify normal operation
cat /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Should be 0
watch -n 1 'cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input'  # Values updating

# Enter update mode
echo 1 > /sys/bus/platform/devices/psu-pmbus-0/update_mode
cat /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Should be 1
dmesg | tail -5 | grep "Entering update mode"

# Verify measurements stop updating
watch -n 1 'cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input'  # Values frozen

# Exit update mode
echo 0 > /sys/bus/platform/devices/psu-pmbus-0/update_mode
dmesg | tail -5 | grep "Exiting update mode"
watch -n 1 'cat /sys/bus/i2c/devices/6-0008/hwmon/hwmon*/in1_input'  # Values updating again
```

### Testing Fault Tracking

```bash
# Check current fault count
cat /sys/bus/i2c/devices/6-0008/fault_count

# Start continuous monitoring
dmesg -w | grep -E "FAULT|CLEARED" &

# Apply load change (start/stop mining)
# Watch for fault messages in dmesg

# Check final fault count
cat /sys/bus/i2c/devices/6-0008/fault_count
```

## Known Limitations

### Temperature Limits Not Available

**Issue:** PSUs do not expose temperature limit registers via PMBus.

**Impact:**
- hwmon temperature attributes do not have `_min`/`_max`/`_lcrit`/`_crit` threshold attributes
- Only alarm attributes are available (based on PSU's internal thresholds)
- Cannot set custom temperature thresholds from userspace

**Workaround:** Use alarm attributes to detect when PSU's internal thresholds are exceeded.

### Update Mode is Per-PSU Instance

**Issue:** Update mode only affects the specific PSU instance.

**Impact:**
- Must enter update mode separately for each PSU
- Other PSUs continue normal operation during update
- Cannot pause all PSUs simultaneously

**Benefit:** Allows updating multiple PSUs sequentially without affecting operation of others.

### Power Values (Standard µW with 32-bit Protection)

**Behavior:** Driver follows hwmon spec, returning power in microwatts (µW).

**Details:**
- **64-bit systems:** Power returned in µW per hwmon spec (can handle up to 9,223 GW)
- **32-bit systems:** Returns -ERANGE if power > ~2.147 MW (prevents INT32_MAX overflow)
- **Custom sysfs attributes:** `power1_input_mw` and `power2_input_mw` always return milliwatts

**Example (64-bit):**
- 4kW PSU at full load reports `power1_input = 4000000000` (4000W in µW)

**Fallback for 32-bit or high-power scenarios:**
- Use custom `power1_input_mw` and `power2_input_mw` sysfs attributes
- These return milliwatts (mW) and work regardless of system architecture

### No Voltage Limit Value Attributes

**Issue:** Driver does not expose voltage limit values (`in*_min`, `in*_max`, `in*_crit`).

**Impact:**
- Cannot read the exact thresholds that trigger alarms
- Only alarm states are exposed (boolean flags)

**Rationale:**
- PSU hardware limits (MFR_VOUT_MAX) don't match alarm thresholds
- PSU's internal firmware thresholds are what actually trigger alarms
- Alarm states provide the actionable information needed

### Device-Specific Quirks

**PEC (Packet Error Checking):**
- Chicony: PEC must be OFF for reads, ON for specific writes
- BOCO: Block reads (firmware version registers) do NOT support PEC

**Exponent Handling:**
- Chicony: Fixed exponent (-9), doesn't support VOUT_MODE register
- BOCO: Fixed exponent (-7)

**Operation Values:**
- Standard: ON=0x80, OFF=0x00
- Chicony: ON=0x80, OFF=0x40 (non-standard)

**Temperature Sensors:**
- Mapping varies by manufacturer
- Number of sensors varies (1-3)
- Labels are device-specific
