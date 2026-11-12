# libpsu Specification

This document covers implementation details, vendor-specific behaviors, and
platform integration guidance for consumers of libpsu. It captures knowledge
that is not obvious from the header files alone, including undocumented
hardware quirks discovered during production deployment.

## Scope

libpsu provides **portable protocol definitions and data format conversions**.
The behaviors documented below are implemented by platform-specific driver
code (Linux kernel module, FreeRTOS task, etc.) but are described here as the
canonical reference so all consumers implement them consistently.

## PEC (Packet Error Checking)

PMBus optionally appends a CRC-8 byte to each transaction for data integrity.
PEC support is **per-command and per-vendor** -- there is no global on/off
setting that works across all PSU models.

### Chicony S24 PEC Rules

| Direction | Commands | PEC |
|-----------|----------|-----|
| **All reads** | Any register | **OFF** |
| Write | `PMBUS_OPERATION_ADDR` (0x01) | ON |
| Write | `PMBUS_VOUT_COMMAND_ADDR` (0x21) | ON |
| Write | `PMBUS_CLEAR_FAULTS_ADDR` (0x03) | ON |
| Write | All other registers | **OFF** |

**Why reads are PEC-off**: Chicony S4K0E001P PSUs have a firmware
compatibility issue where measurement reads fail when PEC is enabled. This
was discovered in production and confirmed by the vendor. PEC must be
disabled globally at initialization (`client->flags &= ~I2C_CLIENT_PEC`) and
only selectively enabled for the three critical write commands listed above.

**Protection limit writes** (OTP 0xBC, OCP 0xBD, OVP 0xBE, UV 0xB6) do
**not** use PEC. These are manufacturer-specific registers that were not
validated with PEC during Chicony's firmware development.

### BOCO PEC Rules

| Operation Type | PEC |
|----------------|-----|
| Byte reads | ON |
| Word reads | ON |
| Byte writes | ON |
| Word writes | ON |
| **Block reads** | **OFF** |

**Why block reads are PEC-off**: BOCO PSUs do not implement PEC on block
transfer responses. This affects firmware version reads (APP 0xDB, BL 0xDC)
and product revision strings.

**Implementation note**: When reading firmware version registers, the driver
must explicitly disable PEC before the block read and re-enable it afterward.
This requires a mutex to prevent concurrent I2C operations from seeing an
inconsistent PEC state:

```
lock(mutex)
disable_pec()
block_read(register)
enable_pec()
unlock(mutex)
```

### PEC Implementation Guidance for New Platforms

Each platform's I2C transport layer must support per-transaction PEC control.
The decision of whether to use PEC is made by vendor-specific code, not by
the protocol layer. The recommended pattern is a callback:

```c
bool (*use_pec_for_command)(uint8_t command, bool is_write, bool is_block);
```

The transport layer calls this before each I2C operation to determine PEC
state. PEC state must be saved and restored around each operation to avoid
leaking state between transactions.

## I2C Retry Policy

### Default Constants

| Constant | Value | Notes |
|----------|-------|-------|
| `PSU_I2C_MAX_RETRIES_DEFAULT` | 3 | Maximum attempts per operation |
| `PSU_I2C_RETRY_DELAY_MS_DEFAULT` | 30 ms | Delay between retries |
| Inter-command delay | 2 ms | Delay after each successful operation |

These defaults are exported by libpsu for reference. Each consumer should use
values appropriate for their platform -- the hashboard firmware may need
different retry counts or delays depending on I2C bus characteristics.

### Retry Semantics

1. On transient failure: retry up to `MAX_RETRIES` times with
   `RETRY_DELAY_MS` between attempts.
2. On success: insert a short inter-command delay (2 ms in the Linux driver)
   to avoid back-to-back transactions that some PSU firmware handles poorly.
3. Block reads return byte count on success (>0), other operations return 0.
   Negative values are errno-style error codes.

### ENXIO Handling (Physical Disconnect)

`-ENXIO` (errno 6) indicates no device acknowledged the I2C address. This
means the PSU is physically absent or has lost power.

