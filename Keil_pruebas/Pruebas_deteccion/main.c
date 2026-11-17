#include "stm32f3xx.h"
#include "Verdes.h"

#define RELOJ_TICKS_HZ         1000U
#define RELOJ_PERIODO_MAX_MS  65536U
#define SONIDO_PRUEBA_HZ      0U

/* 0 = prueba a 3 kHz en ambos sentidos. */
volatile LadoComunicacion lado_comunicacion = LADO_PRUEBA;
volatile uint32_t sonido_tx_hz = SONIDO_PRUEBA_HZ;
volatile uint32_t sonido_rx_hz = SONIDO_PRUEBA_HZ;
volatile uint32_t sonido_recibido = 0U;
volatile uint32_t luz1_recibida = 0U;
volatile uint32_t luz2_recibida = 0U;

volatile uint16_t luz_adc = 0U;
volatile uint16_t luz_umbral_adc = 2048U;
volatile uint32_t luz_detectada = 0U;
volatile uint16_t luz2_adc = 0U;
volatile uint16_t luz2_umbral_adc = 2048U;
volatile uint32_t luz2_detectada = 0U;

volatile uint32_t reloj_periodo_tx_ms = 1000U;
volatile uint32_t reloj_ancho_tx_ms = 500U;
volatile uint32_t reloj_nivel_rx = 0U;
volatile uint32_t reloj_periodo_rx_ms = 0U;
volatile uint32_t reloj_ancho_rx_ms = 0U;
volatile uint32_t reloj_flancos_subida = 0U;
volatile uint32_t reloj_flancos_bajada = 0U;

static bool sonido_encendido = false;
static uint32_t sonido_tx_aplicado_hz = 0U;
static LadoComunicacion lado_aplicado = LADO_PRUEBA;
static uint32_t reloj_periodo_aplicado_ms = 0U;
static uint32_t reloj_ancho_aplicado_ms = 0U;
static bool leyendo_luz2 = false;
static volatile uint32_t reloj_ms = 0U;
static uint32_t reloj_ultima_subida_ms = 0U;
static bool reloj_tiene_subida = false;

static void Reloj_8MHz_Init(void)
{
    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0U) { }
    RCC->CFGR &= ~RCC_CFGR_SW;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    RCC->CR &= ~RCC_CR_PLLON;
    SystemCoreClockUpdate();
}

void Sonido_Emitir(bool encender)
{
    sonido_encendido = encender;
    sonido_tx_aplicado_hz = encender ? sonido_tx_hz : 0U;
    PWM_Buzzer_SetFrequency(sonido_tx_aplicado_hz);
}

bool Configurar_Lado(LadoComunicacion lado)
{
    switch (lado) {
    case LADO_PRUEBA:
        sonido_tx_hz = SONIDO_PRUEBA_HZ;
        sonido_rx_hz = SONIDO_PRUEBA_HZ;
        break;
    case LADO_1:
        sonido_tx_hz = 3000U;
        sonido_rx_hz = 4000U;
        break;
    case LADO_2:
        sonido_tx_hz = 4000U;
        sonido_rx_hz = 3000U;
        break;
    default:
        return false;
    }

    lado_comunicacion = lado;
    lado_aplicado = lado;
    if (sonido_encendido) {
        Sonido_Emitir(true);
    }
    return true;
}

void Laser1_Encender(void)
{
    GPIOB->BSRR = GPIO_BSRR_BS_0;
}

void Laser1_Apagar(void)
{
    GPIOB->BSRR = GPIO_BSRR_BR_0;
}

void Laser2_Encender(void)
{
    GPIOB->BSRR = GPIO_BSRR_BS_5;
}

void Laser2_Apagar(void)
{
    GPIOB->BSRR = GPIO_BSRR_BR_5;
}

void Laser_Encender(void)
{
    Laser1_Encender();
}

void Laser_Apagar(void)
{
    Laser1_Apagar();
}

static void EsperarCiclosMinimos(uint32_t ciclos)
{
    while (ciclos != 0U) {
        __NOP();
        --ciclos;
    }
}

