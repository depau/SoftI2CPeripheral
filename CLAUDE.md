# SoftI2CPeripheral — CLAUDE.md

Interrupt-driven software I2C peripheral library for AVR. See `README.md` for user-facing documentation.

## Critical: hardware INT numbers vs Arduino interrupt numbers

`digitalPinToInterrupt()` on ATmega2560 returns **Arduino-abstracted** numbers, which do NOT match hardware INT register indices. `SoftI2CPeripheral::begin()` takes **hardware INT numbers** (used directly to index EICRA/EIMSK registers). Always pass raw numbers, never `digitalPinToInterrupt()`:

```cpp
// CORRECT — hardware INT numbers for SCL=pin21, SDA=pin20
i2c.begin(0x42, SCL, SDA, 0, 1);

// WRONG — returns Arduino numbers 2 and 3, configures INT2/INT3 instead of INT0/INT1
i2c.begin(0x42, SCL, SDA, digitalPinToInterrupt(SCL), digitalPinToInterrupt(SDA));
```

Arduino Mega (ATmega2560) mapping:

| Arduino pin | `digitalPinToInterrupt()` | Hardware INT | Hardware INT# |
|-------------|--------------------------|--------------|---------------|
| 21 (SCL)    | 2                        | INT0 (PD0)   | **0**         |
| 20 (SDA)    | 3                        | INT1 (PD1)   | **1**         |
| 2           | 0                        | INT4 (PE4)   | **4**         |
| 3           | 1                        | INT5 (PE5)   | **5**         |

The default ISR vectors in `src/SoftI2CPeripheral.h` are `INT4_vect`/`INT5_vect` (designed for pins 2/3).

## API behaviour notes

- The library is a **transport-layer** peripheral — no register map. The main loop handles raw write bytes and provides read responses.
- `available()` returns true when a complete I2C event is ready: either a write-only transaction (on STOP) or a compound write+read transaction (on the read address ACK, while SCL is stretched).
- `needsResponse()` returns true when a read transaction is pending and SCL is being stretched waiting for `respond()`.
- `getWriteLength()` returns the number of bytes in the write phase. `read()` returns the next unread byte as `int` (-1 when exhausted), like `Wire.read()`. Read all bytes **before** calling `respond()` or `consume()`, as the buffer may be overwritten once the bus is released.
- For compound write+read (repeated START): `available()` fires during the read phase with the write data still accessible. Call `respond(data, len)` to provide the response and release SCL.
- For write-only transactions: call `consume()` to release the backpressure clock stretch (if any).
- Printing **before** `respond()`/`consume()` is safe and recommended — SCL is held low so the master is frozen, meaning interrupt-disabling libraries (e.g. SoftwareSerial) cannot cause missed I2C events during that window.
