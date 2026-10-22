#include "weight_calibration.h"
#include <math.h>

int WeightCalibration_Init(WeightCalibration *state, double preset_counts_per_gram)
{
  *state = (WeightCalibration){0};
  if (!isfinite(preset_counts_per_gram) || preset_counts_per_gram == 0.0 ||
      !isfinite(4294967295.0 / fabs(preset_counts_per_gram)))
    return 0;
  state->counts_per_gram = preset_counts_per_gram;
  state->calibrated = 1;
  return 1;
}

static void Start(WeightCalibration *state, WeightOperation operation)
{
  state->operation = operation;
  state->count = 0;
  state->sum = 0;
}

WeightResult WeightCalibration_StartTare(WeightCalibration *state)
{
  if (state->operation != WEIGHT_IDLE)
    return WEIGHT_BUSY;
  Start(state, WEIGHT_TARE);
  return WEIGHT_PENDING;
}

WeightResult WeightCalibration_StartCal(WeightCalibration *state, double mass_g)
{
  if (state->operation != WEIGHT_IDLE)
    return WEIGHT_BUSY;
  if (!isfinite(mass_g) || mass_g <= 0.0)
    return WEIGHT_INVALID_MASS;
  if (!state->zero_valid)
    return WEIGHT_NEEDS_TARE;
  Start(state, WEIGHT_CALIBRATE);
  state->reference_g = mass_g;
  return WEIGHT_PENDING;
}

WeightResult WeightCalibration_Sample(WeightCalibration *state, int read_ok, int32_t raw)
{
  if (state->operation == WEIGHT_IDLE)
    return WEIGHT_PENDING;
  if (!read_ok)
  {
    state->operation = WEIGHT_IDLE;
    return WEIGHT_READ_FAILED;
  }
  state->sum += raw;
  if (++state->count < WEIGHT_CALIBRATION_SAMPLES)
    return WEIGHT_PENDING;

  double average = (double)state->sum / WEIGHT_CALIBRATION_SAMPLES;
  WeightOperation operation = state->operation;
  state->operation = WEIGHT_IDLE;
  if (operation == WEIGHT_TARE)
  {
    state->zero = average;
    state->zero_valid = 1;
    /* A later tare changes zero only; an existing gain remains usable. */
    return WEIGHT_TARE_DONE;
  }
  double factor = (average - state->zero) / state->reference_g;
  if (!isfinite(factor) || factor == 0.0 ||
      !isfinite(4294967295.0 / fabs(factor)))
    return WEIGHT_NO_CHANGE;
  state->counts_per_gram = factor;
  state->calibrated = 1;
  return WEIGHT_CALIBRATION_DONE;
}

int WeightCalibration_Grams(const WeightCalibration *state, int32_t raw, double *grams)
{
  *grams = NAN;
  if (!state->calibrated || !state->zero_valid)
    return 0;
  *grams = ((double)raw - state->zero) / state->counts_per_gram;
  return isfinite(*grams);
}
