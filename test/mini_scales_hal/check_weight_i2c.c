#include "i2c.h"
#include "weight_i2c.h"
#include "weight_calibration.h"
#include "weight_command.h"
#include "weight_profile.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

I2C_HandleTypeDef hi2c1;
static uint32_t raw_bits;
static int fail_read;
static unsigned read_count;

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *bus, uint16_t address,
                                  uint16_t reg, uint16_t reg_size,
                                  uint8_t *bytes, uint16_t size, uint32_t timeout)
{
  assert(bus == &hi2c1 && address == 0x4CU);
  assert(reg_size == I2C_MEMADD_SIZE_8BIT && timeout == 25U);
  ++read_count;
  if (fail_read)
    return HAL_ERROR;
  if (reg == 0x00U)
  {
    assert(size == 4);
    for (unsigned i = 0; i < 4; ++i)
      bytes[i] = (uint8_t)(raw_bits >> (8U * i));
  }
  else
  {
    assert(reg == 0xFEU && size == 1);
    bytes[0] = 4;
  }
  return HAL_OK;
}

/* Any attempt to write module tare/gain/address/flash fails this test. */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *bus, uint16_t address,
                                   uint16_t reg, uint16_t reg_size,
                                   uint8_t *bytes, uint16_t size, uint32_t timeout)
{
  (void)bus; (void)address; (void)reg; (void)reg_size;
  (void)bytes; (void)size; (void)timeout;
  assert(0 && "Unit Weight-I2C must remain read-only");
  return HAL_ERROR;
}

static void CheckDriver(void)
{
  const int32_t values[] = {0, 123456, -12345, 8388607, -8388608, INT32_MAX, INT32_MIN};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
  {
    memcpy(&raw_bits, &values[i], sizeof(raw_bits));
    int32_t actual;
    unsigned before = read_count;
    assert(WeightI2C_ReadRaw(&actual) && actual == values[i]);
    assert(read_count == before + 1); /* No Mini Scales button/weight reads. */
  }
  uint8_t version;
  assert(WeightI2C_GetFirmwareVersion(&version) && version == 4);
  fail_read = 1;
  int32_t raw = 123;
  assert(!WeightI2C_ReadRaw(&raw) && raw == 0);
  assert(!WeightI2C_GetFirmwareVersion(&version) && version == 255);
  fail_read = 0;
  raw_bits = 123456;
  assert(WeightI2C_ReadRaw(&raw) && raw == 123456);
}

static WeightResult Collect(WeightCalibration *state, int32_t raw)
{
  for (unsigned i = 0; i < 15; ++i)
    assert(WeightCalibration_Sample(state, 1, raw) == WEIGHT_PENDING);
  return WeightCalibration_Sample(state, 1, raw);
}

static void CheckCalibration(void)
{
  WeightCalibration state = {0};
  double grams;
  assert(!WeightCalibration_Grams(&state, 100, &grams) && isnan(grams));
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_NEEDS_TARE);
  const double invalid[] = {0, -10, NAN, INFINITY, -INFINITY};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(WeightCalibration_StartCal(&state, invalid[i]) == WEIGHT_INVALID_MASS);
  assert(WeightCalibration_StartTare(&state) == WEIGHT_PENDING);
  assert(WeightCalibration_StartTare(&state) == WEIGHT_BUSY);
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_BUSY);
  for (unsigned i = 0; i < 15; ++i)
  {
    assert(WeightCalibration_Sample(&state, 1, 1000 + (int)(i % 2)) == WEIGHT_PENDING);
    assert(!state.zero_valid);
  }
  assert(WeightCalibration_Sample(&state, 1, 1001) == WEIGHT_TARE_DONE);
  assert(state.zero == 1000.5 && !state.calibrated);
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_PENDING);
  assert(Collect(&state, 51000) == WEIGHT_CALIBRATION_DONE);
  assert(WeightCalibration_Grams(&state, 51000, &grams) && fabs(grams - 500) < 1e-9);
  assert(WeightCalibration_Grams(&state, -48999, &grams) && fabs(grams + 500) < 1e-9);
  double old_zero = state.zero;
  double old_factor = state.counts_per_gram;

  /* Failed tare/calibration must leave the last good calibration usable. */
  assert(WeightCalibration_StartTare(&state) == WEIGHT_PENDING);
  assert(WeightCalibration_Sample(&state, 1, 99999) == WEIGHT_PENDING);
  assert(state.zero == old_zero);
  assert(WeightCalibration_Sample(&state, 0, 0) == WEIGHT_READ_FAILED);
  assert(state.zero == old_zero && state.counts_per_gram == old_factor && state.calibrated);
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_PENDING);
  assert(WeightCalibration_Sample(&state, 0, 0) == WEIGHT_READ_FAILED);
  assert(state.zero == old_zero && state.counts_per_gram == old_factor && state.calibrated);

  /* Re-tare preserves gain; zero ADC change rejects a new gain. */
  assert(WeightCalibration_StartTare(&state) == WEIGHT_PENDING);
  assert(Collect(&state, 1000) == WEIGHT_TARE_DONE);
  assert(state.counts_per_gram == old_factor && state.calibrated);
  assert(WeightCalibration_Grams(&state, 1000, &grams) && grams == 0);
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_PENDING);
  assert(Collect(&state, 1000) == WEIGHT_NO_CHANGE);
  assert(state.counts_per_gram == old_factor && state.calibrated);

  /* Reversed sensor polarity is supported by a negative slope. */
  assert(WeightCalibration_StartCal(&state, 500) == WEIGHT_PENDING);
  assert(Collect(&state, -49000) == WEIGHT_CALIBRATION_DONE);
  assert(state.counts_per_gram == -100);
  assert(WeightCalibration_Grams(&state, -49000, &grams) && grams == 500);
  assert(WeightCalibration_Grams(&state, 51000, &grams) && grams == -500);

  /* Full int32 averaging/difference do not overflow. No inherited 5 kg limit. */
  WeightCalibration wide = {0};
  assert(WeightCalibration_StartTare(&wide) == WEIGHT_PENDING);
  assert(Collect(&wide, INT32_MIN) == WEIGHT_TARE_DONE);
  assert(wide.zero == INT32_MIN);
  assert(WeightCalibration_StartCal(&wide, 10000) == WEIGHT_PENDING);
  assert(Collect(&wide, INT32_MAX) == WEIGHT_CALIBRATION_DONE);
  assert(WeightCalibration_Grams(&wide, INT32_MAX, &grams) && fabs(grams - 10000) < 1e-8);

  WeightCalibration reboot = {0};
  assert(!WeightCalibration_Grams(&reboot, 0, &grams) && isnan(grams));
}

