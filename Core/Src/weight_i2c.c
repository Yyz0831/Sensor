#include "weight_i2c.h"
#include "i2c.h"
#include <string.h>

#define WEIGHT_I2C_ADDRESS (0x26U << 1)

int WeightI2C_ReadRaw(int32_t *raw_adc)
{
  uint8_t bytes[4];
  *raw_adc = 0;
  if (HAL_I2C_Mem_Read(&hi2c1, WEIGHT_I2C_ADDRESS, 0x00U,
                       I2C_MEMADD_SIZE_8BIT, bytes, sizeof(bytes), 25U) != HAL_OK)
  {
    return 0;
  }
  uint32_t bits = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                  ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  memcpy(raw_adc, &bits, sizeof(*raw_adc));
  return 1;
}

int WeightI2C_GetFirmwareVersion(uint8_t *version)
{
  *version = 255;
  return HAL_I2C_Mem_Read(&hi2c1, WEIGHT_I2C_ADDRESS, 0xFEU,
                         I2C_MEMADD_SIZE_8BIT, version, 1, 25U) == HAL_OK;
}
