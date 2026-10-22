#include "mini_scales.h"
#include "i2c.h"
#include <math.h>
#include <string.h>

/* M5Stack U177, v3 protocol. The STM32 HAL expects the shifted address. */
#define MINI_SCALES_ADDRESS       (0x26U << 1)
#define MINI_SCALES_RAW_ADC_REG   0x00U
#define MINI_SCALES_WEIGHT_REG    0x10U
#define MINI_SCALES_BUTTON_REG    0x20U
#define MINI_SCALES_TARE_REG      0x50U
#define MINI_SCALES_FIRMWARE_REG  0xFEU
#define MINI_SCALES_TIMEOUT_MS    25U

static HAL_StatusTypeDef ReadRegister(uint16_t reg, uint8_t *bytes, uint16_t size)
{
  return HAL_I2C_Mem_Read(&hi2c1, MINI_SCALES_ADDRESS, reg,
                          I2C_MEMADD_SIZE_8BIT, bytes, size,
                          MINI_SCALES_TIMEOUT_MS);
}

static uint32_t DecodeLittleEndian32(const uint8_t bytes[4])
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

MiniScalesStatus MiniScales_Read(MiniScalesSample *sample)
{
  uint8_t weight_bytes[4];
  uint8_t adc_bytes[4];
  uint8_t button;
  sample->weight_g = NAN;
  sample->raw_adc = 0;
  sample->button = 255;

  if (ReadRegister(MINI_SCALES_WEIGHT_REG, weight_bytes, 4) != HAL_OK ||
      ReadRegister(MINI_SCALES_RAW_ADC_REG, adc_bytes, 4) != HAL_OK ||
      ReadRegister(MINI_SCALES_BUTTON_REG, &button, 1) != HAL_OK)
  {
    return MINI_SCALES_IO_ERROR;
  }

  /* Protocol float is IEEE-754 binary32, least significant byte first. */
  uint32_t weight_bits = DecodeLittleEndian32(weight_bytes);
  uint32_t adc_bits = DecodeLittleEndian32(adc_bytes);
  float weight;
  memcpy(&weight, &weight_bits, sizeof(weight));
  memcpy(&sample->raw_adc, &adc_bits, sizeof(sample->raw_adc));
  if (button > 1)
  {
    return MINI_SCALES_INVALID_DATA;
  }
  sample->button = button;
  if (!isfinite(weight))
  {
    return MINI_SCALES_INVALID_DATA;
  }
  if (weight < -5000.0f || weight > 5000.0f)
  {
    return MINI_SCALES_OUT_OF_RANGE;
  }
  sample->weight_g = weight;
  return MINI_SCALES_OK;
}

MiniScalesStatus MiniScales_Tare(void)
{
  uint8_t command = 1;
  return HAL_I2C_Mem_Write(&hi2c1, MINI_SCALES_ADDRESS,
                           MINI_SCALES_TARE_REG, I2C_MEMADD_SIZE_8BIT,
                           &command, 1, MINI_SCALES_TIMEOUT_MS) == HAL_OK
             ? MINI_SCALES_OK : MINI_SCALES_IO_ERROR;
}

MiniScalesStatus MiniScales_GetFirmwareVersion(uint8_t *version)
{
  *version = 255;
  return ReadRegister(MINI_SCALES_FIRMWARE_REG, version, 1) == HAL_OK
             ? MINI_SCALES_OK : MINI_SCALES_IO_ERROR;
}
