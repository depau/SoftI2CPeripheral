/*
 * SoftI2CPeripheral.h - Interrupt-driven software I2C peripheral with clock stretching
 *
 * Transport-layer I2C peripheral: no register map interpretation.
 * ISRs handle bus timing; clock stretching provides flow control.
 * Main loop receives raw write bytes and provides read responses on demand.
 *
 * Clock stretching:
 *   - Read transactions: SCL held low after address ACK until respond() called
 *   - Write backpressure: if previous event unconsumed, SCL held low after
 *     address ACK until consume()/respond() releases it
 *
 * API:
 *   available()       - true when event pending (write received or read requested)
 *   needsResponse()   - true if read transaction waiting for respond()
 *   getWriteLength()  - number of bytes in write phase
 *   read()            - return next unread write byte (-1 if none), like Wire.read()
 *   respond(data,len) - provide read response, release clock stretch
 *   consume()         - acknowledge write-only event, release backpressure stretch
 *
 * IMPORTANT: read write data BEFORE calling consume() or respond(), as the
 * buffer may be overwritten by new data once the bus is released.
 *
 * Default ISR vectors target Arduino Mega pins 2/3 (INT4/INT5). Override via
 * build_flags if using different pins:
 *
 *   build_flags =
 *     -DSOFT_I2C_SCL_INT_VECT=INT0_vect
 *     -DSOFT_I2C_SDA_INT_VECT=INT1_vect
 *
 * Pass hardware INT numbers (not digitalPinToInterrupt()) to begin():
 *   - Arduino Mega pin 2  → INT4, hw int# 4
 *   - Arduino Mega pin 3  → INT5, hw int# 5
 *   - Arduino Mega pin 21 → INT0, hw int# 0
 *   - Arduino Mega pin 20 → INT1, hw int# 1
 */

#ifndef SOFT_I2C_PERIPHERAL_H
#define SOFT_I2C_PERIPHERAL_H

#include <Arduino.h>
#include <avr/interrupt.h>

#ifndef SOFT_I2C_PERIPHERAL_BUF_SIZE
#define SOFT_I2C_PERIPHERAL_BUF_SIZE 32
#endif

class SoftI2CPeripheral;
extern SoftI2CPeripheral *_g_i2c_peripheral_instance;

#pragma GCC push_options
#pragma GCC optimize("O3")

class SoftI2CPeripheral {
public:
  void begin(uint8_t address, uint8_t pin_scl, uint8_t pin_sda, uint8_t int_num_scl, uint8_t int_num_sda) {
    _address = address;
    _int_num_scl = int_num_scl;
    _int_num_sda = int_num_sda;

    _scl_pinreg = portInputRegister(digitalPinToPort(pin_scl));
    _scl_mask = digitalPinToBitMask(pin_scl);
    _scl_portreg = portOutputRegister(digitalPinToPort(pin_scl));
    _scl_ddrreg = portModeRegister(digitalPinToPort(pin_scl));

    _sda_pinreg = portInputRegister(digitalPinToPort(pin_sda));
    _sda_mask = digitalPinToBitMask(pin_sda);
    _sda_portreg = portOutputRegister(digitalPinToPort(pin_sda));
    _sda_ddrreg = portModeRegister(digitalPinToPort(pin_sda));

    pinMode(pin_scl, INPUT_PULLUP);
    pinMode(pin_sda, INPUT_PULLUP);

    _mode = MODE_IDLE;
    _bit_count = 0;
    _shift = 0;
    _controller_nack = 0;
    _stretching = false;
    _stretch_release_pending = false;
    _event_ready = false;
    _response_needed = false;
    _event_write_len = 0;
    _rx_count = 0;
    _pending_write_len = 0;
    _tx_buf_len = 0;
    _tx_buf_idx = 0;

    _g_i2c_peripheral_instance = this;

    _configInterrupt(int_num_scl);
    _configInterrupt(int_num_sda);
    EIMSK |= (1 << int_num_scl) | (1 << int_num_sda);
  }

  void end() { // NOLINT(*-make-member-function-const)
    EIMSK &= ~((1 << _int_num_scl) | (1 << _int_num_sda));
    _g_i2c_peripheral_instance = nullptr;
  }

  // ---- Main-loop API ----

  bool available() const { return _event_ready; }
  bool needsResponse() const { return _response_needed; }
  bool isIdle() const { return _mode == MODE_IDLE; }
  uint8_t getWriteLength() const { return _event_write_len; }