static void Luz_Init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN | RCC_AHBENR_ADC12EN;
    (void)RCC->AHBENR;

    /* D3 (PB0) y D11 (PB5): salidas push-pull, activas en alto. */
    GPIOB->BSRR = GPIO_BSRR_BR_0 | GPIO_BSRR_BR_5;
    GPIOB->OTYPER &= ~(GPIO_OTYPER_OT_0 | GPIO_OTYPER_OT_5);
    GPIOB->PUPDR &= ~(GPIO_PUPDR_PUPDR0 | GPIO_PUPDR_PUPDR5);
    GPIOB->MODER = (GPIOB->MODER & ~(GPIO_MODER_MODER0 | GPIO_MODER_MODER5))
                 | GPIO_MODER_MODER0_0 | GPIO_MODER_MODER5_0;

    /* A1 (PA1) = ADC1_IN2. A2 (PA3) = ADC1_IN4. Pull-downs externos. */
    GPIOA->PUPDR &= ~(GPIO_PUPDR_PUPDR1 | GPIO_PUPDR_PUPDR3);
    GPIOA->MODER |= GPIO_MODER_MODER1 | GPIO_MODER_MODER3;

    RCC->AHBRSTR |= RCC_AHBRSTR_ADC12RST;
    RCC->AHBRSTR &= ~RCC_AHBRSTR_ADC12RST;
    ADC1_2_COMMON->CCR = ADC_CCR_CKMODE_1;  /* HCLK / 2 = 4 MHz. */

    ADC1->CR &= ~ADC_CR_ADVREGEN;
    ADC1->CR |= ADC_CR_ADVREGEN_0;
    EsperarCiclosMinimos(160U);  /* Al menos 20 us a 8 MHz. */

    ADC1->CR |= ADC_CR_ADCAL;  /* Calibracion single-ended. */
    while ((ADC1->CR & ADC_CR_ADCAL) != 0U) { }
    EsperarCiclosMinimos(16U);  /* Al menos 8 ciclos de ADC. */

    /* Conversiones individuales de 12 bits, 61.5 ciclos de muestreo. */
    ADC1->SMPR1 = ADC_SMPR1_SMP2_2 | ADC_SMPR1_SMP2_0
                | ADC_SMPR1_SMP4_2 | ADC_SMPR1_SMP4_0;
    ADC1->SQR1 = 2U << ADC_SQR1_SQ1_Pos;
    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while ((ADC1->ISR & ADC_ISR_ADRDY) == 0U) { }
    ADC1->CR |= ADC_CR_ADSTART;
}

static void Luz_Actualizar(void)
{
    if ((ADC1->ISR & ADC_ISR_EOS) == 0U || (ADC1->CR & ADC_CR_ADSTART) != 0U) {
        return;
    }

    const uint16_t muestra = (uint16_t)ADC1->DR;
    if (leyendo_luz2) {
        luz2_adc = muestra;
        luz2_detectada = (muestra >= luz2_umbral_adc);
        ADC1->SQR1 = 2U << ADC_SQR1_SQ1_Pos;
    } else {
        luz_adc = muestra;
        luz_detectada = (muestra >= luz_umbral_adc);
        ADC1->SQR1 = 4U << ADC_SQR1_SQ1_Pos;
    }

    leyendo_luz2 = !leyendo_luz2;
    ADC1->ISR = ADC_ISR_EOS;
    ADC1->CR |= ADC_CR_ADSTART;
}

void Canales_Leer(void)
{
    sonido_recibido = (sonido_rx_hz == 3000U) ? detectado_3khz : detectado_4khz;
    luz1_recibida = luz_detectada;
    luz2_recibida = luz2_detectada;
}

bool Reloj_Configurar(uint32_t periodo_ms, uint32_t ancho_ms)
{
    if (periodo_ms > RELOJ_PERIODO_MAX_MS || ancho_ms == 0U || ancho_ms >= periodo_ms) {
        return false;
    }

    /* Mantener el pulso actual. ARR y CCR4 se cargan juntos en el siguiente
     * update de TIM3, una vez liberados los eventos de actualizacion.
     */
    TIM3->CR1 |= TIM_CR1_UDIS;
    TIM3->ARR = periodo_ms - 1U;
    TIM3->CCR4 = ancho_ms;
    TIM3->CR1 &= ~TIM_CR1_UDIS;

    reloj_periodo_tx_ms = periodo_ms;
    reloj_ancho_tx_ms = ancho_ms;
    reloj_periodo_aplicado_ms = periodo_ms;
    reloj_ancho_aplicado_ms = ancho_ms;
    return true;
}

