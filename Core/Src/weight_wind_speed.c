#include "weight_wind_speed.h"
#ifdef WEIGHT_WIND_SPEED_BACKUP_PROFILE
#include "weight_wind_speed_profile_backup.h"
#else
#include "weight_wind_speed_profile.h"
#endif
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void BeginZero(volatile WeightWindSpeed *state, uint32_t now)
{
  state->status = WEIGHT_SPEED_ZEROING;
  state->zero_valid = 0;
  state->sample_valid = 0;
  state->filter_valid = 0;
  state->zero_sum = 0;
  state->zero_count = 0;
  state->zero_min = INT32_MAX;
  state->zero_max = INT32_MIN;
  state->zero_sample_ms = now;
}

void WeightWindSpeed_Init(volatile WeightWindSpeed *state, uint32_t now)
{
  memset((void *)state, 0, sizeof(*state)); /* Before the output ISR is enabled. */
  state->button_changed_ms = now;
  state->button_down_ms = now;
  state->read_ms = now;
  BeginZero(state, now);
}

int WeightWindSpeed_Button(volatile WeightWindSpeed *state, uint32_t now, int pressed)
{
  uint8_t down = pressed != 0;
  if (down != state->button_raw_down)
  {
    state->button_raw_down = down;
    state->button_changed_ms = now;
  }
  if (down != state->button_down &&
      (uint32_t)(now - state->button_changed_ms) >= WEIGHT_SPEED_BUTTON_DEBOUNCE_MS)
  {
    state->button_down = down;
    state->button_fired = 0;
    state->button_down_ms = now;
  }
  if (state->button_down && !state->button_fired &&
      (uint32_t)(now - state->button_down_ms) >= WEIGHT_SPEED_BUTTON_HOLD_MS)
  {
    state->button_fired = 1;
    BeginZero(state, now);
    return 1;
  }
  return 0;
}

int WeightWindSpeed_Convert(double delta, double *speed_mps)
{
  if (speed_mps == NULL || !isfinite(delta))
    return 0;
  if (delta <= 0.0)
  {
    *speed_mps = 0.0;
    return 1;
  }
  size_t upper = 1;
  while (upper < WEIGHT_WIND_SPEED_KNOT_COUNT - 1U &&
         delta > WEIGHT_WIND_SPEED_KNOTS[upper].delta_adc)
    ++upper;
  const WeightWindSpeedKnot *left = &WEIGHT_WIND_SPEED_KNOTS[upper - 1U];
  const WeightWindSpeedKnot *right = &WEIGHT_WIND_SPEED_KNOTS[upper];
  /* Above the last knot, continue the final segment without an upper cutoff.
   * No clipping to the 2-10 m/s evaluation band: preserve the fitted model.
   */
  double fraction = (delta - left->delta_adc) / (right->delta_adc - left->delta_adc);
  *speed_mps = left->speed_mps + fraction * (right->speed_mps - left->speed_mps);
  return isfinite(*speed_mps) && *speed_mps >= 0.0;
}

int WeightWindSpeed_Read(volatile WeightWindSpeed *state, uint32_t now,
                         int read_ok, int32_t raw)
{
  if (!state->sample_valid || (uint32_t)(now - state->read_ms) >= WEIGHT_SPEED_STALE_MS)
    state->filter_valid = 0;
  state->sample_valid = 0;
  if (!read_ok)
  {
    state->filter_valid = 0;
    if (state->status == WEIGHT_SPEED_ZEROING)
      state->zero_valid = 0;
    state->status = WEIGHT_SPEED_ERROR;
    return 0;
  }
  state->read_ms = now;
  if (state->status == WEIGHT_SPEED_ZEROING)
  {
    uint32_t elapsed = now - state->zero_sample_ms;
    if (elapsed >= WEIGHT_SPEED_STALE_MS)
    {
      state->status = WEIGHT_SPEED_ERROR;
      return 0;
    }
    if (elapsed < WEIGHT_SPEED_POLL_MS)
      return 0;
    state->zero_sample_ms = now;
    state->zero_sum += raw;
    if (raw < state->zero_min) state->zero_min = raw;
    if (raw > state->zero_max) state->zero_max = raw;
    ++state->zero_count;
    if ((int64_t)state->zero_max - state->zero_min > WEIGHT_SPEED_ZERO_SPREAD)
    {
      state->status = WEIGHT_SPEED_ERROR;
      return 0;
    }
    if (state->zero_count < WEIGHT_SPEED_ZERO_SAMPLES)
      return 0;
    state->zero = (double)state->zero_sum / WEIGHT_SPEED_ZERO_SAMPLES;
    state->zero_valid = 1;
  }
  if (!state->zero_valid)
    return 0;
  state->raw_adc = raw;
  state->sample_valid = 1;
  state->status = WEIGHT_SPEED_READY;
  return 1;
}