**Linux driver behavior**:
- First ENXIO: retry (could be a transient bus glitch).
- Second ENXIO: confirm physical disconnect. Immediately disable the PSU
  enable GPIO and request device removal from the platform driver.

**Why immediate GPIO disable matters**: When a PSU (particularly Chicony) is
unplugged, its internal capacitors can hold the I2C bus low for 40+ seconds,
blocking all other I2C traffic on the same bus. Disabling the PSU enable GPIO
forces capacitor drain, reducing recovery time to ~2.5 seconds.

### Disconnect Detection Strategies

The Linux driver uses three complementary strategies:

1. **ENXIO-based (immediate)**: PSU physically removed, bus NACK detected.
   Triggers immediate GPIO disable and removal.

2. **Consecutive failure threshold**: After N consecutive STATUS_WORD read
   failures (default: 3), declare the PSU disconnected. Handles cases where
   the PSU returns errors other than ENXIO (e.g., ETIMEDOUT for Chicony).

3. **VIN-based (auxiliary)**: If the last known input voltage was below 50V
   and an I2C failure occurs, declare power loss. Uses an atomic (lock-free)
   VIN cache to avoid blocking on the measurement mutex when the I2C bus is
   stuck. The `U32_MAX` sentinel means VIN was never successfully read.

**Hashboard firmware note**: The hashboard has a hardwired PSU connection, so
ENXIO/disconnect detection is less relevant. However, the retry logic and
inter-command delays should still be implemented for transient bus errors.

## GPIO Enable for Fault Clearing

The PSU enable GPIO controls power delivery. It is active-low in hardware
(active-low configured via device tree `GPIO_ACTIVE_LOW` flag, abstracted by
the GPIO API so software uses logical 1=enabled, 0=disabled).

### Fault Clear Sequence

Clearing PSU faults requires a specific GPIO power-cycle sequence **before**
sending the PMBus `CLEAR_FAULTS` command. Sending `CLEAR_FAULTS` alone is
insufficient for some fault types on both Chicony and BOCO PSUs.

```
1. Read and save current GPIO state
2. Disable PSU (GPIO = 0)
3. Wait PSU_EN_ASSERT_HOLD_MS (50 ms)
4. Enable PSU (GPIO = 1)
5. If original state was DISABLED (0):
   a. Wait PSU_EN_ASSERT_HOLD_MS (50 ms)
   b. Disable PSU again (GPIO = 0) to restore original state
6. Send PMBus CLEAR_FAULTS command (send-byte to register 0x03)
```

**Why the power cycle**: Some fault latches in the PSU hardware (particularly
overvoltage and overcurrent protection circuits) are not clearable via the
PMBus command alone. The hardware fault latch requires a power cycle (PSU
enable de-assertion and re-assertion) to reset.

**Why restore original state**: If the PSU was intentionally disabled before
the fault clear was requested, the sequence must not leave it enabled. The
original GPIO state is read before the cycle and restored afterward.

### GPIO Initialization

The GPIO is initialized **LOW (disabled)** at boot by the platform driver.
The child PSU driver enables it only after confirming the PSU is present and
functional (successful I2C probe + manufacturer info read + initial
configuration). This prevents missing or unstable PSUs from affecting the
I2C bus during initial system bring-up.

### GPIO in Panic Handler

On kernel panic, all registered PSUs are immediately disabled via GPIO using
`gpiod_set_value()` (non-sleeping variant, safe in atomic context). This is
a safety measure to prevent uncontrolled PSU operation during a system crash.

### GPIO Readback Verification

After setting the GPIO, the driver reads back the value and verifies it
matches the requested state. A mismatch returns `-EIO`. This catches GPIO
hardware failures or incorrect device tree polarity configuration.

## Vendor-Specific Behaviors

### Chicony S24 (S4K0E001P)

**VOUT_MODE register**: Not supported. Always returns an incorrect or
undefined value. Use the fixed exponent `CHICONY_VOUT_EXPONENT` (-9) instead
of reading `VOUT_MODE` (0x20).

