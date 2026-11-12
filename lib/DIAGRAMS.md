# libpsu Diagrams

## Library Architecture

Shows what libpsu provides (portable) vs what stays platform-specific.

```mermaid
graph TB
    subgraph libpsu ["libpsu (Apache-2.0 OR GPL-2.0-only)"]
        direction TB
        TYPES["pmbus_types.h<br/><i>Portable typedefs<br/>PSU_BIT() macro</i>"]
        PROTO["pmbus_protocol.h<br/><i>Register addresses<br/>Status bit masks<br/>Format constants</i>"]
        CONV["pmbus_conversion.h/.c<br/><i>LINEAR11 ↔ milli<br/>LINEAR16 ↔ mV<br/>Exponent extraction</i>"]
        CHICONY["vendors/chicony.h<br/><i>Operation values<br/>Limit registers<br/>MFR status bits<br/>HW limit constants</i>"]
        BOCO["vendors/boco.h<br/><i>Watchdog registers<br/>FW version registers<br/>LLC temp register</i>"]

        PROTO --> TYPES
        CONV --> PROTO
        CHICONY --> TYPES
        BOCO --> TYPES
    end

    subgraph kernel ["Linux Kernel Driver (Apache-2.0 OR GPL-2.0-only; built under GPL)"]
        direction TB
        I2C["I2C Wrappers<br/><i>SMBus API, retry logic,<br/>PEC control, locking</i>"]
        DRIVER["Core Driver<br/><i>Probe/remove, workqueues,<br/>GPIO, panic handler,<br/>hot-plug, state machine</i>"]
        HWMON["hwmon + sysfs<br/><i>Sensor registration,<br/>control attributes</i>"]
        VEND_K["Vendor Drivers<br/><i>Init, PEC callbacks,<br/>sysfs attrs, FW reads</i>"]
    end

    subgraph hb ["Hashboard Firmware (Apache-2.0, future)"]
        direction TB
        HAL["HAL I2C<br/><i>Platform I2C driver,<br/>RTOS mutex</i>"]
        TASK["PSU Task<br/><i>Measurement loop,<br/>fault handling</i>"]
    end

    DRIVER --> CONV
    DRIVER --> PROTO
    VEND_K --> CHICONY
    VEND_K --> BOCO
    TASK --> CONV
    TASK --> PROTO
    TASK --> CHICONY

    style libpsu fill:#e8f4e8,stroke:#2d8a2d
    style kernel fill:#e8eaf6,stroke:#3f51b5
    style hb fill:#fff3e0,stroke:#e67e22
```

## I2C Retry and PEC Flow

Shows the per-operation retry loop with PEC save/restore.

```mermaid
flowchart TD
    START([I2C Operation Request]) --> DISCONNECTED{PSU<br/>disconnected?}
    DISCONNECTED -- Yes --> ENODEV[Return -ENODEV]
    DISCONNECTED -- No --> PEC_QUERY["Query vendor callback:<br/>use_pec_for_command(reg, is_write, is_block)"]
    PEC_QUERY --> PEC_SAVE["Save current PEC state<br/>Set PEC for this operation"]
    PEC_SAVE --> ATTEMPT["Attempt I2C operation<br/>(retry = 0)"]

    ATTEMPT --> SUCCESS{Success?}
    SUCCESS -- Yes --> PEC_RESTORE_OK["Restore PEC state"]
    PEC_RESTORE_OK --> DELAY["Inter-command delay (2 ms)"]
    DELAY --> RETURN_OK([Return 0 / byte count])

    SUCCESS -- No --> ENXIO{Error ==<br/>ENXIO?}
    ENXIO -- "Yes (retry > 0)" --> PEC_RESTORE_DISC["Restore PEC state"]
    PEC_RESTORE_DISC --> HANDLE_DISC["Handle disconnect:<br/>1. Set state = DISCONNECTED<br/>2. Disable GPIO immediately<br/>3. Request removal"]
    HANDLE_DISC --> RETURN_ENXIO([Return -ENXIO])

    ENXIO -- "Yes (retry == 0)" --> RETRY_CHECK
    ENXIO -- No --> RETRY_CHECK{Retries<br/>remaining?}
    RETRY_CHECK -- Yes --> RETRY_DELAY["Sleep RETRY_DELAY_MS (30 ms)"]
    RETRY_DELAY --> ATTEMPT
    RETRY_CHECK -- No --> PEC_RESTORE_FAIL["Restore PEC state"]
    PEC_RESTORE_FAIL --> RETURN_ERR([Return last error])
```

