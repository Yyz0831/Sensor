#ifndef MINI_SCALES_H
#define MINI_SCALES_H

#include <stdint.h>

typedef enum
{
  MINI_SCALES_OK = 0,
  MINI_SCALES_IO_ERROR = 1,
  MINI_SCALES_INVALID_DATA = 2,
  MINI_SCALES_OUT_OF_RANGE = 3
} MiniScalesStatus;

typedef struct
{
  float weight_g;
  int32_t raw_adc;
  uint8_t button; /* 0: pressed, 1: released, 255: unavailable. */
} MiniScalesSample;

MiniScalesStatus MiniScales_Read(MiniScalesSample *sample);
MiniScalesStatus MiniScales_Tare(void);
MiniScalesStatus MiniScales_GetFirmwareVersion(uint8_t *version);

#endif
