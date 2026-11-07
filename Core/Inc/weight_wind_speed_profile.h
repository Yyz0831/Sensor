#ifndef WEIGHT_WIND_SPEED_PROFILE_H
#define WEIGHT_WIND_SPEED_PROFILE_H

/* Generated from raw ADC: run_20261105_112511
 * Input: online EMA(raw ADC, tau=200 ms) minus this reset's no-wind zero.
 * Twelve fitted measured-range knots plus fixed origin; 13 points total.
 * Reference 2-10 m/s: 259 pairs; full-fit MSE=0.00548534154444 (m/s)^2.
 * This is training residual, not held-out accuracy. See calibration report.
 * Firmware updates EMA at each 5 ms output slot; hardware tare remains 4 s.
 * Nonpositive delta -> zero; above the last knot -> final-segment extrapolation.
 * Raw ADC acquisition and the sensor module configuration are unchanged.
 */
#define WEIGHT_WIND_SPEED_EMA_TAU_MS 200.0
#define WEIGHT_WIND_SPEED_KNOT_COUNT 13U

typedef struct
{
  double delta_adc;
  double speed_mps;
} WeightWindSpeedKnot;

static const WeightWindSpeedKnot WEIGHT_WIND_SPEED_KNOTS[WEIGHT_WIND_SPEED_KNOT_COUNT] = {
  {0, 0},
  {17179.573629591614, 2.1865718777721206},
  {30853.752817270099, 2.3355991516349599},
  {48502.950683743555, 2.8679530232268977},
  {70127.16722901196, 3.8702851600335411},
  {95726.402453075352, 4.4193927004144822},
  {125300.65635593371, 5.2623932731894083},
  {158849.92893758704, 6.0478660359050718},
  {196374.22019803536, 6.5335788257960496},
  {237873.53013727863, 7.379150655063536},
  {283347.85875531688, 7.9543371194998782},
  {332797.20605215005, 8.597939067824738},
  {386221.57202777825, 9.6402191463426963},
};

#endif
