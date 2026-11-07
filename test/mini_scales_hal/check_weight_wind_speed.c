#include "weight_wind_speed.h"
#include "weight_wind_speed_profile.h"
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Frozen references from the final_delta_knots.csv calibration artifact,
 * evaluated independently with 60-digit Decimal interpolation, not the firmware
 * table or conversion function. Low-segment values test policy, not accuracy.
 * Source: outputs/wind-tunnel-15cm-delta-lut-20261105/final_delta_knots.csv
 * SHA-256: ed787f282d571c2140e3a4aed859307c6ef58a879005edf54a5f83b1f274d196
 */
/* State-machine checks explicitly request an estimate after a read. The
 * firmware-cadence checks below use Read and OutputTick separately instead.
 */
static int ReadAndUpdate(WeightWindSpeed *state, uint32_t now, int ok, int32_t raw)
{
  return WeightWindSpeed_Read(state, now, ok, raw) && WeightWindSpeed_Update(state, now);
}

static const struct { double delta; double speed; } known_points[] = {
  {0.000000000000000000000000, 0.000000000000000000000000},
  {1.000000000000000000000000, 0.000112719502246164282767},
  {100.000000000000000000000000, 0.011271950224616428276685},
  {1000.000000000000000000000000, 0.112719502246164282766846},
  {10000.000000000000000000000000, 1.127195022461642827668457},
  {25000.000000000000000000000000, 2.234842112589940444188488},
  {100000.000000000000000000000000, 4.484328624926087906892119},
  {250000.000000000000000000000000, 7.468492698408698635663963},
  {422007.000000000000000000000000, 10.126261572434464868298621},
  {1000000.000000000000000000000000, 19.686154966776988080357181},
};

static const struct { double delta; double speed; double below; double above; } reference_knots[] = {
  {0.000000000000000000000000, 0.000000000000000000000000, 0.000000000000000000000000, 0.000112719502246164282767},
  {19399.077995505184000000000000, 2.186654415687862700000000, 2.186541696185616535717233, 2.186663019217221510292800},
  {34396.109315930570000000000000, 2.315681814948140000000000, 2.315673211418781189707200, 2.315716991547445348014064},
  {53658.567072516270000000000000, 2.993269573087748000000000, 2.993234396488442651985936, 2.993306776092802402891299},
  {77186.451265262280000000000000, 3.868577567629883700000000, 3.868540364624829297108701, 3.868604558216243401801907},
  {104979.761894168610000000000000, 4.618735318381398000000000, 4.618708327795038298198093, 4.618763428808447755261538},
  {137038.498959235240000000000000, 5.519920107956238600000000, 5.519891997529188844738462, 5.519940367317265326624682},
  {173362.662460462160000000000000, 6.255824450321441000000000, 6.255804190960414273375318, 6.255838992359937591125121},
  {213952.252397849400000000000000, 6.846079829751774000000000, 6.846065287713277408874879, 6.846097096097386667118008},
  {258807.268771396980000000000000, 7.620562044919289000000000, 7.620544778573676332881992, 7.620572999823089899098690},
  {307927.711581104800000000000000, 8.158671770557204000000000, 8.158660815653403100901310, 8.158689822755282999107891},
  {361313.580826973140000000000000, 9.122404056803166000000000, 9.122386004605087000892109, 9.122420596611423786034108},
  {418964.876509001500000000000000, 10.075945433196843000000000, 10.075928893388585213965892, 10.075961973005100786034108},
};

static const struct { double delta; double speed; } reference_midpoints[] = {
  {9699.538997752592000000000000, 1.093327207843931350000000},
  {26897.593655717877000000000000, 2.251168115318001350000000},
  {44027.338194223420000000000000, 2.654475694017944000000000},
  {65422.509168889275000000000000, 3.430923570358815850000000},
  {91083.106579715445000000000000, 4.243656443005640850000000},
  {121009.130426701925000000000000, 5.069327713168818300000000},
  {155200.580709848700000000000000, 5.887872279138839800000000},
  {193657.457429155780000000000000, 6.550952140036607500000000},
  {236379.760584623190000000000000, 7.233320937335531500000000},
  {283367.490176250890000000000000, 7.889616907738246500000000},
  {334620.646204038970000000000000, 8.640537913680185000000000},
  {390139.228667987320000000000000, 9.599174745000004500000000},
};

static uint32_t StartZero(WeightWindSpeed *state, uint32_t start)
{
  assert(!WeightWindSpeed_Button(state, start, 1));
  uint32_t down = start + WEIGHT_SPEED_BUTTON_DEBOUNCE_MS;
  assert(!WeightWindSpeed_Button(state, down, 1));
  assert(!WeightWindSpeed_Button(state, down + WEIGHT_SPEED_BUTTON_HOLD_MS - 1U, 1));
  uint32_t trigger = down + WEIGHT_SPEED_BUTTON_HOLD_MS;
  assert(WeightWindSpeed_Button(state, trigger, 1));
  assert(WeightWindSpeed_Status(state, trigger) == WEIGHT_SPEED_ZEROING);
  assert(!state->zero_valid && !state->sample_valid && !state->filter_valid);
  return trigger;
}

static void ReleaseButton(WeightWindSpeed *state, uint32_t now)
{
  assert(!WeightWindSpeed_Button(state, now, 0));
  assert(!WeightWindSpeed_Button(state, now + WEIGHT_SPEED_BUTTON_DEBOUNCE_MS, 0));
}