  int read() {
    if (_rx_read_idx < _event_write_len)
      return _rx_buf[_rx_read_idx++];
    return -1;
  }

  void respond(const uint8_t *data, uint8_t len) {
    uint8_t n = (len < SOFT_I2C_PERIPHERAL_BUF_SIZE) ? len : SOFT_I2C_PERIPHERAL_BUF_SIZE;
    for (uint8_t i = 0; i < n; i++)
      _tx_buf[i] = data[i];
    _tx_buf_len = n;
    _tx_buf_idx = 0;

    _load_and_drive_first_bit();

    uint8_t sreg = SREG;
    cli();
    _event_ready = false;
    _response_needed = false;
    _event_write_len = 0;
    _stretching = false;
    SREG = sreg;
    // Release SCL AFTER restoring interrupts — the rising edge must
    // trigger the ISR so bit_count advances. If released under cli(),
    // the controller can pull SCL LOW before the ISR fires, losing the
    // rising edge (single-bit EIFR flag gets overwritten by the falling).
    _scl_release();
  }

  void consume() {
    uint8_t sreg = SREG;
    cli();
    _event_ready = false;
    _event_write_len = 0;
    bool was_stretching = _stretching;
    _stretching = false;
    SREG = sreg;
    if (was_stretching)
      _scl_release();
  }

  // ---- ISR entry points (public for ISR access, do NOT call directly) ----

  void isr_sda_change() {
    if (!(*_scl_pinreg & _scl_mask))
      return; // SCL low → not START/STOP

    if (!(*_sda_pinreg & _sda_mask)) {
      // SDA fell while SCL high → START (or repeated START)
      // Save write length for compound transactions before resetting
      if (_mode == MODE_WRITE && _rx_count > 0) {
        _pending_write_len = _rx_count;
      } else if (_mode == MODE_IDLE) {
        _pending_write_len = 0;
      }
      _mode = MODE_ADDR;
      _bit_count = 0;
      _shift = 0;
      _rx_count = 0;
    } else {
      // SDA rose while SCL high → STOP
      if (_mode == MODE_WRITE && _rx_count > 0) {
        _event_write_len = _rx_count;
        _rx_read_idx = 0;
        _event_ready = true;
        _response_needed = false;
      }
      _mode = MODE_IDLE;
      _sda_release();
    }
  }

  void isr_scl_change() {
    if (_mode == MODE_IDLE)
      return;

    if (*_scl_pinreg & _scl_mask) {
      // ============ RISING EDGE ============

      if (_bit_count < 8) {
        if (_mode != MODE_READ) {
          _shift = (_shift << 1) | ((*_sda_pinreg & _sda_mask) ? 1 : 0);
        }
        _bit_count++;
      } else if (_bit_count == 8) {
        if (_mode == MODE_READ) {
          _controller_nack = (*_sda_pinreg & _sda_mask) ? 1 : 0;
        }
        _bit_count = 9;
      }

    } else {
      // ============ FALLING EDGE ============

      if (_bit_count == 8) {
        if (_mode == MODE_READ) {
          _sda_release(); // Let controller drive ACK/NACK
        } else {
          if (_handle_rx_byte(_shift)) {
            _sda_pull_low(); // ACK
          } else {
            _sda_release(); // NACK
            _mode = MODE_IDLE;
          }
        }

      } else if (_bit_count == 9) {
        _sda_release();
        _bit_count = 0;
        _shift = 0;

        if (_mode == MODE_READ) {
          if (_controller_nack) {
            _mode = MODE_IDLE;
            return;
          }
          if (_response_needed) {
            // First read byte after address: stretch clock until respond()
            _event_write_len = _pending_write_len;
            _rx_read_idx = 0;
            _event_ready = true;
            _scl_pull_low();
            _stretching = true;
            return; // respond() will load TX buffer and release SCL
          }
          // Subsequent read bytes: send from TX buffer
          _load_and_drive_first_bit();

        } else if (_mode == MODE_WRITE && _event_ready) {
          // Backpressure: previous event not consumed
          _scl_pull_low();
          _stretching = true;
        }

      } else if (_mode == MODE_READ && _bit_count > 0 && _bit_count < 8) {
        // Mid-byte TX: drive the next bit.
        _drive_sda(_tx_byte & _tx_mask);
        _tx_mask >>= 1;
      }
    }
  }

private:
  enum Mode : uint8_t {
    MODE_IDLE,
    MODE_ADDR,
    MODE_WRITE,
    MODE_READ
  };

