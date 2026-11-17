#include "stm32f3xx.h"
#include "Verdes.h"

#define AUDIO_MUESTREO_HZ  32000U
#define AUDIO_MUESTRAS     256U
#define AUDIO_ESCALA_COEF  16384
#define AUDIO_NIVEL        64  /* Muestras de un bit centradas en +/-64. */
#define AUDIO_BLOQUES_CONFIRMACION  2U
#define AUDIO_EVENTOS_DMA  (DMA_ISR_HTIF2 | DMA_ISR_TCIF2)

volatile uint32_t nivel_entrada = 0U;
volatile uint32_t sonido_frecuencia_medida_hz = 0U;
volatile uint32_t detectado_3khz = 0U;
volatile uint32_t detectado_4khz = 0U;

/* Energia normalizada por bloque, multiplicada por 1000. */
volatile uint32_t energia_3khz = 0U;
volatile uint32_t energia_4khz = 0U;
volatile uint32_t umbral_3khz = 40U;
volatile uint32_t umbral_4khz = 40U;

static volatile uint8_t audio_dma[2][AUDIO_MUESTRAS];
static uint8_t cambios_3khz = 0U;
static uint8_t cambios_4khz = 0U;

/* 2*cos(2*pi*f/32000) en Q14. Tres bins de 125 Hz por tono. */
static const int32_t coeficientes_3khz[3] = {27684, 27246, 26791};  /* 2875, 3000, 3125 Hz */
static const int32_t coeficientes_4khz[3] = {23732, 23170, 22595};  /* 3875, 4000, 4125 Hz */

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

static void Audio_IniciarDMA(void)
{
    /* TIM2_UP solicita a DMA1_CH2 una lectura de IDR.
     * Guardar solo el byte bajo de IDR, que contiene PA0.
     */
    DMA1_Channel2->CCR = 0U;
    DMA1->IFCR = DMA_IFCR_CGIF2;
    DMA1_Channel2->CPAR = (uint32_t)(uintptr_t)&GPIOA->IDR;
    DMA1_Channel2->CMAR = (uint32_t)(uintptr_t)&audio_dma[0][0];
    DMA1_Channel2->CNDTR = 2U * AUDIO_MUESTRAS;
    DMA1_Channel2->CCR = DMA_CCR_PSIZE_1 | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_EN;
}

void Audio_Init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_DMA1EN;
    (void)RCC->AHBENR;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;

    /* LM393 -> SW1 -> PA0 (A0). El pull-up fija HIGH con SW1 abierto. */
    GPIOA->MODER &= ~GPIO_MODER_MODER0;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~GPIO_PUPDR_PUPDR0) | GPIO_PUPDR_PUPDR0_0;

    RCC->APB1RSTR |= RCC_APB1RSTR_TIM2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;
    TIM2->PSC = 0U;
    TIM2->ARR = SystemCoreClock / AUDIO_MUESTREO_HZ - 1U;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0U;
    sonido_frecuencia_medida_hz = 0U;
    Audio_IniciarDMA();
    TIM2->DIER = TIM_DIER_UDE;
    TIM2->CR1 = TIM_CR1_CEN;
}

static uint32_t Audio_MedirFrecuencia(const int8_t *bloque)
{
    uint32_t primer_flanco = 0U;
    uint32_t ultimo_flanco = 0U;
    uint32_t flancos = 0U;

    for (uint32_t i = 1U; i < AUDIO_MUESTRAS; ++i) {
        if ((bloque[i - 1U] <= 0) && (bloque[i] > 0)) {
            if (flancos == 0U) {
                primer_flanco = i;
            }
            ultimo_flanco = i;
            ++flancos;
        }
    }

    if (flancos < 2U) {
        return 0U;
    }

    /* Usa los periodos completos entre el primer flanco y el ultimo. */
    const uint32_t muestras = ultimo_flanco - primer_flanco;
    return ((flancos - 1U) * AUDIO_MUESTREO_HZ + muestras / 2U) / muestras;
}

static uint32_t Audio_Goertzel(const int8_t *bloque, int32_t coef)
{
    int32_t previo = 0;
    int32_t anterior = 0;

    const int8_t *fin = bloque + AUDIO_MUESTRAS;
    while (bloque != fin) {
        const int32_t actual = *bloque++ + coef * previo / AUDIO_ESCALA_COEF - anterior;
        anterior = previo;
        previo = actual;
    }

    const int64_t potencia = (int64_t)previo * previo + (int64_t)anterior * anterior
                           - (int64_t)coef * previo * anterior / AUDIO_ESCALA_COEF;
    return (uint32_t)potencia;
}

static uint32_t Audio_EnergiaBanda(const int8_t *bloque, const int32_t *coeficientes)
{
    uint32_t potencia = 0U;
    for (uint32_t i = 0U; i < 3U; ++i) {
        potencia += Audio_Goertzel(bloque, coeficientes[i]);
    }

    const uint32_t escala = AUDIO_MUESTRAS * AUDIO_NIVEL;
    return (uint32_t)((uint64_t)potencia * 1000U / (escala * escala));
}

static void Audio_Confirmar(uint32_t energia, uint32_t umbral, uint8_t *cambios,
                            volatile uint32_t *detectado)
{
    const uint32_t presente = (energia >= umbral);
    if (presente == *detectado) {
        *cambios = 0U;
    } else if (++*cambios >= AUDIO_BLOQUES_CONFIRMACION) {
        *detectado = presente;
        *cambios = 0U;
    }
}

void Audio_Actualizar(void)
{
    nivel_entrada = GPIOA->IDR & GPIO_IDR_0;
    const uint32_t estado = DMA1->ISR;
    if ((estado & DMA_ISR_TEIF2) != 0U) {
        sonido_frecuencia_medida_hz = 0U;
        detectado_3khz = 0U;
        detectado_4khz = 0U;
        energia_3khz = 0U;
        energia_4khz = 0U;
        cambios_3khz = 0U;
        cambios_4khz = 0U;
        Audio_IniciarDMA();
        return;
    }

    const uint32_t eventos = estado & AUDIO_EVENTOS_DMA;
    if (eventos == 0U) {
        return;
    }
    /* Los bits de fin de transferencia tienen la misma posicion en ISR e IFCR. */
    DMA1->IFCR = eventos;
    const uint32_t restantes = DMA1_Channel2->CNDTR;
    const uint32_t mitad = ((restantes > AUDIO_MUESTRAS) || (restantes == 0U)) ? 1U : 0U;

    int8_t bloque[AUDIO_MUESTRAS];
    for (uint32_t i = 0U; i < AUDIO_MUESTRAS; ++i) {
        bloque[i] = (audio_dma[mitad][i] & GPIO_IDR_0) ? AUDIO_NIVEL : -AUDIO_NIVEL;
    }
    if ((DMA1->ISR & (AUDIO_EVENTOS_DMA | DMA_ISR_TEIF2)) != 0U) {
        return;  /* El DMA cambio de mitad durante la copia. */
    }

    sonido_frecuencia_medida_hz = Audio_MedirFrecuencia(bloque);
    energia_3khz = Audio_EnergiaBanda(bloque, coeficientes_3khz);
    energia_4khz = Audio_EnergiaBanda(bloque, coeficientes_4khz);
    Audio_Confirmar(energia_3khz, umbral_3khz, &cambios_3khz, &detectado_3khz);
    Audio_Confirmar(energia_4khz, umbral_4khz, &cambios_4khz, &detectado_4khz);
}