static uint32_t Calibrate(WeightWindSpeed *state, uint32_t start, int32_t raw)
{
  WeightWindSpeed_Init(state, start);
  assert(WeightWindSpeed_Status(state, start) == WEIGHT_SPEED_ZEROING);
  uint32_t trigger = start;
  assert(!ReadAndUpdate(state, trigger, 1, raw));
  assert(!ReadAndUpdate(state, trigger + WEIGHT_SPEED_POLL_MS - 1U, 1, raw));
  assert(state->zero_count == 0);
  for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
  {
    uint32_t now = trigger + i * WEIGHT_SPEED_POLL_MS;
    int valid = ReadAndUpdate(state, now, 1, raw);
    assert(valid == (i == WEIGHT_SPEED_ZERO_SAMPLES));
    if (i < WEIGHT_SPEED_ZERO_SAMPLES)
      assert(!state->zero_valid && !state->sample_valid);
  }
  assert(state->zero_valid);
  assert(fabs(state->zero - raw) < 1e-9);
  assert(state->filter_valid && state->filtered_adc == raw);
  return trigger + WEIGHT_SPEED_ZERO_SAMPLES * WEIGHT_SPEED_POLL_MS;
}

static void CheckConversion(void)
{
  for (unsigned i = 0; i < sizeof(known_points) / sizeof(known_points[0]); ++i)
  {
    double actual = -1;
    assert(WeightWindSpeed_Convert(known_points[i].delta, &actual));
    assert(fabs(actual - known_points[i].speed) < 1e-10);
  }
  const double negative[] = {-DBL_MAX, -4294967295.0, -1000000.0, -5001.0, -5000.0, -0.25};
  double value = -1;
  for (unsigned i = 0; i < sizeof(negative) / sizeof(negative[0]); ++i)
    assert(WeightWindSpeed_Convert(negative[i], &value) && value == 0.0);
  assert(!WeightWindSpeed_Convert(NAN, &value));
  assert(!WeightWindSpeed_Convert(INFINITY, &value));
  assert(!WeightWindSpeed_Convert(-INFINITY, &value));
  assert(!WeightWindSpeed_Convert(1, NULL));
  double previous = -1;
  for (int i = 0; i <= 1000; ++i)
  {
    assert(WeightWindSpeed_Convert(1000000.0 * i / 1000.0, &value));
    assert(value >= previous);
    previous = value;
  }
}

static void CheckLutKnotsAndSegments(void)
{
  /* Each archived knot, both neighbouring inputs, and every interval midpoint
   * must agree with frozen independent references. Do not build expected
   * outputs from the production table, which could conceal a wrong profile.
   */
  assert(WEIGHT_WIND_SPEED_KNOT_COUNT == 13U);
  for (unsigned i = 0; i < sizeof(reference_knots) / sizeof(reference_knots[0]); ++i)
  {
    double delta = reference_knots[i].delta;
    assert(WEIGHT_WIND_SPEED_KNOTS[i].delta_adc == delta);
    assert(WEIGHT_WIND_SPEED_KNOTS[i].speed_mps == reference_knots[i].speed);
    double at, below, above, adjacent;
    assert(WeightWindSpeed_Convert(delta, &at));
    assert(WeightWindSpeed_Convert(delta - 1.0, &below));
    assert(WeightWindSpeed_Convert(delta + 1.0, &above));
    assert(fabs(at - reference_knots[i].speed) < 1e-10);
    assert(fabs(below - reference_knots[i].below) < 1e-10);
    assert(fabs(above - reference_knots[i].above) < 1e-10);
    assert(below <= at && at <= above);
    assert(WeightWindSpeed_Convert(nextafter(delta, -INFINITY), &adjacent));
    assert(fabs(adjacent - reference_knots[i].speed) < 1e-10);
    assert(WeightWindSpeed_Convert(nextafter(delta, INFINITY), &adjacent));
    assert(fabs(adjacent - reference_knots[i].speed) < 1e-10);
  }
  for (unsigned i = 0; i < sizeof(reference_midpoints) / sizeof(reference_midpoints[0]); ++i)
  {
    double actual;
    assert(WeightWindSpeed_Convert(reference_midpoints[i].delta, &actual));
    assert(fabs(actual - reference_midpoints[i].speed) < 1e-10);
  }
  double value;
  assert(WeightWindSpeed_Convert(430000.0, &value));
  assert(fabs(value - 10.258464259838948638922113) < 1e-10);
  assert(value > reference_knots[12].speed); /* Extend last segment; no plateau. */
  assert(WeightWindSpeed_Convert(nextafter(-5000.0, -INFINITY), &value) && value == 0.0);
  assert(WeightWindSpeed_Convert(nextafter(535293.5870245794, INFINITY), &value));
  assert(value >= 12.0);
}

static void CheckButton(void)
{
  WeightWindSpeed state;
  WeightWindSpeed_Init(&state, 0);
  assert(WeightWindSpeed_Status(&state, 0) == WEIGHT_SPEED_ZEROING);
  assert(!ReadAndUpdate(&state, 100, 1, 8754000));
  assert(!state.zero_valid && !state.sample_valid);
  /* A sub-debounce pulse cannot restart the automatic zero acquisition. */
  assert(!WeightWindSpeed_Button(&state, 200, 1));
  assert(!WeightWindSpeed_Button(&state, 210, 0));
  assert(!WeightWindSpeed_Button(&state, 250, 0));
  assert(state.zero_count == 1);
  uint32_t trigger = StartZero(&state, 300);
  for (uint32_t n = 0; n < 6000; ++n)
    assert(!WeightWindSpeed_Button(&state, trigger + n, 1));
  ReleaseButton(&state, trigger + 6000);
  (void)StartZero(&state, trigger + 6100);
  /* The long-press timers use unsigned differences across tick wrap. */
  WeightWindSpeed_Init(&state, UINT32_MAX - 10U);
  (void)StartZero(&state, UINT32_MAX - 10U);
}

