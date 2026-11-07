#include "weight_adc_stream.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void CheckProtocol(void)
{
  const int32_t values[] = {0, 8769386, -12345, INT32_MIN, INT32_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
  {
    WeightAdcStream state;
    WeightAdcStream_Init(&state, 0);
    WeightAdcStream_Update(&state, 0, 1, values[i]);
    char line[WEIGHT_ADC_LINE_CAPACITY], expected[32];
    snprintf(expected, sizeof(expected), "%ld\n", (long)values[i]);
    size_t length = WeightAdcStream_Tick(&state, 5, 1, line, sizeof(line));
    assert(length == strlen(expected) && strcmp(line, expected) == 0);
    assert(length <= 12);
    /* Max 12 bytes, 8N1: 24,000 bit/s at 200 lines/s fits 115,200 baud. */
    assert(length * 10U * 200U < 115200U);
  }
}

static void CheckTiming(void)
{
  WeightAdcStream state;
  WeightAdcStream_Init(&state, 0);
  char line[WEIGHT_ADC_LINE_CAPACITY];
  unsigned lines = 0, reads = 0;
  uint32_t uart_ready_ms = 0, previous_output_ms = 0;
  for (uint32_t now = 1; now <= 10000; ++now)
  {
    /* 10 Hz foreground reads; output ticks continue during I2C waits. */
    if ((now - 1) % WEIGHT_ADC_POLL_MS == 0)
    {
      ++reads;
      WeightAdcStream_Update(&state, now, 1, (int32_t)reads);
    }
    size_t length = WeightAdcStream_Tick(&state, now, now >= uart_ready_ms,
                                        line, sizeof(line));
    if (length)
    {
      assert(now - previous_output_ms == 5);
      previous_output_ms = now;
      uart_ready_ms = now + (uint32_t)((length * 10U * 1000U + 115199U) / 115200U);
      ++lines;
    }
  }
  assert(reads == 100 && lines == 2000);
  /* A delayed ISR skips slots rather than emitting 20 stale lines at once. */
  WeightAdcStream_Update(&state, 10100, 1, 7);
  assert(WeightAdcStream_Tick(&state, 10100, 1, line, sizeof(line)) == 2);
  assert(WeightAdcStream_Tick(&state, 10100, 1, line, sizeof(line)) == 0);
  assert(WeightAdcStream_Tick(&state, 10104, 1, line, sizeof(line)) == 0);
  assert(WeightAdcStream_Tick(&state, 10105, 1, line, sizeof(line)) == 2);
}

static void CheckFailures(void)
{
  WeightAdcStream state;
  char line[WEIGHT_ADC_LINE_CAPACITY];
  WeightAdcStream_Init(&state, 0);
  for (uint32_t now = 1; now <= 100; ++now)
    assert(!WeightAdcStream_Tick(&state, now, 1, line, sizeof(line)));
  WeightAdcStream_Update(&state, 101, 1, 123);
  assert(WeightAdcStream_Tick(&state, 105, 1, line, sizeof(line)) == 4);
  WeightAdcStream_Update(&state, 106, 0, 0);
  for (uint32_t now = 106; now < 200; ++now)
    assert(!WeightAdcStream_Tick(&state, now, 1, line, sizeof(line)));
  WeightAdcStream_Update(&state, 200, 1, 0); /* Actual zero is a valid reading. */
  assert(WeightAdcStream_Tick(&state, 200, 1, line, sizeof(line)) == 2);
  assert(strcmp(line, "0\n") == 0);
  assert(WeightAdcStream_Tick(&state, 445, 1, line, sizeof(line)) == 2);
  assert(!WeightAdcStream_Tick(&state, 450, 1, line, sizeof(line))); /* Stale. */
  WeightAdcStream_Update(&state, 451, 1, -50);
  assert(WeightAdcStream_Tick(&state, 455, 1, line, sizeof(line)) == 4);
  assert(strcmp(line, "-50\n") == 0);
}

static void CheckWrapAndBusy(void)
{
  WeightAdcStream state;
  char line[WEIGHT_ADC_LINE_CAPACITY] = "in flight\n";
  uint32_t start = UINT32_MAX - 3U;
  WeightAdcStream_Init(&state, start);
  WeightAdcStream_Update(&state, start, 1, INT32_MIN);
  assert(!WeightAdcStream_Tick(&state, start + 4U, 1, line, sizeof(line)));
  assert(!WeightAdcStream_Tick(&state, start + 5U, 0, line, sizeof(line)));
  assert(strcmp(line, "in flight\n") == 0); /* Do not overwrite an active TX buffer. */
  assert(!WeightAdcStream_Tick(&state, start + 6U, 1, line, sizeof(line)));
  assert(WeightAdcStream_Tick(&state, start + 10U, 1, line, sizeof(line)) == 12);
  assert(strcmp(line, "-2147483648\n") == 0);
  assert(!WeightAdcStream_Tick(&state, start + 15U, 1, line, 12));
  assert(!WeightAdcStream_Tick(&state, start + 20U, 1, NULL, 0));
  assert(!WeightAdcStream_Tick(&state, start + 250U, 1, line, sizeof(line)));
}

static void EmitWireFixture(void)
{
  const int32_t values[] = {8769386, -12345, 0, INT32_MIN, INT32_MAX,
                            100, 101, -2, -3, 8769533};
  WeightAdcStream state;
  char line[WEIGHT_ADC_LINE_CAPACITY];
  WeightAdcStream_Init(&state, 0);
  for (uint32_t now = 1; now <= 1000; ++now)
  {
    if ((now - 1) % 100 == 0)
      WeightAdcStream_Update(&state, now, 1, values[(now - 1) / 100]);
    size_t length = WeightAdcStream_Tick(&state, now, 1, line, sizeof(line));
    if (length)
      assert(fwrite(line, 1, length, stdout) == length);
  }
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  if (strcmp(argv[1], "wire") == 0)
  {
    EmitWireFixture();
    return 0;
  }
  if (strcmp(argv[1], "protocol") == 0) CheckProtocol();
  else if (strcmp(argv[1], "timing") == 0) CheckTiming();
  else if (strcmp(argv[1], "failures") == 0) CheckFailures();
  else if (strcmp(argv[1], "wrap_busy") == 0) CheckWrapAndBusy();
  else assert(0 && "unknown group");
  puts("ADC stream checks passed");
  return 0;
}
