# SoftI2CPeripheral

Interrupt-driven software I2C peripheral (slave) library for AVR microcontrollers.

Operates at the **transport layer**: the library handles all bus timing and clock
stretching via ISRs, and delivers complete I2C transactions to your `loop()` as
events. There is no register map — you receive raw write bytes and provide raw
read responses.

## Features

- Write-only and compound write+read (repeated START) transactions
- Clock stretching for flow control:
  - **Read transactions**: SCL held low after the address ACK until `respond()` is
    called, giving `loop()` unlimited time to prepare the response
  - **Write backpressure**: SCL held low if the previous event has not been
    consumed, preventing data loss under high bus load
- Open-drain pin control compatible with multi-master buses
- Single global instance; ISRs are wired via weak-overridable macros

## Requirements

- AVR microcontroller with **external interrupt pins** supporting any-edge
  triggering (EICRA/EICRB `0b01` mode): ATmega328P, ATmega2560, ATmega32U4, …
- Arduino framework (tested with PlatformIO / Arduino IDE)
- Two pins with dedicated `INTn` hardware interrupt vectors (not PCINT)

> **Note:** The library does **not** use `digitalPinToInterrupt()`. You must pass
> raw hardware INT numbers directly to `begin()`. See [Pin mapping](#pin-mapping)
> below.

## Installation

**PlatformIO** — add to `platformio.ini`:

```ini
lib_deps =
  depau/SoftI2CPeripheral@^0.1.0
```

**Arduino IDE** — download the `.zip` from the
[releases page](https://github.com/depau/SoftI2CPeripheral/releases) and install
via *Sketch → Include Library → Add .ZIP Library…*

## Quick start

```cpp
#include <SoftI2CPeripheral.h>
#include <SoftwareSerial.h>

// Arduino Mega: pins 2/3 are INT4/INT5 — the library defaults.
// No build_flags needed when using these pins.
SoftI2CPeripheral i2c;
SoftwareSerial dbg(/* rx= */ 10, /* tx= */ 11);

void setup() {
  dbg.begin(115200);
  i2c.begin(/* address= */ 0x42,
            /* pin_scl=  */ 2, /* pin_sda= */ 3,
            /* int_scl=  */ 4, /* int_sda= */ 5);  // hardware INT numbers
}

void loop() {
  if (!i2c.available())
    return;

  // --- Drain the write buffer FIRST, before consume()/respond() ---
  uint8_t buf[SOFT_I2C_PERIPHERAL_BUF_SIZE];
  uint8_t len = i2c.getWriteLength();
  for (uint8_t i = 0; i < len; i++)
    buf[i] = (uint8_t)i2c.read();

  // --- Print WHILE the clock is still stretched (before releasing the bus) ---
  dbg.print("RX: 0x");
  dbg.println(buf[0], HEX);

  if (i2c.needsResponse()) {
    uint8_t resp = buf[0] + 1;
    dbg.print("TX: 0x");
    dbg.println(resp, HEX);  // still stretched — safe!
    i2c.respond(&resp, 1);   // releases SCL
  } else {
    i2c.consume();            // releases SCL (if backpressure was active)
  }
}
```

## Using SoftwareSerial for debug output

This library deliberately holds SCL low (clock stretching) between the moment an
event is signalled via `available()` and the moment you call `respond()` or
`consume()`. That window is the ideal place to do any work — including printing
debug output — because the I2C master is frozen and **cannot send new data** while
SCL is held.

If you use [SoftwareSerial](https://docs.arduino.cc/learn/built-in-libraries/software-serial/)
(or any other library that briefly disables global interrupts during transmission),
**do all your printing before `respond()`/`consume()`**. That way:

1. SCL is held low → the master cannot advance the protocol
2. `SoftwareSerial` disables interrupts and transmits the bytes
3. You call `respond()`/`consume()` → SCL is released → normal operation resumes

This pattern makes `SoftwareSerial` completely safe alongside `SoftI2CPeripheral`,
even though `SoftwareSerial` holds `cli()` for up to ~87 µs per byte at 115200 baud.

```
Timeline (read transaction):
  Master sends address + W ──► address ACK
  Master sends data byte(s) ──► data ACK
  Master sends repeated START + R ──► address ACK
  SCL held LOW ◄─────────────────────────────────────────► respond() called
                     ↑ you are here (available() == true)
                     │
                     ├── read write bytes
                     ├── SoftwareSerial.print(...)  ← safe: master is frozen
                     └── i2c.respond(data, len)     ← releases SCL
```

For **write-only** transactions the library does not stretch SCL unless there is
backpressure from a previous unconsumed event. Still, printing before `consume()`
is recommended: it minimises the window during which interrupts are disabled, and
if the master happens to hold SCL low (e.g. due to its own clock stretching or bus
congestion) you will not miss the transition.

> **Hardware Serial note:** If you use the built-in `Serial` (hardware UART), its
> transmit path is interrupt-driven (the UDRE ISR drains a ring buffer a byte at a
> time, each ISR run taking < 1 µs). This is inherently safe at any point in the
> code. The "print before respond" advice is still good practice for determinism,
> but hardware Serial does not impose the same strict ordering requirement that
> SoftwareSerial does.

## API reference

### `begin(address, pin_scl, pin_sda, int_num_scl, int_num_sda)`

Initialises the peripheral. Must be called once before using the bus.

| Parameter    | Type      | Description |
|------------- |-----------|-------------|
| `address`    | `uint8_t` | 7-bit I2C address (without R/W bit) |
| `pin_scl`    | `uint8_t` | Arduino pin number for SCL |
| `pin_sda`    | `uint8_t` | Arduino pin number for SDA |
| `int_num_scl`| `uint8_t` | **Hardware** INT number for SCL (see table below) |
| `int_num_sda`| `uint8_t` | **Hardware** INT number for SDA |

### `end()`

Disables the external interrupts and detaches the instance. The pins are left in
their current state.

### `bool available()`

Returns `true` when a complete I2C event is ready for processing:
- A write-only transaction (fires on STOP)
- A compound write+read transaction (fires when SCL is being stretched during the
  read phase — the write data is still accessible)

### `bool needsResponse()`

Returns `true` if the current event is a read transaction and SCL is being
stretched waiting for `respond()`.

### `uint8_t getWriteLength()`

Number of bytes received in the write phase (0 for read-only transactions).

### `int read()`

Returns the next unread byte from the write buffer as `int` (0–255), or `-1` when
all bytes have been consumed. Behaves like `Wire.read()`.

**Must be called before `consume()` or `respond()`** — the buffer may be
overwritten as soon as the bus is released.

### `void respond(const uint8_t *data, uint8_t len)`

Provides the read-phase response, loads it into the TX buffer, and releases the
SCL stretch. `len` is clamped to `SOFT_I2C_PERIPHERAL_BUF_SIZE`.

Only valid when `needsResponse()` is `true`.

### `void consume()`

Acknowledges a write-only event and releases the backpressure clock stretch (if
active). Call this instead of `respond()` for write-only transactions.

### `bool isIdle()`

Returns `true` when the state machine is idle (no transaction in progress). Useful
for low-power sleep decisions.

## Pin mapping

You must pass **hardware INT numbers**, not the value returned by
`digitalPinToInterrupt()`.

You can find them in the Arduino docs if you look for the pinout PDF, i.e. `INT0`.

## ISR vector configuration

The default ISR vectors are `INT4_vect` / `INT5_vect`, matching Arduino Mega pins
2 and 3. If you use different pins, override them via `build_flags` in
`platformio.ini` **before** including the library:

```ini
[env:myboard]
build_flags =
  -DSOFT_I2C_SCL_INT_VECT=INT0_vect
  -DSOFT_I2C_SDA_INT_VECT=INT1_vect
```

Or in an Arduino sketch, define them before the `#include`:

```cpp
#define SOFT_I2C_SCL_INT_VECT  INT0_vect
#define SOFT_I2C_SDA_INT_VECT  INT1_vect
#include <SoftI2CPeripheral.h>
```

To wire the ISRs manually (e.g. if you need to share a vector), define
`SOFT_I2C_PERIPHERAL_NO_DEFAULT_ISRS` and call the entry points yourself:

```cpp
#define SOFT_I2C_PERIPHERAL_NO_DEFAULT_ISRS
#include <SoftI2CPeripheral.h>

SoftI2CPeripheral i2c;

ISR(INT0_vect) { i2c.isr_scl_change(); }
ISR(INT1_vect) { i2c.isr_sda_change(); }
```

## Buffer size

The default RX and TX buffer size is **32 bytes**. Override with a `build_flag`:

```ini
build_flags =
  -DSOFT_I2C_PERIPHERAL_BUF_SIZE=64
```

## Transaction patterns

### Write-only

```
Master:  START  ADDR+W  DATA…  STOP
```

`available()` fires after STOP. `needsResponse()` is `false`. Call `consume()`
to release backpressure stretching (if any).

```cpp
if (i2c.available() && !i2c.needsResponse()) {
  uint8_t len = i2c.getWriteLength();
  for (uint8_t i = 0; i < len; i++) {
    uint8_t b = (uint8_t)i2c.read();
    // process b
  }
  i2c.consume();
}
```

### Compound write+read (repeated START)

```
Master:  START  ADDR+W  DATA…  Sr  ADDR+R  ← SCL stretched here →  DATA…  STOP
```

`available()` fires when SCL is being stretched after the repeated START address
ACK. `needsResponse()` is `true`. The write data is still accessible. Call
`respond()` with the read payload; SCL is released automatically.

```cpp
if (i2c.available() && i2c.needsResponse()) {
  uint8_t reg = (uint8_t)i2c.read();       // read write phase first
  uint8_t val = regs[reg];
  i2c.respond(&val, 1);                     // releases SCL
}
```

## Examples

Three examples are included in `examples/`:

| Example | Description |
|---------|-------------|
| `EchoServer` | Echoes any received bytes back on reads. Minimal boilerplate, good starting point. |
| `PingPong` | Increments each received byte by one and returns it. Demonstrates compound write+read. |
| `RegisterMap` | 16-register device with auto-incrementing address pointer, similar to real sensors. |

