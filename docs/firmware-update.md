# Firmware Update

PSU firmware updates require exclusive I2C access to prevent driver interference. The driver supports a comprehensive update workflow with automatic bootloader detection and platform-level control.

## Sequence Diagram

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Userspace  │     │   Platform   │     │  Child I2C   │     │     PSU      │
│    (tools)   │     │    Driver    │     │    Driver    │     │  Hardware    │
└──────┬───────┘     └──────┬───────┘     └──────┬───────┘     └──────┬───────┘
       │                    │                    │                    │
       │                    │   ═══ INITIAL PROBE ═══                 │
       │                    │                    │                    │
       │                    │──── i2c_new_client_device() ───────────>│
       │                    │                    │                    │
       │                    │                    │── Read 0xD6 ──────>│
       │                    │                    │<── bootloader=1 ───│
       │                    │                    │                    │
       │                    │<── ERR_PTR(-EBUSY) ────────────────────│
       │                    │                    │                    │
       │                    │── Unbind child ───>│                    │
       │                    │── Cancel polling ──│                    │
       │                    │── Set in_update_mode=true               │
       │                    │                    │                    │
       │   ═══ PSU IN BOOTLOADER MODE - READY FOR UPDATE ═══          │
       │                    │                    │                    │
       │── cat state ──────>│                    │                    │
       │<── "update_mode" ──│                    │                    │
       │                    │                    │                    │
       │── cat update_mode ─>│                   │                    │
       │<── "1" ────────────│                    │                    │
       │                    │                    │                    │
       │   ═══ USERSPACE FIRMWARE UPLOAD ═══     |                    │
       │                    │                    │                    │
       │────────────── Direct I2C writes (firmware data) ────────────>│
       │<─────────────────── ACK ─────────────────────────────────────│
       │                    │                    │                    │
       │   (repeat until firmware upload complete)                    │
       │                    │                    │                    │
       │   ═══ EXIT UPDATE MODE ═══              |                    │
       │                    │                    │                    │
       │── echo 0 > update_mode ────────────────>│                    │
       │                    │                    │                    │
       │                    │── Set in_update_mode=false              │
       │                    │── Probe I2C address ───────────────────>│
       │                    │<── PSU responds (normal mode) ──────────│
       │                    │                    │                    │
       │                    │── i2c_new_client_device() ─────────────>│
       │                    │                    │── Read 0xD6 ──────>│
       │                    │                    │<── bootloader=0 ───│
       │                    │                    │── Normal init ────>│
       │                    │                    │                    │
       │   ═══ PSU OPERATIONAL ═══               |                    │
       │                    │                    │                    │
       │── cat state ──────>│                    │                    │
       │<── "connected" ────│                    │                    │
       │                    │                    │                    │
       │── sensors ─────────────────────────────>│<── Read sensors ──>│
       │<── sensor values ───────────────────────│                    │
       │                    │                    │                    │
```

## Platform Driver vs Child I2C Driver

The firmware update workflow involves two driver levels:

| Component | sysfs Path | Purpose |
|-----------|------------|---------|
| **Platform Driver** | `/sys/bus/platform/devices/psu-pmbus-*/` | Manages update_mode, creates/destroys child devices |
| **Child I2C Driver** | `/sys/bus/i2c/devices/X-YYYY/` | Per-PSU sensor readings, control when connected |

**Key insight:** During firmware update, the child I2C driver is **unbound** (doesn't exist), so you interact with the **platform driver** to control update mode.

## Step-by-Step Update Instructions

### Step 1: Identify the Platform Driver Node

```bash
# Verify the platform driver sysfs path exists
ls /sys/bus/platform/devices/psu-pmbus-0
# Expected output includes: state  update_mode  psu_id  psu_type  psu_gpio_enable
```

### Step 2: Check Initial State

```bash
# Check if PSU is already in update mode (from detected bootloader)
cat /sys/bus/platform/devices/psu-pmbus-0/update_mode
# Output: 1 (bootloader detected) or 0 (normal operation)

cat /sys/bus/platform/devices/psu-pmbus-0/state
# Output: "update_mode", "connected", or "disconnected"
```

**If `update_mode=1` after boot:** The driver detected the PSU in bootloader mode during initial probe. This means a previous firmware update was interrupted, and the PSU is waiting for firmware. Proceed to Step 4.

**If `update_mode=0`:** The PSU is operating normally. You can manually enter update mode if needed.

### Step 3: Enter Update Mode (if not already in bootloader)

```bash
# Enter update mode - unbinds child driver, stops I2C polling
echo 1 | sudo tee /sys/bus/platform/devices/psu-pmbus-0/update_mode

# Verify
cat /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Should be 1
cat /sys/bus/platform/devices/psu-pmbus-0/state        # Should be "update_mode"
```

This:
- Unbinds the child I2C driver (frees I2C address for direct access)
- Cancels reconnection polling
- Sets platform driver to update mode state

### Step 4: Perform Firmware Upload

```bash
# Upload firmware binary via psuctl in userspace
psuctl update-firmware firmware.bin
```

**During upload:** The PSU is in bootloader mode. Direct I2C access is safe because:
- Child driver is unbound (no driver I2C traffic)
- Platform polling is suspended (no probe attempts)

### Step 5: Exit Update Mode (Resume Normal Operation)

```bash
# Exit update mode - re-probes PSU and rebinds child driver
echo 0 | sudo tee /sys/bus/platform/devices/psu-pmbus-0/update_mode
```

This triggers:
1. Platform driver exits update mode
2. Probes the I2C address for PSU presence
3. If PSU detected: creates new child device
4. Child driver initializes and starts sensor polling

### Step 6: Verify Successful Rebind

```bash
# Check platform state
cat /sys/bus/platform/devices/psu-pmbus-0/state
# Expected: "connected"