static void CheckCommands(void)
{
  double mass;
  assert(WeightCommand_Parse("tare", &mass) == WEIGHT_COMMAND_TARE);
  assert(WeightCommand_Parse(" \ttare\r\n", &mass) == WEIGHT_COMMAND_TARE);
  assert(WeightCommand_Parse("help", &mass) == WEIGHT_COMMAND_HELP);
  assert(WeightCommand_Parse("cal 500", &mass) == WEIGHT_COMMAND_CAL && mass == 500);
  assert(WeightCommand_Parse(" cal 12.5 \r\n", &mass) == WEIGHT_COMMAND_CAL && mass == 12.5);
  const char *invalid[] = {"", "t", "cal", "cal500", "cal 0", "cal -5", "cal nan",
                           "cal inf", "cal 1e999", "cal 1e-999", "cal 500g", "cal 500 extra",
                           "tare extra", "help extra", "CAL 500"};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(WeightCommand_Parse(invalid[i], &mass) == WEIGHT_COMMAND_INVALID);
}

static void CheckPreset(void)
{
  WeightCalibration state = {0};
  double grams;
  assert(WeightCalibration_Init(&state, WEIGHT_PROFILE_COUNTS_PER_GRAM));
  assert(state.calibrated && !state.zero_valid && state.operation == WEIGHT_IDLE);
  assert(state.counts_per_gram == 1082.79688 && WEIGHT_PROFILE_REFERENCE_G == 100);
  assert(!WeightCalibration_Grams(&state, 200000, &grams) && isnan(grams));
  assert(WeightCalibration_StartCal(&state, 100) == WEIGHT_NEEDS_TARE);
  assert(WeightCalibration_StartTare(&state) == WEIGHT_PENDING);
  assert(Collect(&state, 10000) == WEIGHT_TARE_DONE);
  assert(WeightCalibration_Grams(&state, 118280, &grams) && fabs(grams - 100) < 0.001);
  assert(WeightCalibration_Grams(&state, 10000, &grams) && grams == 0);
  assert(WeightCalibration_Grams(&state, -98280, &grams) && fabs(grams + 100) < 0.001);
  assert(WeightCalibration_StartCal(&state, 100) == WEIGHT_PENDING);
  assert(Collect(&state, 210000) == WEIGHT_CALIBRATION_DONE);
  assert(state.counts_per_gram == 2000); /* Manual override works this session. */
  assert(WeightCalibration_Init(&state, WEIGHT_PROFILE_COUNTS_PER_GRAM));
  assert(!state.zero_valid && state.counts_per_gram == 1082.79688);
  assert(!WeightCalibration_Grams(&state, 210000, &grams) && isnan(grams));

  const double invalid[] = {0, NAN, INFINITY, -INFINITY};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
  {
    assert(!WeightCalibration_Init(&state, invalid[i]));
    assert(!state.calibrated && !state.zero_valid);
  }
  assert(WeightCalibration_Init(&state, -100)); /* Reversed sensor orientation. */
  assert(WeightCalibration_StartTare(&state) == WEIGHT_PENDING);
  assert(Collect(&state, 1000) == WEIGHT_TARE_DONE);
  assert(WeightCalibration_Grams(&state, -9000, &grams) && grams == 100);
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  if (strcmp(argv[1], "driver") == 0) CheckDriver();
  else if (strcmp(argv[1], "calibration") == 0) CheckCalibration();
  else if (strcmp(argv[1], "commands") == 0) CheckCommands();
  else if (strcmp(argv[1], "preset") == 0) CheckPreset();
  else assert(0 && "unknown test group");
  puts("Weight-I2C checks passed");
  return 0;
}
