#include "temperature_sensors.h"
#include "temperature_conversion.h"
#include "adc.h"
#include "i2c.h"
#include <math.h>

#define MCP9808_ADDRESS       (0x18U << 1)
#define MCP9808_CONFIG        0x01U
#define MCP9808_TEMPERATURE   0x05U
#define MCP9808_MANUFACTURER  0x06U
#define MCP9808_DEVICE_ID     0x07U
#define MCP9808_RESOLUTION    0x08U
#define I2C_TIMEOUT_MS        25U
#define MCP9808_WARMUP_MS     300U
#define TMP36_AVERAGE_COUNT   16U
/* ADC reference is VDDA. Adjust this to the measured 3V3 rail if needed. */
#define ADC_REFERENCE_VOLTS   3.3f
/* Temporary experiment; set to 0.0f to restore the TMP36 reading. */
#define TMP36_TEST_OFFSET_C   50.0f

static uint8_t mcp_initialized;
static uint32_t mcp_started_ms;

static HAL_StatusTypeDef MCP9808_ReadRegister(uint16_t reg, uint16_t *value)
{
  uint8_t bytes[2];
  HAL_StatusTypeDef result = HAL_I2C_Mem_Read(
      &hi2c1, MCP9808_ADDRESS, reg, I2C_MEMADD_SIZE_8BIT,
      bytes, sizeof(bytes), I2C_TIMEOUT_MS);
  if (result == HAL_OK)
  {
    *value = ((uint16_t)bytes[0] << 8) | bytes[1];
  }
  return result;
}

static SensorStatus MCP9808_Initialize(void)
{
  uint16_t manufacturer;
  uint16_t device;
  uint8_t config[2] = {0, 0};
  uint8_t resolution = 3; /* 0.0625 C, nominal conversion time 250 ms. */

  if (MCP9808_ReadRegister(MCP9808_MANUFACTURER, &manufacturer) != HAL_OK ||
      MCP9808_ReadRegister(MCP9808_DEVICE_ID, &device) != HAL_OK)
  {
    return SENSOR_IO_ERROR;
  }
  if (manufacturer != 0x0054U || (device & 0xFF00U) != 0x0400U)
  {
    return SENSOR_ID_ERROR;
  }
  if (HAL_I2C_Mem_Write(&hi2c1, MCP9808_ADDRESS, MCP9808_CONFIG,
                        I2C_MEMADD_SIZE_8BIT, config, sizeof(config),
                        I2C_TIMEOUT_MS) != HAL_OK ||
      HAL_I2C_Mem_Write(&hi2c1, MCP9808_ADDRESS, MCP9808_RESOLUTION,
                        I2C_MEMADD_SIZE_8BIT, &resolution, 1,
                        I2C_TIMEOUT_MS) != HAL_OK)
  {
    return SENSOR_IO_ERROR;
  }
  mcp_initialized = 1;
  mcp_started_ms = HAL_GetTick();
  return SENSOR_WARMING_UP;
}

static SensorStatus MCP9808_ReadTemperature(float *temperature)
{
  uint16_t raw;
  if (!mcp_initialized)
  {
    return MCP9808_Initialize();
  }
  if ((uint32_t)(HAL_GetTick() - mcp_started_ms) < MCP9808_WARMUP_MS)
  {
    return SENSOR_WARMING_UP;
  }
  if (MCP9808_ReadRegister(MCP9808_TEMPERATURE, &raw) != HAL_OK)
  {
    mcp_initialized = 0; /* Retry identification/configuration next sample. */
    return SENSOR_IO_ERROR;
  }
  float value = MCP9808_DecodeTemperature(raw);
  if (value < -40.0f || value > 125.0f)
  {
    return SENSOR_OUT_OF_RANGE;
  }
  *temperature = value;
  return SENSOR_OK;
}

static SensorStatus TMP36_ReadTemperature(float *temperature)
{
  uint32_t sum = 0;
  for (uint32_t i = 0; i < TMP36_AVERAGE_COUNT; ++i)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      (void)HAL_ADC_Stop(&hadc1);
      return SENSOR_IO_ERROR;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK)
    {
      (void)HAL_ADC_Stop(&hadc1);
      return SENSOR_IO_ERROR;
    }
    sum += HAL_ADC_GetValue(&hadc1);
    if (HAL_ADC_Stop(&hadc1) != HAL_OK)
    {
      return SENSOR_IO_ERROR;
    }
  }
  float value = TMP36_ConvertTemperature(
      (float)sum / TMP36_AVERAGE_COUNT, ADC_REFERENCE_VOLTS);
  value += TMP36_TEST_OFFSET_C;
  if (value < -40.0f || value > 125.0f)
  {
    return SENSOR_OUT_OF_RANGE;
  }
  *temperature = value;
  return SENSOR_OK;
}

void TemperatureSensors_Read(TemperatureSample *sample)
{
  sample->time_ms = HAL_GetTick();
  sample->mcp9808_c = NAN;
  sample->tmp36_c = NAN;
  sample->mcp_status = MCP9808_ReadTemperature(&sample->mcp9808_c);
  sample->tmp_status = TMP36_ReadTemperature(&sample->tmp36_c);
}
