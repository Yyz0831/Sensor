#include "temperature_conversion.h"

float MCP9808_DecodeTemperature(uint16_t raw)
{
  /* Bits 15:13 are alert flags; bit 12 is the sign bit. */
  float temperature = (float)(raw & 0x0FFFU) / 16.0f;
  if ((raw & 0x1000U) != 0U)
  {
    temperature -= 256.0f;
  }
  return temperature;
}

float TMP36_ConvertTemperature(float adc_counts, float reference_voltage)
{
  float voltage = adc_counts * reference_voltage / 4095.0f;
  return (voltage - 0.5f) * 100.0f;
}
