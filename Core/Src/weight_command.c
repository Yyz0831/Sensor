#include "weight_command.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *SkipSpace(const char *text)
{
  while (isspace((unsigned char)*text))
    ++text;
  return text;
}

WeightCommand WeightCommand_Parse(const char *text, double *mass_g)
{
  *mass_g = 0;
  text = SkipSpace(text);
  if (strncmp(text, "tare", 4) == 0 && *SkipSpace(text + 4) == '\0')
    return WEIGHT_COMMAND_TARE;
  if (strncmp(text, "help", 4) == 0 && *SkipSpace(text + 4) == '\0')
    return WEIGHT_COMMAND_HELP;
  if (strncmp(text, "cal", 3) != 0 || !isspace((unsigned char)text[3]))
    return WEIGHT_COMMAND_INVALID;
  const char *number = SkipSpace(text + 3);
  char *end;
  errno = 0;
  double mass = strtod(number, &end);
  if (end == number || errno == ERANGE || *SkipSpace(end) != '\0' ||
      !isfinite(mass) || mass <= 0.0)
    return WEIGHT_COMMAND_INVALID;
  *mass_g = mass;
  return WEIGHT_COMMAND_CAL;
}
