#ifndef TEMPERATURE_SENSORS_H
#define TEMPERATURE_SENSORS_H

#include <stdint.h>

typedef enum
{
  SENSOR_OK = 0,
  SENSOR_IO_ERROR = 1,
  SENSOR_ID_ERROR = 2,
  SENSOR_WARMING_UP = 3,
  SENSOR_OUT_OF_RANGE = 4
} SensorStatus;

typedef struct
{
  uint32_t time_ms;
  float mcp9808_c;
  float tmp36_c;
  SensorStatus mcp_status;
  SensorStatus tmp_status;
} TemperatureSample;

void TemperatureSensors_Read(TemperatureSample *sample);

#endif