static void CheckAutomaticStartupAndReset(void)
{
  assert(WEIGHT_SPEED_ZERO_SAMPLES * WEIGHT_SPEED_POLL_MS == 4000U);
  WeightWindSpeed state;
  WeightSpeedStream stream;
  const int32_t baselines[] = {8754000, 8764000};
  const int32_t delta = 25000;
  char prepared[WEIGHT_SPEED_LINE_CAPACITY], tx[WEIGHT_SPEED_LINE_CAPACITY];
  for (uint32_t run = 0; run < 2; ++run)
  {
    uint32_t start = run * 10000U;
    /* Reset must discard the previous run's nonzero sample and calibration. */
    WeightWindSpeed_Init(&state, start);
    WeightSpeedStream_Init(&stream, start);
    assert(WeightWindSpeed_Status(&state, start) == WEIGHT_SPEED_ZEROING);
    assert(!state.zero_valid && !state.sample_valid && !state.filter_valid && state.zero_count == 0);
    for (uint32_t elapsed = WEIGHT_SPEED_OUTPUT_MS;
         elapsed <= WEIGHT_SPEED_ZERO_SAMPLES * WEIGHT_SPEED_POLL_MS;
         elapsed += WEIGHT_SPEED_OUTPUT_MS)
    {
      uint32_t now = start + elapsed;
      if (elapsed % WEIGHT_SPEED_POLL_MS == 0)
      {
        uint32_t count = elapsed / WEIGHT_SPEED_POLL_MS;
        int32_t raw = baselines[run] + (count % 2 ? -50 : 50);
        int valid = ReadAndUpdate(&state, now, 1, raw);
        assert(valid == (count == WEIGHT_SPEED_ZERO_SAMPLES));
        assert(state.zero_count == count);
        if (valid)
        {
          size_t length = WeightWindSpeed_Format(state.speed_mps, prepared, sizeof(prepared));
          assert(length > 0);
          WeightSpeedStream_Publish(&stream, now, prepared, length);
        }
        else
        {
          assert(!state.zero_valid && !state.sample_valid && !state.filter_valid);
          WeightSpeedStream_Invalidate(&stream);
        }
      }
      size_t sent = WeightSpeedStream_Tick(&stream, now, 1, tx, sizeof(tx));
      assert((sent > 0) == (elapsed == WEIGHT_SPEED_ZERO_SAMPLES * WEIGHT_SPEED_POLL_MS));
    }
    assert(state.zero_valid && fabs(state.zero - baselines[run]) < 1e-9);
    assert(state.filtered_adc == baselines[run] + 50);
    uint32_t next = start + (WEIGHT_SPEED_ZERO_SAMPLES + 1U) * WEIGHT_SPEED_POLL_MS;
    assert(ReadAndUpdate(&state, next, 1, baselines[run] + delta));
    /* Fresh EMA seeded with the 40th raw tare sample (baseline + 50). */
    assert(fabs(state.filtered_adc - baselines[run] - 9867.060040169796081) < 1e-7);
    assert(fabs(state.speed_mps - 1.112210096360957167507277) < 1e-10);
  }
}

static void CheckAutomaticStartupFailures(void)
{
  for (int unstable = 0; unstable <= 1; ++unstable)
  {
    WeightWindSpeed state;
    WeightWindSpeed_Init(&state, 0);
    assert(!ReadAndUpdate(&state, 100, 1, 8754000));
    /* Neither a failed read nor excessive motion may yield a startup zero. */
    assert(!ReadAndUpdate(&state, 200, unstable, 8754501));
    assert(WeightWindSpeed_Status(&state, 200) == WEIGHT_SPEED_ERROR);
    for (uint32_t now = 300; now <= 5000; now += WEIGHT_SPEED_POLL_MS)
    {
      assert(!ReadAndUpdate(&state, now, 1, 8754000));
      assert(!state.zero_valid && !state.sample_valid);
    }
  }
}