static void Aplicar_Ajustes(void)
{
    const LadoComunicacion lado = lado_comunicacion;
    if (lado != lado_aplicado && !Configurar_Lado(lado)) {
        lado_comunicacion = lado_aplicado;
    }

    /* En Watch, sonido_tx_hz = 0 silencia el buzzer local. */
    if (sonido_encendido && sonido_tx_hz != sonido_tx_aplicado_hz) {
        Sonido_Emitir(true);
    }

    const uint32_t periodo_ms = reloj_periodo_tx_ms;
    const uint32_t ancho_ms = reloj_ancho_tx_ms;
    if (periodo_ms != reloj_periodo_aplicado_ms || ancho_ms != reloj_ancho_aplicado_ms) {
        if (!Reloj_Configurar(periodo_ms, ancho_ms)) {
            reloj_periodo_tx_ms = reloj_periodo_aplicado_ms;
            reloj_ancho_tx_ms = reloj_ancho_aplicado_ms;
        }
    }
}

static void Reloj_Canales_Init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    (void)RCC->APB1ENR;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;

    /* D6 (PB1) = TIM3_CH4, AF2. PWM activo en alto, con tick de 1 ms. */
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM3RST;
    TIM3->PSC = SystemCoreClock / RELOJ_TICKS_HZ - 1U;
    TIM3->CCMR2 = (6U << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM3->CR1 = TIM_CR1_ARPE;
    Reloj_Configurar(reloj_periodo_tx_ms, reloj_ancho_tx_ms);
    TIM3->EGR = TIM_EGR_UG;

    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_1;
    GPIOB->PUPDR &= ~GPIO_PUPDR_PUPDR1;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFRL1) | (2U << GPIO_AFRL_AFRL1_Pos);
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER1) | GPIO_MODER_MODER1_1;

    /* D2 (PA12) = CLOCK_RX, pull-down interno y EXTI en ambos flancos. */
    GPIOA->MODER &= ~GPIO_MODER_MODER12;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~GPIO_PUPDR_PUPDR12) | GPIO_PUPDR_PUPDR12_1;
    SYSCFG->EXTICR[3] &= ~SYSCFG_EXTICR4_EXTI12;
    EXTI->RTSR |= EXTI_RTSR_TR12;
    EXTI->FTSR |= EXTI_FTSR_TR12;
    reloj_nivel_rx = (GPIOA->IDR & GPIO_IDR_12) != 0U;
    EXTI->PR = EXTI_PR_PR12;
    NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    NVIC_SetPriority(EXTI15_10_IRQn, (1U << __NVIC_PRIO_BITS) - 1U);
    EXTI->IMR |= EXTI_IMR_MR12;
    NVIC_EnableIRQ(EXTI15_10_IRQn);

    TIM3->CCER = TIM_CCER_CC4E;
    TIM3->CR1 |= TIM_CR1_CEN;
}

void SysTick_Handler(void)
{
    ++reloj_ms;
}

void EXTI15_10_IRQHandler(void)
{
    if ((EXTI->PR & EXTI_PR_PR12) == 0U) {
        return;
    }
    EXTI->PR = EXTI_PR_PR12;

    const uint32_t ahora_ms = reloj_ms;
    reloj_nivel_rx = (GPIOA->IDR & GPIO_IDR_12) != 0U;
    if (reloj_nivel_rx != 0U) {
        if (reloj_tiene_subida) {
            reloj_periodo_rx_ms = ahora_ms - reloj_ultima_subida_ms;
        }
        reloj_ultima_subida_ms = ahora_ms;
        reloj_tiene_subida = true;
        ++reloj_flancos_subida;
    } else {
        if (reloj_tiene_subida) {
            reloj_ancho_rx_ms = ahora_ms - reloj_ultima_subida_ms;
        }
        ++reloj_flancos_bajada;
    }
}

int main(void)
{
    Reloj_8MHz_Init();
    PWM_Buzzer_Init();
    Luz_Init();
    Audio_Init();
    SysTick_Config(SystemCoreClock / RELOJ_TICKS_HZ);
    NVIC_SetPriority(SysTick_IRQn, (1U << __NVIC_PRIO_BITS) - 2U);
    Reloj_Canales_Init();

    Configurar_Lado(lado_comunicacion);
    Sonido_Emitir(true);
    Laser1_Encender();
    Laser2_Encender();

    while (1) {
        Aplicar_Ajustes();
        Audio_Actualizar();
        Luz_Actualizar();
        Canales_Leer();
    }
}