## Fault Clear GPIO Sequence

Shows the power-cycle sequence required to clear hardware fault latches.

```mermaid
sequenceDiagram
    participant Caller
    participant Driver
    participant GPIO
    participant PSU as PSU (PMBus)

    Caller->>Driver: psu_clear_faults()

    Note over Driver: Step 1: Save GPIO state
    Driver->>GPIO: Read current state
    GPIO-->>Driver: original_state (0 or 1)

    Note over Driver: Step 2: Power cycle
    Driver->>GPIO: Set DISABLED (0)
    Note over GPIO: Wait 50 ms

    Driver->>GPIO: Set ENABLED (1)
    Note over GPIO: Wait 50 ms<br/>(inside set_enable_gpio)

    alt Original state was DISABLED
        Note over Driver: Step 3: Restore disabled state
        Driver->>GPIO: Set DISABLED (0)
        Note over GPIO: Wait 50 ms
    end

    Note over Driver: Step 4: Send PMBus command
    Driver->>PSU: CLEAR_FAULTS (0x03, send-byte)
    PSU-->>Driver: ACK

    Driver-->>Caller: Return 0
```

## Disconnect Detection State Machine

Shows the three detection strategies and their interactions.

```mermaid
stateDiagram-v2
    [*] --> Connected

    state Connected {
        [*] --> Polling: STATUS_WORD every 250ms
        Polling --> Polling: Success (reset failure count)
    }

    Connected --> ENXIO_Disconnect: ENXIO on retry > 0
    Connected --> Threshold_Disconnect: N consecutive failures
    Connected --> VIN_Disconnect: VIN < 50V + I2C failure

    state "Immediate Disconnect" as ENXIO_Disconnect {
        [*] --> DisableGPIO1: Disable GPIO immediately
        DisableGPIO1 --> RequestRemoval1
    }

    state "Threshold Disconnect" as Threshold_Disconnect {
        [*] --> DisableGPIO2: Disable GPIO immediately
        DisableGPIO2 --> RequestRemoval2
    }

    state "VIN Disconnect" as VIN_Disconnect {
        [*] --> DisableGPIO3: Disable GPIO immediately
        DisableGPIO3 --> RequestRemoval3
    }

    ENXIO_Disconnect --> Disconnected
    Threshold_Disconnect --> Disconnected
    VIN_Disconnect --> Disconnected

    state Disconnected {
        [*] --> PollForReconnect: Poll probe addresses every 5s
        PollForReconnect --> PollForReconnect: No PSU found
    }

    Disconnected --> Connected: PSU detected at probe address
```

## PEC Decision Matrix

Shows how PEC is determined per-vendor per-operation.

```mermaid
flowchart LR
    OP([I2C Operation]) --> VENDOR{Vendor?}

    VENDOR -- Chicony --> C_DIR{Direction?}
    C_DIR -- Read --> C_OFF([PEC OFF])
    C_DIR -- Write --> C_CMD{Command?}
    C_CMD -- "0x01, 0x03, 0x21" --> C_ON([PEC ON])
    C_CMD -- Other --> C_OFF2([PEC OFF])

    VENDOR -- BOCO --> B_TYPE{Block<br/>transfer?}
    B_TYPE -- Yes --> B_OFF([PEC OFF])
    B_TYPE -- No --> B_ON([PEC ON])

    VENDOR -- Unknown --> U_OFF([PEC OFF<br/>default])

    style C_OFF fill:#ffcdd2
    style C_OFF2 fill:#ffcdd2
    style B_OFF fill:#ffcdd2
    style U_OFF fill:#ffcdd2
    style C_ON fill:#c8e6c9
    style B_ON fill:#c8e6c9
```

