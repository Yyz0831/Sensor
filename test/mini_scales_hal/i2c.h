/* Minimal host-only HAL interface for testing the actual Mini Scales driver. */
#ifndef MINI_SCALES_TEST_I2C_H
#define MINI_SCALES_TEST_I2C_H
#include <stdint.h>

typedef struct { int unused; } I2C_HandleTypeDef;
typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
#define I2C_MEMADD_SIZE_8BIT 1U
extern I2C_HandleTypeDef hi2c1;
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                  uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *, uint16_t, uint16_t,
                                   uint16_t, uint8_t *, uint16_t, uint32_t);
#endif