static void CheckZero(void)
{
  WeightWindSpeed state;
  uint32_t ready = Calibrate(&state, 0, 8754000);
  assert(ReadAndUpdate(&state, ready + 100, 1, 8754000));
  assert(state.speed_mps == 0);
  assert(WeightWindSpeed_Status(&state, ready + 100) == WEIGHT_SPEED_READY);
  /* Large absolute counts require a 64-bit calibration sum. */
  (void)Calibrate(&state, 0, INT32_MAX - 10);
  WeightWindSpeed_Init(&state, 0);
  uint32_t trigger = 0;
  for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
  {
    uint32_t now = trigger + i * 100U;
    (void)ReadAndUpdate(&state, now, 1, 8754000 + (i % 2 ? 0 : 500));
    if (i < WEIGHT_SPEED_ZERO_SAMPLES)
    {
      uint32_t count = state.zero_count;
      (void)ReadAndUpdate(&state, now + 1U, 1, 8754500);
      assert(state.zero_count == count); /* Held 200 Hz output cannot speed up zeroing. */
    }
  }
  assert(state.zero_valid);
  assert(fabs(state.zero - 8754250.0) < 1e-9);

  WeightWindSpeed_Init(&state, 0);
  trigger = 0;
  for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
    (void)ReadAndUpdate(&state, trigger + i * 100U, 1,
                              8754000 + (i % 2 ? 0 : 501));
  assert(!state.zero_valid && !state.sample_valid);
  assert(WeightWindSpeed_Status(&state, trigger + 4000) == WEIGHT_SPEED_ERROR);
  assert(!ReadAndUpdate(&state, trigger + 4100, 1, 8754000));
  /* Failed recalibration also leaves the stream unready, not a stale old zero. */
  ready = Calibrate(&state, 0, 8754000);
  ReleaseButton(&state, ready + 100);
  trigger = StartZero(&state, ready + 200);
  assert(!ReadAndUpdate(&state, trigger + 100, 1, 8754000));
  assert(!ReadAndUpdate(&state, trigger + 200, 0, 0));
  assert(!state.zero_valid && !state.sample_valid);
  assert(WeightWindSpeed_Status(&state, trigger + 200) == WEIGHT_SPEED_ERROR);
  assert(!ReadAndUpdate(&state, trigger + 300, 1, 8754000));

  /* B1 remains a way to recover and establish a new baseline without reset. */
  ReleaseButton(&state, trigger + 400);
  trigger = StartZero(&state, trigger + 500);
  for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
    assert(ReadAndUpdate(&state, trigger + i * WEIGHT_SPEED_POLL_MS,
                               1, 8740000) == (i == WEIGHT_SPEED_ZERO_SAMPLES));
  assert(state.zero_valid && fabs(state.zero - 8740000.0) < 1e-9);
  assert(state.speed_mps == 0);
}

static void CheckRecoveryAndFormatting(void)
{
  WeightWindSpeed state;
  uint32_t now = Calibrate(&state, 0, 8754000) + 100;
  const int32_t delta = 25000;
  assert(ReadAndUpdate(&state, now, 1, 8754000 + delta));
  assert(fabs(state.speed_mps - 1.108791704657964883135205) < 1e-10);
  assert(state.sample_valid);
  assert(WeightWindSpeed_Status(&state, now + WEIGHT_SPEED_STALE_MS - 1) == WEIGHT_SPEED_READY);
  assert(WeightWindSpeed_Status(&state, now + WEIGHT_SPEED_STALE_MS) == WEIGHT_SPEED_ERROR);
  assert(!ReadAndUpdate(&state, now + 300, 0, 0));
  assert(!state.sample_valid && state.zero_valid && !state.filter_valid);
  assert(WeightWindSpeed_Status(&state, now + 300) == WEIGHT_SPEED_ERROR);
  assert(ReadAndUpdate(&state, now + 400, 1, 8754000 + delta));
  assert(WeightWindSpeed_Status(&state, now + 400) == WEIGHT_SPEED_READY);
  assert(fabs(state.speed_mps - 2.234842112589940444188488) < 1e-10);
  assert(state.filter_valid && state.filtered_adc == 8754000 + delta);
  /* A real 250 ms data gap still reseeds rather than retaining stale history. */
  assert(WeightWindSpeed_Status(&state, now + 650) == WEIGHT_SPEED_ERROR);
  assert(ReadAndUpdate(&state, now + 650, 1, 8754000));
  assert(state.sample_valid && state.filter_valid && state.zero_valid);
  assert(state.speed_mps == 0 && state.filtered_adc == 8754000);

  char line[WEIGHT_SPEED_LINE_CAPACITY];
  assert(WeightWindSpeed_Format(5.125, line, sizeof(line)) == 6);
  assert(strcmp(line, "5.125\n") == 0);
  assert(WeightWindSpeed_Format(0, line, sizeof(line)) == 6);
  assert(strcmp(line, "0.000\n") == 0);
  assert(!WeightWindSpeed_Format(NAN, line, sizeof(line)));
  assert(!WeightWindSpeed_Format(INFINITY, line, sizeof(line)));
  assert(!WeightWindSpeed_Format(-1, line, sizeof(line)));
  assert(!WeightWindSpeed_Format(1, NULL, 0));
  assert(!WeightWindSpeed_Format(1, line, 6));
  assert(WeightWindSpeed_Format(-0.0, line, sizeof(line)) == 6);
  assert(strcmp(line, "0.000\n") == 0);
}

static void CheckStatusAndZeroInterruption(void)
{
  WeightWindSpeed state;
  WeightWindSpeed_Init(&state, 0);
  assert(WeightWindSpeed_Status(&state, 0) == WEIGHT_SPEED_ZEROING);
  assert(WeightWindSpeed_LedOn(&state, 0));
  uint32_t trigger = 0;
  assert(!ReadAndUpdate(&state, trigger + 100U, 1, 8754000));
  int first = WeightWindSpeed_LedOn(&state, trigger + 100U);
  assert(!ReadAndUpdate(&state, trigger + 200U, 1, 8754000));
  assert(WeightWindSpeed_LedOn(&state, trigger + 200U) != first);
  uint32_t expired = trigger + 200U + WEIGHT_SPEED_STALE_MS;
  assert(WeightWindSpeed_Status(&state, expired) == WEIGHT_SPEED_ERROR);
  assert(!ReadAndUpdate(&state, expired, 1, 8754000));
  assert(!state.zero_valid && !state.sample_valid);
  assert(WeightWindSpeed_Status(&state, expired) == WEIGHT_SPEED_ERROR);
  /* ERROR is two 100 ms pulses each second, distinguishable from zeroing. */
  uint32_t cycle = ((expired / 1000U) + 1U) * 1000U;
  assert(WeightWindSpeed_LedOn(&state, cycle));
  assert(!WeightWindSpeed_LedOn(&state, cycle + 100U));
  assert(WeightWindSpeed_LedOn(&state, cycle + 200U));
  assert(!WeightWindSpeed_LedOn(&state, cycle + 300U));
  assert(!WeightWindSpeed_LedOn(&state, cycle + 900U));
  uint32_t ready = Calibrate(&state, 0, 8754000);
  assert(ReadAndUpdate(&state, ready + 100U, 1, 8754000));
  assert(WeightWindSpeed_LedOn(&state, ready + 100U));
  assert(WeightWindSpeed_LedOn(&state, ready + 200U));
}

