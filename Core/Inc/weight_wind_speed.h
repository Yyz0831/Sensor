#ifndef WEIGHT_WIND_SPEED_H
#define WEIGHT_WIND_SPEED_H

#include <stddef.h>
#include <stdint.h>

#define WEIGHT_SPEED_POLL_MS 100U
#define WEIGHT_SPEED_OUTPUT_MS 5U
#define WEIGHT_SPEED_STALE_MS 250U
#define WEIGHT_SPEED_ZERO_SAMPLES 40U
#define WEIGHT_SPEED_ZERO_SPREAD 500U
#define WEIGHT_SPEED_BUTTON_DEBOUNCE_MS 30U
#define WEIGHT_SPEED_BUTTON_HOLD_MS 1000U
#define WEIGHT_SPEED_LINE_CAPACITY 16U

typedef enum
{
  WEIGHT_SPEED_WAITING,
  WEIGHT_SPEED_ZEROING,
  WEIGHT_SPEED_READY,
  WEIGHT_SPEED_ERROR
} WeightSpeedStatus;

/* Shared with the 5 ms output ISR. Foreground Init/Read/Button calls must not
 * overlap that ISR (initialize before enabling it; mask interrupts for writes).
 * Only the output ISR calls Update, so I2C waits cannot stall the EMA.
 */
typedef struct
{
  WeightSpeedStatus status;
  double zero;
  double speed_mps;
  double filtered_adc;
  int32_t raw_adc;
  int64_t zero_sum;
  int32_t zero_min;
  int32_t zero_max;
  uint32_t zero_count;
  uint32_t zero_sample_ms;
  uint32_t read_ms;
  uint32_t filter_ms;
  uint32_t button_changed_ms;
  uint32_t button_down_ms;
  uint8_t zero_valid;
  uint8_t sample_valid;
  uint8_t filter_valid;
  uint8_t button_raw_down;
  uint8_t button_down;
  uint8_t button_fired;
} WeightWindSpeed;

/* Starts a fresh automatic no-wind zero acquisition on every boot/reset. */
void WeightWindSpeed_Init(volatile WeightWindSpeed *state, uint32_t now);
/* pressed is logical (1 when B1 is down), not the GPIO voltage level.
 * Returns 1 once per debounced long press when a new zero acquisition starts.
 */
int WeightWindSpeed_Button(volatile WeightWindSpeed *state, uint32_t now, int pressed);
/* Call for each actual foreground I2C transaction. Never call per output line.
 * Zeroing uses raw samples; after zeroing, cache the latest raw ADC. This does
 * not advance the EMA. Returns 1 when a valid sample is available for Update.
 * Reset/re-zero, failed reads and stale gaps discard EMA history.
 */
int WeightWindSpeed_Read(volatile WeightWindSpeed *state, uint32_t now,
                         int read_ok, int32_t raw);
/* At each 5 ms output slot, even when UART is busy: update the causal EMA with
 * the latest held raw sample and actual elapsed time, then convert to speed.
 * This never refreshes read_ms: held samples still expire after 250 ms.
 */
int WeightWindSpeed_Update(volatile WeightWindSpeed *state, uint32_t now);
/* Pure conversion. Nonpositive delta yields zero; above the final knot, keep
 * extrapolating. Nonfinite inputs/results and a NULL output pointer fail.
 */
int WeightWindSpeed_Convert(double delta, double *speed_mps);
WeightSpeedStatus WeightWindSpeed_Status(const volatile WeightWindSpeed *state, uint32_t now);
int WeightWindSpeed_LedOn(const volatile WeightWindSpeed *state, uint32_t now);
/* Output-ISR formatter; exactly one nonnegative decimal with 3 digits + LF.
 * Firmware calls formatting only here, never concurrently from foreground.
 */
size_t WeightWindSpeed_Format(double speed_mps, char *line, size_t capacity);

/* ASCII cache owned by the output ISR. Foreground invalidation must be called
 * with interrupts masked. Publish's timestamp is the last actual ADC read,
 * not the formatting time, so repeated output cannot hide a stale sensor.
 */
typedef struct
{
  char line[WEIGHT_SPEED_LINE_CAPACITY];
  size_t length;
  uint32_t read_ms;
  uint32_t output_ms;
  uint8_t valid;
} WeightSpeedStream;

void WeightSpeedStream_Init(volatile WeightSpeedStream *state, uint32_t now);
void WeightSpeedStream_Invalidate(volatile WeightSpeedStream *state);
void WeightSpeedStream_Publish(volatile WeightSpeedStream *state, uint32_t now,
                               const char *line, size_t length);
/* ISR-safe: time/validity checks and bounded character copy only, no formatting.
 * tx_line belongs to the UART until it becomes ready again.
 */
size_t WeightSpeedStream_Tick(volatile WeightSpeedStream *state, uint32_t now,
                             int tx_ready, char *tx_line, size_t capacity);
/* Complete 200 Hz speed-output path: advance EMA, convert, format, then send.
 * Call only from the output ISR. Busy UART skips transmission, not filtering.
 */
size_t WeightWindSpeed_OutputTick(volatile WeightWindSpeed *speed,
                                  volatile WeightSpeedStream *stream, uint32_t now,
                                  int tx_ready, char *tx_line, size_t capacity);

void WeightWindTunnel_Tick(uint32_t now);

#endif
