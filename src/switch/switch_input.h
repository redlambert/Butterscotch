#ifndef _BS_SWITCH_INPUT_H_
#define _BS_SWITCH_INPUT_H_

#include <stdbool.h>

struct Runner;

bool SwitchInput_handleEvents(struct Runner* runner);
void SwitchInput_setVibration(int device, float left, float right);

#endif
