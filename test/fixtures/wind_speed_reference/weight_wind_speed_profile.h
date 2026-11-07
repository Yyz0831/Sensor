#ifndef WEIGHT_WIND_SPEED_PROFILE_H
#define WEIGHT_WIND_SPEED_PROFILE_H

/* User-selected 15 x 15 cm delta-ADC calibration, 2026-11-05:
 * input = causal EMA(raw_adc, tau=200 ms) - current no-wind zero.
 * Twelve fitted measured-range knots plus the fixed physical origin (0, 0).
 * Use linear interpolation, and the final segment above the last fitted knot.
 * Finite inputs outside the fitted range keep producing output: nonpositive
 * increments yield zero; positive increments above the last knot extrapolate.
 * Continued output does not extend the validated 2-10 m/s range.
 * Fitted/scored only at reference speeds 2-10 m/s: five runs / 1271 pairs.
 * The origin is a constraint, not evidence of accuracy below 2 m/s.
 * Exact model, prior power law and checks: calibration/wind_tunnel_15cm_20261105_lut/.
 * Every reset measures a new zero; these knots are increments, not absolute ADC.
 */
#define WEIGHT_WIND_SPEED_EMA_TAU_MS 200.0
#define WEIGHT_WIND_SPEED_KNOT_COUNT 13U

typedef struct
{
  double delta_adc;
  double speed_mps;
} WeightWindSpeedKnot;

static const WeightWindSpeedKnot WEIGHT_WIND_SPEED_KNOTS[WEIGHT_WIND_SPEED_KNOT_COUNT] = {
  {0.0, 0.0},
  {19399.077995505184, 2.1866544156878627},
  {34396.10931593057, 2.31568181494814},
  {53658.56707251627, 2.993269573087748},
  {77186.45126526228, 3.8685775676298837},
  {104979.76189416861, 4.618735318381398},
  {137038.49895923524, 5.5199201079562386},
  {173362.66246046216, 6.255824450321441},
  {213952.2523978494, 6.846079829751774},
  {258807.26877139698, 7.620562044919289},
  {307927.7115811048, 8.158671770557204},
  {361313.58082697314, 9.122404056803166},
  {418964.8765090015, 10.075945433196843},
};

#endif
