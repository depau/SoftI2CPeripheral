/*
 * RegisterMap — SoftI2CPeripheral example
 *
 * Simulates a 16-register device with auto-incrementing address pointer,
 * like many real I2C sensors and EEPROMs.
 *
 * Protocol:
 *   Write [reg, val, val, ...]  → set register(s) starting at reg, auto-increment
 *   Write [reg] then Sr+Read    → read register(s) starting at reg, auto-increment
 *
 * Example controller code (using Wire):
 *
 *   // Write 0xAB to register 3:
 *   Wire.beginTransmission(0x42);
 *   Wire.write(3);
 *   Wire.write(0xAB);
 *   Wire.endTransmission();
 *
 *   // Read register 3:
 *   Wire.beginTransmission(0x42);
 *   Wire.write(3);
 *   Wire.endTransmission(false);   // repeated START
 *   Wire.requestFrom(0x42, 1);
 *   uint8_t val = Wire.read();     // returns 0xAB
 *
 * Wiring (Arduino Mega, pins 2/3 = INT4/INT5 — library defaults):
 *   SCL → pin 2, with 4.7 kΩ pull-up to Vcc
 *   SDA → pin 3, with 4.7 kΩ pull-up to Vcc
 */

#include <SoftI2CPeripheral.h>

#define MY_ADDRESS  0x42
#define SCL_PIN     2
#define SDA_PIN     3
#define SCL_INT_HW  4
#define SDA_INT_HW  5

#define NUM_REGS    16

SoftI2CPeripheral i2c;
uint8_t regs[NUM_REGS];

void setup() {
  Serial.begin(115200);

  // Pre-populate registers with recognisable values
  for (uint8_t i = 0; i < NUM_REGS; i++)
    regs[i] = i * 0x11;

  i2c.begin(MY_ADDRESS, SCL_PIN, SDA_PIN, SCL_INT_HW, SDA_INT_HW);
  Serial.println("RegisterMap ready at 0x42 (16 registers)");
}

void loop() {
  if (!i2c.available())
    return;

  uint8_t len = i2c.getWriteLength();
  if (len == 0) {
    i2c.consume();
    return;
  }

  // First byte is the register address
  uint8_t reg = (uint8_t)(i2c.read() & (NUM_REGS - 1));

  // Remaining bytes (if any) are data to write with auto-increment
  for (uint8_t i = 1; i < len; i++) {
    int val = i2c.read();
    if (val >= 0) {
      regs[reg] = (uint8_t)val;
      Serial.print("REG[");
      Serial.print(reg);
      Serial.print("] ← 0x");
      Serial.println((uint8_t)val, HEX);
      reg = (reg + 1) & (NUM_REGS - 1);
    }
  }

  if (i2c.needsResponse()) {
    // Build response from reg pointer, wrapping around
    uint8_t resp[NUM_REGS];
    for (uint8_t i = 0; i < NUM_REGS; i++)
      resp[i] = regs[(reg + i) & (NUM_REGS - 1)];
    i2c.respond(resp, NUM_REGS);
    Serial.print("Read from REG[");
    Serial.print(reg);
    Serial.println("]");
  } else {
    i2c.consume();
  }
}
