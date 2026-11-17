#include "stm32f3xx.h"
#include "buzzer.h"

void PWM_Buzzer_Init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR;
    RCC->APB2ENR |= RCC_APB2ENR_TIM16EN;
    (void)RCC->APB2ENR;

    TIM16->CCMR1 = (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM16->CCER = TIM_CCER_CC1E;
    TIM16->BDTR = TIM_BDTR_MOE;
    TIM16->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;

    /* PB4 (D12) = TIM16_CH1, AF1. */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFRL4) | (1U << GPIO_AFRL_AFRL4_Pos);
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER4) | GPIO_MODER_MODER4_1;
}

void PWM_Buzzer_SetFrequency(uint32_t freq)
{
    /* APB2 sin division: TIM16 recibe el reloj del nucleo. */
    const uint32_t timer_hz = SystemCoreClock;

    if ((freq == 0U) || (freq > timer_hz / 2U)) {
        TIM16->CCR1 = 0U;
    } else {
        uint32_t period_ticks = (timer_hz + freq / 2U) / freq;
        uint32_t divider = (period_ticks - 1U) / 65536U + 1U;

        /* Mantener el periodo dentro de los 16 bits de TIM16. */
        period_ticks = (period_ticks + divider / 2U) / divider;
        TIM16->PSC = divider - 1U;
        TIM16->ARR = period_ticks - 1U;
        TIM16->CCR1 = period_ticks / 2U;
    }

    TIM16->EGR = TIM_EGR_UG;
}

