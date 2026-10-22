#include "mini_scales.h"
#include "i2c.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

I2C_HandleTypeDef hi2c1;
static uint32_t weight_bits = 0x42FB0000U; /* 125.5 g */
static uint8_t button = 1;
static int fail_reg = -1;
static int fail_write;
static unsigned writes;

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *bus, uint16_t address,
                                  uint16_t reg, uint16_t reg_size,
                                  uint8_t *bytes, uint16_t size, uint32_t timeout)
{
  assert(bus == &hi2c1 && address == 0x4CU);
  assert(reg_size == I2C_MEMADD_SIZE_8BIT && timeout == 25U);
  if ((int)reg == fail_reg)
    return HAL_ERROR;
  if (reg == 0xFEU)
  {
    assert(size == 1);
    bytes[0] = 4;
  }
  else if (reg == 0x20U)
  {
    assert(size == 1);
    bytes[0] = button;
  }
  else
  {
    assert(size == 4 && (reg == 0x10U || reg == 0x00U));
    uint32_t bits = reg == 0x10U ? weight_bits : 0xFFFFCFC7U; /* ADC=-12345 */
    for (unsigned i = 0; i < 4; ++i)
      bytes[i] = (uint8_t)(bits >> (8U * i));
  }
  return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *bus, uint16_t address,
                                   uint16_t reg, uint16_t reg_size,
                                   uint8_t *bytes, uint16_t size, uint32_t timeout)
{
  assert(bus == &hi2c1 && address == 0x4CU);
  assert(reg == 0x50U && reg_size == I2C_MEMADD_SIZE_8BIT);
  assert(size == 1 && bytes[0] == 1 && timeout == 25U);
  ++writes;
  return fail_write ? HAL_ERROR : HAL_OK;
}

int main(void)
{
  MiniScalesSample sample;
  assert(MiniScales_Read(&sample) == MINI_SCALES_OK);
  assert(sample.weight_g == 125.5f && sample.raw_adc == -12345 && sample.button == 1);
  weight_bits = 0xC1480000U; /* -12.5 g must remain negative. */
  button = 0;
  assert(MiniScales_Read(&sample) == MINI_SCALES_OK);
  assert(sample.weight_g == -12.5f && sample.button == 0);

  for (unsigned i = 0; i < 3; ++i)
  {
    const int failed_reads[] = {0x10, 0x00, 0x20};
    fail_reg = failed_reads[i];
    assert(MiniScales_Read(&sample) == MINI_SCALES_IO_ERROR);
    assert(isnan(sample.weight_g) && sample.button == 255 && sample.raw_adc == 0);
  }
  fail_reg = -1;
  assert(MiniScales_Read(&sample) == MINI_SCALES_OK); /* Recovers without reboot. */
  assert(sample.weight_g == -12.5f);
  weight_bits = 0x7FC00000U;
  assert(MiniScales_Read(&sample) == MINI_SCALES_INVALID_DATA && isnan(sample.weight_g));
  weight_bits = 0x7F800000U;
  assert(MiniScales_Read(&sample) == MINI_SCALES_INVALID_DATA && isnan(sample.weight_g));
  float overload = 5001.0f;
  memcpy(&weight_bits, &overload, sizeof(overload));
  assert(MiniScales_Read(&sample) == MINI_SCALES_OUT_OF_RANGE && isnan(sample.weight_g));
  weight_bits = 0;
  button = 2;
  assert(MiniScales_Read(&sample) == MINI_SCALES_INVALID_DATA && sample.button == 255);
  button = 1;
  uint8_t version;
  assert(MiniScales_GetFirmwareVersion(&version) == MINI_SCALES_OK && version == 4);
  fail_reg = 0xFE;
  assert(MiniScales_GetFirmwareVersion(&version) == MINI_SCALES_IO_ERROR && version == 255);
  assert(writes == 0); /* Reads never change zero point, calibration, or address. */
  assert(MiniScales_Tare() == MINI_SCALES_OK && writes == 1);
  fail_write = 1;
  assert(MiniScales_Tare() == MINI_SCALES_IO_ERROR && writes == 2);
  puts("Mini Scales protocol, signed data, failures, recovery, and tare passed.");
  return 0;
}
