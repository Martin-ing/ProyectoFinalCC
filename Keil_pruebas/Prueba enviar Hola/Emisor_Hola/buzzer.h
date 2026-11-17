#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

void PWM_Buzzer_Init(void);
void PWM_Buzzer_SetFrequency(uint32_t freq);

#endif
