#ifndef WEIGHT_CALIBRATION_H
#define WEIGHT_CALIBRATION_H

#include <stdint.h>

#define WEIGHT_CALIBRATION_SAMPLES 16U

typedef enum { WEIGHT_IDLE, WEIGHT_TARE, WEIGHT_CALIBRATE } WeightOperation;
typedef enum
{
  WEIGHT_PENDING, WEIGHT_TARE_DONE, WEIGHT_CALIBRATION_DONE,
  WEIGHT_BUSY, WEIGHT_NEEDS_TARE, WEIGHT_INVALID_MASS,
  WEIGHT_NO_CHANGE, WEIGHT_READ_FAILED
} WeightResult;

typedef struct
{
  double zero;
  double counts_per_gram;
  uint8_t zero_valid;
  uint8_t calibrated;
  WeightOperation operation;
  unsigned count;
  int64_t sum;
  double reference_g;
} WeightCalibration;

/* Load a preset gain, but never assume a valid zero at startup. Returns 1 if
 * the gain is usable. A zero/invalid preset leaves manual calibration enabled.
 * Pending/failed operations never replace valid values.
 */
int WeightCalibration_Init(WeightCalibration *state, double preset_counts_per_gram);
WeightResult WeightCalibration_StartTare(WeightCalibration *state);
WeightResult WeightCalibration_StartCal(WeightCalibration *state, double mass_g);
WeightResult WeightCalibration_Sample(WeightCalibration *state, int read_ok, int32_t raw);
int WeightCalibration_Grams(const WeightCalibration *state, int32_t raw, double *grams);

#endif