static void CheckReadCompletionSchedule(void)
{
  /* Firmware waits 100 ms after the previous completed I2C transaction.
   * Vary transaction latency instead of assuming an exact 100 ms completion
   * cadence: no valid zero sample should be skipped by the minimum-spacing gate.
   */
  for (uint32_t short_transaction_ms = 0; short_transaction_ms <= 1; ++short_transaction_ms)
  {
    WeightWindSpeed state;
    WeightWindSpeed_Init(&state, 0);
    uint32_t trigger = 0;
    uint32_t completed = trigger;
    for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
    {
      uint32_t transaction_ms = i % 2 ? short_transaction_ms : 2U;
      completed += WEIGHT_SPEED_POLL_MS + transaction_ms;
      (void)ReadAndUpdate(&state, completed, 1, 8754000 + (int32_t)(i % 2));
      assert(state.zero_count == i);
      assert(state.zero_valid == (i == WEIGHT_SPEED_ZERO_SAMPLES));
    }
    uint32_t duration = completed - trigger;
    assert(duration >= 4000U && duration <= 4200U);
    assert(duration == WEIGHT_SPEED_ZERO_SAMPLES * WEIGHT_SPEED_POLL_MS +
                       (WEIGHT_SPEED_ZERO_SAMPLES / 2U) * (short_transaction_ms + 2U));
    assert(WeightWindSpeed_Status(&state, completed) == WEIGHT_SPEED_READY);
    assert(fabs(state.zero - 8754000.5) < 1e-9);
  }
}

static void PublishSpeed(WeightSpeedStream *stream, uint32_t now, double speed)
{
  char prepared[WEIGHT_SPEED_LINE_CAPACITY];
  size_t length = WeightWindSpeed_Format(speed, prepared, sizeof(prepared));
  assert(length > 0);
  WeightSpeedStream_Publish(stream, now, prepared, length);
}

static void CheckOnlineEma(void)
{
  const uint32_t starts[] = {0U, UINT32_MAX - 4050U};
  for (unsigned run = 0; run < sizeof(starts) / sizeof(starts[0]); ++run)
  {
    WeightWindSpeed state;
    uint32_t now = Calibrate(&state, starts[run], 8754000);
    WeightSpeedStream stream;
    WeightSpeedStream_Init(&stream, now);
    char tx[WEIGHT_SPEED_LINE_CAPACITY] = "in flight\n";
    assert(WeightWindSpeed_Read(&state, now + 1, 1, 8779000));
    assert(state.filtered_adc == 8754000 && state.filter_ms == now);
    assert(!WeightWindSpeed_OutputTick(&state, &stream, now + 4, 1, tx, sizeof(tx)));
    for (uint32_t dt = 5; dt <= 200; dt += 5)
    {
      if (dt == 105)
        assert(WeightWindSpeed_Read(&state, now + 101, 1, 8779000));
      char previous[sizeof(tx)];
      memcpy(previous, tx, sizeof(tx));
      int busy = dt == 10 || dt == 15;
      size_t length = WeightWindSpeed_OutputTick(&state, &stream, now + dt, !busy, tx, sizeof(tx));
      assert((length > 0) == !busy);
      if (busy) assert(memcmp(previous, tx, sizeof(tx)) == 0);
      /* Independent closed-form step response, not another EMA recurrence. */
      double expected = 25000.0 * (1.0 - exp(-(double)dt / 200.0));
      assert(fabs(state.filtered_adc - 8754000 - expected) < 1e-7);
      assert(state.filter_ms == now + dt);
      assert(state.read_ms == now + (dt < 105 ? 1U : 101U));
      assert(stream.read_ms == state.read_ms);
      if (dt == 5) assert(fabs(state.filtered_adc - 8754000 - 617.252199291683) < 1e-7);
      if (dt == 100) assert(fabs(state.filtered_adc - 8754000 - 9836.733507184164) < 1e-7);
    }
    /* Delayed ticks use their real dt, skip missed slots, and do not burst. */
    assert(WeightWindSpeed_OutputTick(&state, &stream, now + 217, 1, tx, sizeof(tx)));
    assert(fabs(state.filtered_adc - 8754000 - 25000.0 * (1.0 - exp(-217.0 / 200.0))) < 1e-7);
    assert(!WeightWindSpeed_OutputTick(&state, &stream, now + 217, 1, tx, sizeof(tx)));
    assert(WeightWindSpeed_OutputTick(&state, &stream, now + 220, 1, tx, sizeof(tx)));
    assert(fabs(state.filtered_adc - 8754000 - 25000.0 * (1.0 - exp(-220.0 / 200.0))) < 1e-7);
  }
}

