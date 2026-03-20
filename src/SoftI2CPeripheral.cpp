/*
 * SoftI2CPeripheral.cpp
 */

#include "SoftI2CPeripheral.h"

SoftI2CPeripheral *_g_i2c_peripheral_instance = nullptr;

#if !defined(SOFT_I2C_PERIPHERAL_NO_DEFAULT_ISRS)

ISR(SOFT_I2C_SCL_INT_VECT) {
  if (_g_i2c_peripheral_instance) {
    _g_i2c_peripheral_instance->isr_scl_change();
  }
}

ISR(SOFT_I2C_SDA_INT_VECT) {
  if (_g_i2c_peripheral_instance) {
    _g_i2c_peripheral_instance->isr_sda_change();
  }
}

#endif
