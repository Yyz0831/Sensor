#ifndef WEIGHT_ADC_STREAM_H
#define WEIGHT_ADC_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define WEIGHT_ADC_OUTPUT_MS 5U
#define WEIGHT_ADC_POLL_MS 100U
#define WEIGHT_ADC_STALE_MS 250U
/* "-2147483648\n" plus terminating NUL. */
#define WEIGHT_ADC_LINE_CAPACITY 13U

typedef struct
{
  int32_t raw;
  uint32_t read_ms;
  uint32_t output_ms;
  uint8_t valid;
} WeightAdcStream;

void WeightAdcStream_Init(volatile WeightAdcStream *state, uint32_t now);
/* Call with interrupts masked when sharing state with the output ISR. */
void WeightAdcStream_Update(volatile WeightAdcStream *state, uint32_t now,
                            int read_ok, int32_t raw);
/* Returns one complete integer+LF line's length, or zero (no transmission).
 * Missed/busy slots are skipped, never replayed as a catch-up burst.
 */
size_t WeightAdcStream_Tick(volatile WeightAdcStream *state, uint32_t now,
                          int tx_ready, char *line, size_t capacity);

/* Implemented by the dedicated firmware; called only in its SysTick build. */
void WeightWindTunnel_Tick(uint32_t now);

#endif