**Voltage change minimum delta**: Chicony firmware ignores voltage changes
smaller than 55 mV (`CHICONY_MIN_VOLTAGE_CHANGE_MV`). To change voltage by
a smaller amount, the driver must first step to an intermediate value at
least 55 mV away, then step to the target. This bug was identified from
production Rust code.

**Dual-module architecture**: Chicony PSUs contain two power modules at
separate I2C addresses:
- Primary: `CHICONY_PRIMARY_I2C_ADDR` (0x08)
- Secondary: `CHICONY_SECONDARY_I2C_ADDR` (0x09)

The primary module can read both modules' firmware versions.
`STATUS_MFR_SPECIFIC` (0x80) reports per-module fault status, but only from
the primary module address.

**I2C bus hold on disconnect**: When physically unplugged, Chicony PSUs hold
the I2C bus low while internal capacitors drain. This can take 40+ seconds
without GPIO intervention. The Chicony may also skip ENXIO and go directly to
ETIMEDOUT, which is why consecutive-failure and VIN-based disconnect detection
are needed in addition to ENXIO detection.

**Fan count**: 0. Chicony PSUs use system-level fans, not internal PSU fans.

### BOCO BS402A17 / BS502A17

**VOUT_MODE register**: Returns 0x19 consistently, but the driver skips
reading it and uses the fixed exponent `BOCO_VOUT_EXPONENT` (-7) for
reliability.

**Watchdog**: BOCO PSUs include a hardware watchdog controlled via
registers 0x07 (enable), 0x08 (timeout in seconds, 16-bit), and 0x09
(status: 0x00=wake, 0x01=sleep).

**LLC temperature**: An additional manufacturer-specific temperature sensor
at register `BOCO_LLC_TEMP_ADDR` (0xE9) measures the LLC resonant converter
temperature, supplementing the three standard PMBus temperature registers.

**Temperature sensor mapping**:
| Index | Register | Label |
|-------|----------|-------|
| 0 | 0x8D | Ambient |
| 1 | 0x8E | Synchronous Rectifier (SR) |
| 2 | 0x8F | Power Factor Correction (PFC) |
| 3 | 0xE9 | LLC Resonant Converter |

## Data Format Notes

### LINEAR11 Encoding

The `pmbus_milli_to_linear11()` function finds the smallest exponent that
keeps the mantissa within the 11-bit signed range (-1024 to +1023). This
means the same millivolt value may encode differently than a PSU's own
encoding if the PSU chose a different exponent. Both are valid LINEAR11
representations of the same physical value; round-trip precision loss is
bounded by the mantissa resolution at the chosen exponent.

### LINEAR16 Exponent Source

LINEAR16 values require an external exponent (not embedded in the data word).
This exponent comes from the `VOUT_MODE` register (0x20) for standard PMBus,
or from a fixed per-vendor constant when the PSU's `VOUT_MODE` register is
unreliable (both Chicony and BOCO fall into this category in practice).

**Important**: Always use the vendor-specific fixed exponent when available.
Do not read `VOUT_MODE` unless you have confirmed it returns correct values
for the specific PSU model.

## Locking Considerations

libpsu functions are **not thread-safe**. The library performs no locking
internally. Each consumer is responsible for serializing access:

- **I2C bus access**: Serialize all I2C operations to the same bus/address.
  The Linux driver uses a dedicated `i2c_lock` mutex.
- **Measurement cache**: Serialize reads and writes to the cached measurement
  structure. The Linux driver uses a separate `update_lock` mutex.
- **PEC state**: If PEC state is stored in a shared structure (e.g., Linux
  `client->flags`), protect modifications with the I2C lock.

## 64-Bit Division Portability

The conversion functions use 64-bit intermediate values. On platforms where
the C runtime provides 64-bit division (Cortex-M7 with standard toolchain,
any hosted environment), plain `/` works. The Linux kernel build handles this
via `__KERNEL__`-gated `do_div()` usage for 32-bit ARM targets that don't
link libgcc.

The libpsu implementation uses standard C division operators. The
`__KERNEL__` path is handled in the source file with conditional compilation.
