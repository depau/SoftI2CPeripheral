/*
 * PingPong — SoftI2CPeripheral example
 *
 * Demonstrates compound write+read (repeated START) transactions.
 *
 * The controller writes one byte ("ping") then immediately reads one byte
 * back using a repeated START. This peripheral increments the byte by one
 * and returns it as the "pong".
 *
 * Write-only transactions (no repeated START) are also accepted and logged.
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

SoftI2CPeripheral i2c;

void setup() {
  Serial.begin(115200);
  i2c.begin(MY_ADDRESS, SCL_PIN, SDA_PIN, SCL_INT_HW, SDA_INT_HW);
  Serial.println("PingPong ready at 0x42");
}

void loop() {
  if (!i2c.available())
    return;

  // Read write data before consume()/respond() — buffer may be reused afterwards
  int val = i2c.read();

  if (i2c.needsResponse()) {
    uint8_t pong = (val >= 0) ? (uint8_t)(val + 1) : 0xFF;
    i2c.respond(&pong, 1);
    Serial.print("Ping 0x");
    Serial.print((uint8_t)val, HEX);
    Serial.print(" → Pong 0x");
    Serial.println(pong, HEX);
  } else {
    i2c.consume();
    Serial.print("Write-only: 0x");
    Serial.println((uint8_t)val, HEX);
  }
}