# Watch kernel logs for rebind
dmesg | tail -n 50 | grep psu-pmbus
# Expected: "PSU probe successful" or similar

# Verify child device appeared
ls /sys/bus/i2c/devices/ | grep "6-00"
# Expected: 6-0008 (or your PSU's address)

# Check sensor readings
sensors psu_pmbus-*
```

## Bootloader Mode Auto-Detection

The driver automatically detects if a PSU is in bootloader mode on driver load.

**Detection mechanism:**
1. First I2C operation after GPIO setup reads `PMBUS_MFR_FWUPLOAD_MODE_ADDR` (0xD6)
2. If register returns 0x01: PSU is in bootloader mode
3. Driver automatically enters `update_mode` and skips all initialization
4. Prevents driver I2C from corrupting in-progress firmware update

**Why critical?** If a previous firmware update was interrupted (power loss, crash), the PSU may still be in bootloader mode. Normal driver operations can cause issues.

**Recovery workflow:**
1. Detect bootloader mode: the driver auto-detects this and enters `update_mode` (check via `cat /sys/bus/platform/devices/psu-pmbus-0/state`)
2. Alternatively, read register directly: `i2cget -y 6 0x08 0xD6` (0x01 = bootloader)
3. Complete or retry firmware update using userspace tools
4. PSU exits bootloader mode when update succeeds
5. Power cycle PSU if update cannot be completed

## Troubleshooting

### State stays "update_mode" after writing 0

The PSU may still be in bootloader mode:

```bash
# Check bootloader status via direct I2C read
i2cget -y 6 0x08 0xD6
# 0x01 = still in bootloader, 0x00 = normal mode
```

**Solutions:**
1. **Complete the firmware upload** - PSU exits bootloader after successful update
2. **Power cycle the PSU** - Forces exit from bootloader (may lose partial firmware)
3. **Retry firmware upload** - If upload was incomplete

### Child driver doesn't rebind

```bash
# Check if probe address is correct
cat /sys/bus/platform/devices/psu-pmbus-0/of_node/probe-addresses
# Should list your PSU's I2C address

# Manually trigger re-probe
echo 0 | sudo tee /sys/bus/platform/devices/psu-pmbus-0/update_mode  # Exits and re-probes

# Check for I2C errors
dmesg | tail -n 20 | grep -E "psu|i2c"
```

### PSU not responding after update

```bash
# Power cycle the PSU via GPIO (if available)
GPIO_PATH=/sys/class/gpio/gpio<N>
echo 0 > $GPIO_PATH/value  # Disable
sleep 2
echo 1 > $GPIO_PATH/value  # Enable
sleep 2

# Re-trigger probe
echo 0 | sudo tee /sys/bus/platform/devices/psu-pmbus-0/update_mode
```

## Complete Example Script

```bash
#!/bin/bash
set -e

PLATFORM=/sys/bus/platform/devices/psu-pmbus
I2C_BUS=6
I2C_ADDR=0x08
FIRMWARE_FILE=$1

if [ -z "$FIRMWARE_FILE" ]; then
    echo "Usage: $0 <firmware.bin>"
    exit 1
fi

echo "=== PSU Firmware Update Script ==="

# Step 1: Check initial state
echo "Checking initial state..."
STATE=$(cat $PLATFORM/state)
UPDATE_MODE=$(cat $PLATFORM/update_mode)
echo "  State: $STATE"
echo "  Update Mode: $UPDATE_MODE"

# Step 2: Enter update mode if needed
if [ "$UPDATE_MODE" != "1" ]; then
    echo "Entering update mode..."
    echo 1 | sudo tee $PLATFORM/update_mode > /dev/null
    sleep 1
fi

# Verify update mode
if [ "$(cat $PLATFORM/update_mode)" != "1" ]; then
    echo "ERROR: Failed to enter update mode"
    exit 1
fi
echo "  Now in update mode"

# Step 3: Perform firmware upload
echo "Uploading firmware: $FIRMWARE_FILE"
# Replace with your actual firmware upload command
# psu-firmware-tool --bus $I2C_BUS --addr $I2C_ADDR --file $FIRMWARE_FILE
echo "  (Firmware upload would happen here)"
sleep 2  # Simulate upload time

# Step 4: Exit update mode
echo "Exiting update mode..."
echo 0 | sudo tee $PLATFORM/update_mode > /dev/null

# Step 5: Wait for rebind
echo "Waiting for PSU rebind..."
for i in {1..10}; do
    sleep 1
    STATE=$(cat $PLATFORM/state)
    echo "  Attempt $i: State = $STATE"
    if [ "$STATE" = "connected" ]; then
        echo "SUCCESS: PSU rebound successfully"
        break
    fi
done

# Step 6: Final verification
echo ""
echo "=== Final Status ==="
cat $PLATFORM/state
echo ""
dmesg | tail -n 10 | grep psu-pmbus || true
echo ""
sensors psu_pmbus-* 2>/dev/null || echo "(Sensors not yet available)"
```

## Important Notes

**Timing:**
- Entering update mode: ~1-5 seconds (cancels workqueues synchronously)
- Firmware upload: Minutes (depends on PSU and image size)
- Exiting update mode: ~2-5 seconds (probes and initializes)

**Safety:**
- Update mode is protected by mutex - prevents race conditions
- Bootloader detection is automatic - driver won't interfere with interrupted updates
- Child driver is fully unbound during update - no I2C conflicts

**Per-PSU:**
- This workflow is for single-PSU systems
- Multi-PSU systems update one PSU at a time
- Other PSUs continue normal operation