static void CheckOutputFreshness(void)
{
  WeightWindSpeed state;
  uint32_t now = Calibrate(&state, 0, 8754000);
  WeightSpeedStream stream;
  WeightSpeedStream_Init(&stream, now);
  char tx[WEIGHT_SPEED_LINE_CAPACITY];
  for (uint32_t dt = 5; dt <= 345; dt += 5)
  {
    if (dt == 100) assert(WeightWindSpeed_Read(&state, now + dt, 1, 8779000));
    assert(WeightWindSpeed_OutputTick(&state, &stream, now + dt, 1, tx, sizeof(tx)));
  }
  assert(state.read_ms == now + 100 && stream.read_ms == now + 100);
  assert(!WeightWindSpeed_OutputTick(&state, &stream, now + 350, 1, tx, sizeof(tx)));
  assert(!state.filter_valid && !state.sample_valid);
  assert(WeightWindSpeed_Status(&state, now + 350) == WEIGHT_SPEED_ERROR);
  assert(WeightWindSpeed_Read(&state, now + 351, 1, 8854000));
  assert(!state.filter_valid);
  assert(WeightWindSpeed_OutputTick(&state, &stream, now + 355, 1, tx, sizeof(tx)));
  assert(state.filtered_adc == 8854000); /* No old EMA after expiry. */
  assert(!WeightWindSpeed_Read(&state, now + 360, 0, 0));
  assert(!WeightWindSpeed_OutputTick(&state, &stream, now + 360, 1, tx, sizeof(tx)));
  assert(WeightWindSpeed_Read(&state, now + 361, 1, 8754000));
  assert(WeightWindSpeed_OutputTick(&state, &stream, now + 365, 1, tx, sizeof(tx)));
  assert(strcmp(tx, "0.000\n") == 0);
  uint32_t zero_started = StartZero(&state, now + 400);
  assert(!WeightWindSpeed_OutputTick(&state, &stream, zero_started, 1, tx, sizeof(tx)));
  assert(!state.filter_valid && !state.zero_valid);
}

static void CheckEmaGapsAndRezero(void)
{
  WeightWindSpeed state;
  uint32_t now = Calibrate(&state, 0, 8754000);
  assert(ReadAndUpdate(&state, now + 249, 1, 8779000));
  assert(fabs(state.filtered_adc - 8754000 - 17801.477296730743313) < 1e-7);
  now = Calibrate(&state, 0, 8754000);
  assert(ReadAndUpdate(&state, now + 250, 1, 8779000));
  assert(state.filtered_adc == 8779000); /* Stale history is discarded. */
  now += 350;
  assert(ReadAndUpdate(&state, now, 1, 8854000));
  assert(state.filtered_adc > state.zero);
  uint32_t trigger = StartZero(&state, now + 10);
  for (uint32_t i = 1; i <= WEIGHT_SPEED_ZERO_SAMPLES; ++i)
    assert(ReadAndUpdate(&state, trigger + i * 100, 1, 8764000)
           == (i == WEIGHT_SPEED_ZERO_SAMPLES));
  assert(state.zero == 8764000 && state.filtered_adc == 8764000 && state.speed_mps == 0);
  /* An unchanged large input remains exact through an irregular EMA interval. */
  now = Calibrate(&state, 0, INT32_MAX - 1000000);
  assert(ReadAndUpdate(&state, now + 250, 1, INT32_MAX));
  assert(ReadAndUpdate(&state, now + 421, 1, INT32_MAX));
  assert(state.filtered_adc == INT32_MAX);
  assert(fabs(state.speed_mps - 19.686154966776988080357181) < 1e-10);
}

static void CheckOldLimitsAndFractionalZero(void)
{
  double value;
  assert(WeightWindSpeed_Convert(535294.0, &value));
  assert(fabs(value - 12.000006830534271314266786) < 1e-10);
  assert(WeightWindSpeed_Convert(1000000.0, &value));
  assert(fabs(value - 19.686154966776988080357181) < 1e-10);
  assert(WeightWindSpeed_Convert(-5001.0, &value) && value == 0.0);
  /* A real 40-read fractional tare must not reintroduce the old cutoff. */
  WeightWindSpeed state;
  WeightWindSpeed_Init(&state, 0);
  for (uint32_t i = 1; i <= 40; ++i)
    assert(ReadAndUpdate(&state, i * 100, 1, 8754000 + (i <= 17)) == (i == 40));
  assert(fabs(state.zero - 8754000.425) < 1e-8);
  assert(ReadAndUpdate(&state, 4250, 1, 8754000 + 535295));
  assert(fabs(state.speed_mps - 12.000016340924019541236398) < 1e-10);
  assert(state.sample_valid && state.filter_valid && state.zero_valid);
  assert(ReadAndUpdate(&state, 4350, 1, 9754000));
  assert(state.filtered_adc > 8754000 + 535295 && state.filtered_adc < 9754000);
  assert(state.speed_mps > 12.0);
}