int WeightWindSpeed_Update(volatile WeightWindSpeed *state, uint32_t now)
{
  if (!state->zero_valid || !state->sample_valid || state->status != WEIGHT_SPEED_READY)
    return 0;
  if ((uint32_t)(now - state->read_ms) >= WEIGHT_SPEED_STALE_MS)
  {
    state->sample_valid = 0;
    state->filter_valid = 0;
    state->status = WEIGHT_SPEED_ERROR;
    return 0;
  }
  uint32_t elapsed = now - state->filter_ms;
  if (!state->filter_valid || elapsed >= WEIGHT_SPEED_STALE_MS)
  {
    /* Seed from the held measurement after tare/failure/gap, never an old run. */
    state->filtered_adc = (double)state->raw_adc;
  }
  else
  {
    /* The normal 5 ms coefficient is constant-folded by the compiler. Use
     * actual dt after a delayed tick; never replay missed slots in a burst.
     */
    double alpha = elapsed == WEIGHT_SPEED_OUTPUT_MS
        ? 1.0 - exp(-(double)WEIGHT_SPEED_OUTPUT_MS / WEIGHT_WIND_SPEED_EMA_TAU_MS)
        : 1.0 - exp(-(double)elapsed / WEIGHT_WIND_SPEED_EMA_TAU_MS);
    /* Increment form keeps a constant input exact. */
    state->filtered_adc += alpha * ((double)state->raw_adc - state->filtered_adc);
  }
  state->filter_ms = now;
  state->filter_valid = 1;
  double converted;
  if (!WeightWindSpeed_Convert(state->filtered_adc - state->zero, &converted))
  {
    state->filter_valid = 0;
    state->status = WEIGHT_SPEED_ERROR;
    return 0;
  }
  state->speed_mps = converted;
  return 1;
}

WeightSpeedStatus WeightWindSpeed_Status(const volatile WeightWindSpeed *state, uint32_t now)
{
  if (state->status == WEIGHT_SPEED_READY &&
      (!state->sample_valid || (uint32_t)(now - state->read_ms) >= WEIGHT_SPEED_STALE_MS))
    return WEIGHT_SPEED_ERROR;
  if (state->status == WEIGHT_SPEED_ZEROING &&
      (uint32_t)(now - state->zero_sample_ms) >= WEIGHT_SPEED_STALE_MS)
    return WEIGHT_SPEED_ERROR;
  return state->status;
}

int WeightWindSpeed_LedOn(const volatile WeightWindSpeed *state, uint32_t now)
{
  switch (WeightWindSpeed_Status(state, now))
  {
    case WEIGHT_SPEED_WAITING: return 0;
    case WEIGHT_SPEED_ZEROING: return (now / 100U) % 2U == 0;
    case WEIGHT_SPEED_READY: return 1;
    case WEIGHT_SPEED_ERROR:
    {
      uint32_t phase = now % 1000U;
      return phase < 100U || (phase >= 200U && phase < 300U);
    }
    default: return 0;
  }
}

size_t WeightWindSpeed_Format(double speed_mps, char *line, size_t capacity)
{
  if (line == NULL || capacity < WEIGHT_SPEED_LINE_CAPACITY ||
      !isfinite(speed_mps) || speed_mps < 0.0)
    return 0;
  if (speed_mps == 0.0) speed_mps = 0.0; /* Normalize negative floating-point zero. */
  int length = snprintf(line, capacity, "%.3f\n", speed_mps);
  return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}

void WeightSpeedStream_Init(volatile WeightSpeedStream *state, uint32_t now)
{
  state->valid = 0;
  state->length = 0;
  state->read_ms = now;
  state->output_ms = now;
}

void WeightSpeedStream_Invalidate(volatile WeightSpeedStream *state)
{
  state->valid = 0;
}

void WeightSpeedStream_Publish(volatile WeightSpeedStream *state, uint32_t now,
                               const char *line, size_t length)
{
  state->valid = 0;
  if (line == NULL || length == 0 || length >= WEIGHT_SPEED_LINE_CAPACITY ||
      line[length - 1] != '\n')
    return;
  for (size_t i = 0; i < length; ++i)
    state->line[i] = line[i];
  state->line[length] = '\0';
  state->length = length;
  state->read_ms = now;
  state->valid = 1;
}

size_t WeightSpeedStream_Tick(volatile WeightSpeedStream *state, uint32_t now,
                             int tx_ready, char *tx_line, size_t capacity)
{
  uint32_t elapsed = now - state->output_ms;
  if (elapsed < WEIGHT_SPEED_OUTPUT_MS)
    return 0;
  state->output_ms += (elapsed / WEIGHT_SPEED_OUTPUT_MS) * WEIGHT_SPEED_OUTPUT_MS;
  if (!state->valid || (uint32_t)(now - state->read_ms) >= WEIGHT_SPEED_STALE_MS ||
      !tx_ready || tx_line == NULL || capacity <= state->length)
    return 0;
  for (size_t i = 0; i < state->length; ++i)
    tx_line[i] = state->line[i];
  tx_line[state->length] = '\0';
  return state->length;
}

size_t WeightWindSpeed_OutputTick(volatile WeightWindSpeed *speed,
                                  volatile WeightSpeedStream *stream, uint32_t now,
                                  int tx_ready, char *tx_line, size_t capacity)
{
  if ((uint32_t)(now - stream->output_ms) >= WEIGHT_SPEED_OUTPUT_MS)
  {
    char prepared_line[WEIGHT_SPEED_LINE_CAPACITY];
    int valid = WeightWindSpeed_Update(speed, now);
    size_t length = valid ? WeightWindSpeed_Format(speed->speed_mps,
                                    prepared_line, sizeof(prepared_line)) : 0;
    if (length)
      WeightSpeedStream_Publish(stream, speed->read_ms, prepared_line, length);
    else
      WeightSpeedStream_Invalidate(stream);
  }
  return WeightSpeedStream_Tick(stream, now, tx_ready, tx_line, capacity);
}
