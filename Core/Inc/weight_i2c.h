#ifndef WEIGHT_I2C_H
#define WEIGHT_I2C_H

#include <stdint.h>

/* Read-only U180 driver. No module calibration/address/flash writes. */
int WeightI2C_ReadRaw(int32_t *raw_adc);
int WeightI2C_GetFirmwareVersion(uint8_t *version);

#endif
