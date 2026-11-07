#include "weight_adc_stream.h"
#include <stdio.h>

void WeightAdcStream_Init(volatile WeightAdcStream *state, uint32_t now)
{
  state->raw = 0;
  state->read_ms = now;
  state->output_ms = now;
  state->valid = 0; /* Startup silence until an actual successful read. */
}

void WeightAdcStream_Update(volatile WeightAdcStream *state, uint32_t now,
                            int read_ok, int32_t raw)
{
  state->valid = 0;
  if (read_ok)
  {
    state->raw = raw;
    state->read_ms = now;
    state->valid = 1;
  }
}

size_t WeightAdcStream_Tick(volatile WeightAdcStream *state, uint32_t now,
                          int tx_ready, char *line, size_t capacity)
{
  uint32_t elapsed = now - state->output_ms;
  if (elapsed < WEIGHT_ADC_OUTPUT_MS)
    return 0;
  state->output_ms += (elapsed / WEIGHT_ADC_OUTPUT_MS) * WEIGHT_ADC_OUTPUT_MS;
  if (!state->valid || (uint32_t)(now - state->read_ms) >= WEIGHT_ADC_STALE_MS ||
      !tx_ready || line == NULL || capacity < WEIGHT_ADC_LINE_CAPACITY)
    return 0;

  /* No scaling, clipping, zero subtraction, units, or diagnostic text. */
  int length = snprintf(line, capacity, "%ld\n", (long)state->raw);
  return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}
