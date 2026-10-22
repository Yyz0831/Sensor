#ifndef TEMPERATURE_CONVERSION_H
#define TEMPERATURE_CONVERSION_H

#include <stdint.h>

float MCP9808_DecodeTemperature(uint16_t raw);
float TMP36_ConvertTemperature(float adc_counts, float reference_voltage);

#endif