static void CheckContinuousOutOfRangeStream(void)
{
  /* Independent Decimal references for high -> low -> original-range inputs.
   * Each actual read drives twenty 5 ms EMA updates; endpoint references
   * are the independent closed-form 100 ms responses.
   */
  const struct { int32_t raw_delta; double filtered_delta; double speed; const char *line; } steps[] = {
    {1000000, 393469.340287366576396200465009, 9.654254152661562747471946, "9.654\n"},
    {1000000, 632120.558828557678404476229839, 13.601499547819854828720186, "13.601\n"},
    {1000000, 776869.839851570171066719529236, 15.995624901392868133830148, "15.996\n"},
    {-1000000, 77726.036188654155313599575010, 3.883141281103085191579128, "3.883\n"},
    {-1000000, -346326.116281014151978481134144, 0.000000000000000000000000, "0.000\n"},
    {-1000000, -603526.748071004285112781474122, 0.000000000000000000000000, "0.000\n"},
    {100000, -326710.542632989882915966790909, 0.000000000000000000000000, "0.000\n"},
    {100000, -158813.026929523143709736819517, 0.000000000000000000000000, "0.000\n"},
    {100000, -56978.035965787232364066820827, 0.000000000000000000000000, "0.000\n"},
    {100000, 4788.008285277573299537091834, 0.539701910666998619519386, "0.540\n"},
  };
  WeightWindSpeed state;
  uint32_t now = Calibrate(&state, 0, 8754000);
  WeightSpeedStream stream;
  WeightSpeedStream_Init(&stream, now);
  char tx[WEIGHT_SPEED_LINE_CAPACITY];
  unsigned sent = 0;
  for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
  {
    double before = state.filtered_adc;
    assert(WeightWindSpeed_Read(&state, now + 1, 1, 8754000 + steps[i].raw_delta));
    assert(state.filtered_adc == before); /* Reading alone never advances EMA. */
    for (uint32_t dt = 5; dt <= 100; dt += 5)
    {
      assert(WeightWindSpeed_OutputTick(&state, &stream, now + dt, 1, tx, sizeof(tx)));
      assert(state.filter_ms == now + dt && state.read_ms == now + 1);
      assert(state.zero_valid && state.sample_valid && state.filter_valid);
      ++sent;
    }
    now += 100;
    assert(fabs(state.filtered_adc - state.zero - steps[i].filtered_delta) < 1e-7);
    assert(fabs(state.speed_mps - steps[i].speed) < 1e-10);
    assert(strcmp(tx, steps[i].line) == 0);
    if (i == 1 || i == 2) assert(state.speed_mps > 12.0);
    if (i == 3) assert(state.speed_mps > 0);
    if (i == 6) assert(state.filtered_adc < state.zero);
  }
  assert(sent == 200);
}

static void CheckInt32ExtremeOutput(void)
{
  const struct { int32_t baseline; int32_t raw; double speed; const char *line; } cases[] = {
    {INT32_MIN, INT32_MAX, 71041.081879470936554645709054, "71041.082\n"},
    {INT32_MAX, INT32_MIN, 0.0, "0.000\n"},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    WeightWindSpeed state;
    uint32_t now = Calibrate(&state, 0, cases[i].baseline) + 250;
    assert(ReadAndUpdate(&state, now, 1, cases[i].raw));
    assert(state.filtered_adc == cases[i].raw && state.sample_valid && state.filter_valid);
    assert(WeightWindSpeed_Status(&state, now) == WEIGHT_SPEED_READY);
    assert(fabs(state.speed_mps - cases[i].speed) < 1e-8);
    char prepared[WEIGHT_SPEED_LINE_CAPACITY], tx[WEIGHT_SPEED_LINE_CAPACITY];
    size_t length = WeightWindSpeed_Format(state.speed_mps, prepared, sizeof(prepared));
    assert(length == strlen(cases[i].line) && strcmp(prepared, cases[i].line) == 0);
    assert(length * 10U * 200U < 115200U);
    WeightSpeedStream stream;
    WeightSpeedStream_Init(&stream, now);
    WeightSpeedStream_Publish(&stream, now, prepared, length);
    for (uint32_t dt = 5; dt <= 100; dt += 5)
    {
      assert(WeightSpeedStream_Tick(&stream, now + dt, 1, tx, sizeof(tx)) == length);
      assert(strcmp(tx, cases[i].line) == 0);
    }
  }
}

static void CheckStream(void)
{
  WeightSpeedStream stream;
  WeightSpeedStream_Init(&stream, 0);
  char tx[WEIGHT_SPEED_LINE_CAPACITY];
  assert(!WeightSpeedStream_Tick(&stream, 5, 1, tx, sizeof(tx)));
  unsigned lines = 0, reads = 0;
  uint32_t previous = 5;
  for (uint32_t now = 6; now <= 10005; ++now)
  {
    if ((now - 6) % 100 == 0)
    {
      PublishSpeed(&stream, now, (reads % 10) / 1.0);
      ++reads;
    }
    size_t length = WeightSpeedStream_Tick(&stream, now, 1, tx, sizeof(tx));
    if (length)
    {
      assert(now - previous == 5);
      previous = now;
      assert(length * 10U * 200U < 115200U);
      ++lines;
    }
  }
  assert(reads == 100 && lines == 2000);
  WeightSpeedStream_Invalidate(&stream);
  assert(!WeightSpeedStream_Tick(&stream, 10010, 1, tx, sizeof(tx)));
  PublishSpeed(&stream, 10100, 3.125);
  assert(WeightSpeedStream_Tick(&stream, 10100, 1, tx, sizeof(tx)) == 6);
  assert(strcmp(tx, "3.125\n") == 0);
  assert(!WeightSpeedStream_Tick(&stream, 10100, 1, tx, sizeof(tx)));
  assert(WeightSpeedStream_Tick(&stream, 10345, 1, tx, sizeof(tx)) == 6);
  assert(!WeightSpeedStream_Tick(&stream, 10350, 1, tx, sizeof(tx)));
}

