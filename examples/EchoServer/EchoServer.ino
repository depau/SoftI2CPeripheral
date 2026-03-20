/*
 * EchoServer — SoftI2CPeripheral example
 *
 * Simplest possible peripheral: echoes every received byte back on reads.
 *
 * For write-only transactions:  prints received bytes via Serial and ACKs.
 * For write+read transactions:  responds with the same bytes that were written.
 *
 * Wiring (Arduino Mega, pins 2/3 = INT4/INT5 — library defaults):
 *   SCL → pin 2, with 4.7 kΩ pull-up to Vcc
 *   SDA → pin 3, with 4.7 kΩ pull-up to Vcc
 *
 * For other boards/pins, pass the correct hardware INT numbers to begin().
 * See the library README for the Arduino Mega pin→INT mapping.
 */

#include <SoftI2CPeripheral.h>

#define MY_ADDRESS  0x42
#define SCL_PIN     2   // Arduino Mega: pin 2 = INT4
#define SDA_PIN     3   // Arduino Mega: pin 3 = INT5
#define SCL_INT_HW  4   // Hardware INT number (NOT digitalPinToInterrupt())
#define SDA_INT_HW  5

SoftI2CPeripheral i2c;

void setup() {
  Serial.begin(115200);
  i2c.begin(MY_ADDRESS, SCL_PIN, SDA_PIN, SCL_INT_HW, SDA_INT_HW);
  Serial.println("EchoServer ready at 0x42");
}

void loop() {
  if (!i2c.available())
    return;

  // Drain the write buffer before calling consume()/respond()
  uint8_t buf[SOFT_I2C_PERIPHERAL_BUF_SIZE];
  uint8_t len = i2c.getWriteLength();
  for (uint8_t i = 0; i < len; i++)
    buf[i] = (uint8_t)i2c.read();

  Serial.print("RX [");
  Serial.print(len);
  Serial.print("]:");
  for (uint8_t i = 0; i < len; i++) {
    Serial.print(' ');
    Serial.print(buf[i], HEX);
  }
  Serial.println();

  if (i2c.needsResponse()) {
    i2c.respond(buf, len);   // echo back the same bytes
  } else {
    i2c.consume();
  }
}
