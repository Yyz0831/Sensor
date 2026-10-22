#ifndef WEIGHT_PROFILE_H
#define WEIGHT_PROFILE_H

/* User-confirmed calibration with an actual 100 g reference mass.
 * Retained in the firmware across reset/power loss; zero must be tared anew.
 * Set the gain to 0.0 to disable the preset and require manual calibration.
 */
#define WEIGHT_PROFILE_COUNTS_PER_GRAM 1082.79688
#define WEIGHT_PROFILE_REFERENCE_G    100.0

#endif