static void CheckWrapAndBusy(void)
{
  WeightSpeedStream stream;
  uint32_t start = UINT32_MAX - 3U;
  WeightSpeedStream_Init(&stream, start);
  PublishSpeed(&stream, start, 5.125);
  char tx[WEIGHT_SPEED_LINE_CAPACITY] = "in flight\n";
  assert(!WeightSpeedStream_Tick(&stream, start + 4U, 1, tx, sizeof(tx)));
  assert(!WeightSpeedStream_Tick(&stream, start + 5U, 0, tx, sizeof(tx)));
  assert(strcmp(tx, "in flight\n") == 0);
  PublishSpeed(&stream, start + 6U, 3.125);
  assert(strcmp(tx, "in flight\n") == 0); /* Publishing never writes UART's in-flight buffer. */
  assert(!WeightSpeedStream_Tick(&stream, start + 6U, 1, tx, sizeof(tx)));
  assert(WeightSpeedStream_Tick(&stream, start + 10U, 1, tx, sizeof(tx)) == 6);
  assert(strcmp(tx, "3.125\n") == 0);
  assert(!WeightSpeedStream_Tick(&stream, start + 15U, 1, tx, 5));
  assert(!WeightSpeedStream_Tick(&stream, start + 20U, 1, NULL, 0));
  assert(!WeightSpeedStream_Tick(&stream, start + 256U, 1, tx, sizeof(tx)));
  WeightSpeedStream_Publish(&stream, start + 300U, NULL, 0);
  assert(!WeightSpeedStream_Tick(&stream, start + 300U, 1, tx, sizeof(tx)));
  PublishSpeed(&stream, start + 400U, 5.125);
  WeightSpeedStream_Publish(&stream, start + 401U, "5.125", 5);
  assert(!WeightSpeedStream_Tick(&stream, start + 405U, 1, tx, sizeof(tx)));
}

static void EmitWire(void)
{
  WeightSpeedStream stream;
  WeightSpeedStream_Init(&stream, 0);
  char tx[WEIGHT_SPEED_LINE_CAPACITY];
  for (uint32_t now = 1; now <= 1000; ++now)
  {
    if ((now - 1) % 100U == 0)
    {
      unsigned index = (now - 1) / 100U;
      double speed;
      assert(index < sizeof(known_points) / sizeof(known_points[0]));
      assert(WeightWindSpeed_Convert(known_points[index].delta, &speed));
      PublishSpeed(&stream, now, speed);
    }
    size_t length = WeightSpeedStream_Tick(&stream, now, 1, tx, sizeof(tx));
    if (length)
      assert(fwrite(tx, 1, length, stdout) == length);
  }
}

static void EmitEmaWire(void)
{
  WeightWindSpeed state;
  uint32_t ready = Calibrate(&state, 0, 8754000);
  WeightSpeedStream stream;
  WeightSpeedStream_Init(&stream, ready);
  char tx[WEIGHT_SPEED_LINE_CAPACITY];
  for (uint32_t dt = 1; dt <= 1000; ++dt)
  {
    if ((dt - 1) % 100 == 0)
      assert(WeightWindSpeed_Read(&state, ready + dt, 1, 9754000));
    size_t length = WeightWindSpeed_OutputTick(&state, &stream, ready + dt, 1, tx, sizeof(tx));
    if (length) assert(fwrite(tx, 1, length, stdout) == length);
  }
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  if (strcmp(argv[1], "ema_wire") == 0) { EmitEmaWire(); return 0; }
  if (strcmp(argv[1], "wire") == 0) { EmitWire(); return 0; }
  if (strcmp(argv[1], "conversion") == 0) CheckConversion();
  else if (strcmp(argv[1], "lut_shape") == 0) CheckLutKnotsAndSegments();
  else if (strcmp(argv[1], "button") == 0) CheckButton();
  else if (strcmp(argv[1], "automatic_reset") == 0) CheckAutomaticStartupAndReset();
  else if (strcmp(argv[1], "automatic_failures") == 0) CheckAutomaticStartupFailures();
  else if (strcmp(argv[1], "zero") == 0) CheckZero();
  else if (strcmp(argv[1], "recovery") == 0) CheckRecoveryAndFormatting();
  else if (strcmp(argv[1], "status") == 0) CheckStatusAndZeroInterruption();
  else if (strcmp(argv[1], "read_schedule") == 0) CheckReadCompletionSchedule();
  else if (strcmp(argv[1], "online_ema") == 0) CheckOnlineEma();
  else if (strcmp(argv[1], "output_freshness") == 0) CheckOutputFreshness();
  else if (strcmp(argv[1], "ema_gaps") == 0) CheckEmaGapsAndRezero();
  else if (strcmp(argv[1], "old_limits") == 0) CheckOldLimitsAndFractionalZero();
  else if (strcmp(argv[1], "continuous_output") == 0) CheckContinuousOutOfRangeStream();
  else if (strcmp(argv[1], "int32_output") == 0) CheckInt32ExtremeOutput();
  else if (strcmp(argv[1], "stream") == 0) CheckStream();
  else if (strcmp(argv[1], "wrap_busy") == 0) CheckWrapAndBusy();
  else assert(0 && "unknown group");
  puts("Wind speed checks passed");
  return 0;
}
