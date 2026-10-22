#ifndef WEIGHT_COMMAND_H
#define WEIGHT_COMMAND_H

typedef enum { WEIGHT_COMMAND_INVALID, WEIGHT_COMMAND_TARE,
               WEIGHT_COMMAND_CAL, WEIGHT_COMMAND_HELP } WeightCommand;
WeightCommand WeightCommand_Parse(const char *text, double *mass_g);

#endif