  bool _handle_rx_byte(uint8_t byte) {
    if (_mode == MODE_ADDR) {
      const uint8_t addr = byte >> 1;
      const uint8_t rw = byte & 0x01;
      if (addr != _address)
        return false;

      if (rw) {
        _mode = MODE_READ;
        _response_needed = true;
        _controller_nack = 0;
      } else {
        _mode = MODE_WRITE;
        _rx_count = 0;
      }
      return true;
    }

    if (_mode == MODE_WRITE) {
      if (_rx_count < SOFT_I2C_PERIPHERAL_BUF_SIZE)
        _rx_buf[_rx_count] = byte;
      _rx_count++;
      return true;
    }

    return false;
  }

  void _load_and_drive_first_bit() {
    if (_tx_buf_idx < _tx_buf_len)
      _tx_byte = _tx_buf[_tx_buf_idx++];
    else
      _tx_byte = 0xFF;
    _tx_mask = 0x40;
    _drive_sda(_tx_byte & 0x80);
  }

  // ---- Pin control (open-drain) ----

  inline void _sda_pull_low() {
    *_sda_portreg &= ~_sda_mask;
    *_sda_ddrreg |= _sda_mask;
  }

  inline void _sda_release() {
    *_sda_ddrreg &= ~_sda_mask;
    *_sda_portreg |= _sda_mask;
  }

  inline void _drive_sda(uint8_t high) {
    if (high)
      _sda_release();
    else
      _sda_pull_low();
  }

  inline void _scl_pull_low() {
    *_scl_portreg &= ~_scl_mask;
    *_scl_ddrreg |= _scl_mask;
  }

  inline void _scl_release() {
    *_scl_ddrreg &= ~_scl_mask;
    *_scl_portreg |= _scl_mask;
  }

  static void _configInterrupt(uint8_t int_num) {
    if (int_num < 4) {
      uint8_t shift = int_num * 2;
      EICRA = (EICRA & ~(0x03 << shift)) | (0x01 << shift);
    } else {
      uint8_t shift = (int_num - 4) * 2;
      EICRB = (EICRB & ~(0x03 << shift)) | (0x01 << shift);
    }
  }

  // ---- State ----

  volatile uint8_t _mode = 0;
  volatile uint8_t _bit_count = 0;
  volatile uint8_t _shift = 0;
  volatile uint8_t _tx_byte = 0;
  volatile uint8_t _tx_mask = 0;
  volatile uint8_t _controller_nack = 0;
  volatile bool _stretching = false;
  volatile bool _stretch_release_pending = false;

  // RX: write data from controller (ISR fills, app reads directly)
  volatile uint8_t _rx_buf[SOFT_I2C_PERIPHERAL_BUF_SIZE]{};
  volatile uint8_t _rx_count = 0;
  volatile uint8_t _pending_write_len = 0; // Saved at repeated START
  uint8_t _rx_read_idx = 0;               // Cursor for read()

  // TX: read response (app fills via respond(), ISR sends)
  volatile uint8_t _tx_buf[SOFT_I2C_PERIPHERAL_BUF_SIZE]{};
  volatile uint8_t _tx_buf_len = 0;
  volatile uint8_t _tx_buf_idx = 0;

  // Event state
  volatile bool _event_ready = false;
  volatile bool _response_needed = false;
  volatile uint8_t _event_write_len = 0;

  uint8_t _address = 0;
  uint8_t _int_num_scl = 0;
  uint8_t _int_num_sda = 0;
  volatile uint8_t *_scl_pinreg = nullptr;
  uint8_t _scl_mask = 0;
  volatile uint8_t *_scl_portreg = nullptr;
  volatile uint8_t *_scl_ddrreg = nullptr;
  volatile uint8_t *_sda_pinreg = nullptr;
  uint8_t _sda_mask = 0;
  volatile uint8_t *_sda_portreg = nullptr;
  volatile uint8_t *_sda_ddrreg = nullptr;
};

#pragma GCC pop_options

// ---- Default ISR vector wiring ----

#if !defined(SOFT_I2C_PERIPHERAL_NO_DEFAULT_ISRS)

#ifndef SOFT_I2C_SCL_INT_VECT
#define SOFT_I2C_SCL_INT_VECT INT4_vect
#endif
#ifndef SOFT_I2C_SDA_INT_VECT
#define SOFT_I2C_SDA_INT_VECT INT5_vect
#endif

#endif

#endif // SOFT_I2C_PERIPHERAL_H
