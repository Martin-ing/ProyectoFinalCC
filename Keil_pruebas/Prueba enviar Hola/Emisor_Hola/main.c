#include "stm32f3xx.h"
#include "buzzer.h"
#include "phy_tx.h"
#include <stdbool.h>

#define HOLA_PORTADORA_HZ 4000U
#define RELOJ_TICKS_HZ 1000U

static const uint8_t mensaje[] = {'h', 'o', 'l', 'a'};
#define HOLA_CICLOS (sizeof mensaje * PHY_CICLOS_TRAMA)

volatile uint32_t hola_tx_completos = 0U;
volatile uint32_t hola_tx_pulso = 0U;
volatile uint16_t hola_tx_ancho_actual_ms = 0U;
volatile uint8_t hola_tx_simbolo = 0U;
volatile uint8_t hola_tx_tipo = PHY_TIPO_SYNC;

static PHY_BloqueControl sync;
static uint8_t simbolos_seq0[HOLA_CICLOS];
static uint32_t ciclo_sync = 0U;
static uint32_t ciclo_datos = 0U;
static bool datos_iniciados = false;
static bool tono_encendido = false;

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

static void Emitir_Simbolo(uint8_t simbolo, uint16_t ancho_ms)
{
    const bool tono = (simbolo & 4U) != 0U;
    if (tono != tono_encendido) {
        PWM_Buzzer_SetFrequency(tono ? HOLA_PORTADORA_HZ : 0U);
        tono_encendido = tono;
    }
    GPIOB->BSRR = ((simbolo & 2U) ? GPIO_BSRR_BS_0 : GPIO_BSRR_BR_0)
                | ((simbolo & 1U) ? GPIO_BSRR_BS_5 : GPIO_BSRR_BR_5);
    hola_tx_simbolo = simbolo;
    hola_tx_ancho_actual_ms = ancho_ms;
}

static void Canales_TX_Init(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR;

    /* D3 (PB0) y D11 (PB5) gobiernan los dos emisores de luz. */
    GPIOB->BSRR = GPIO_BSRR_BR_0 | GPIO_BSRR_BR_5;
    GPIOB->OTYPER &= ~(GPIO_OTYPER_OT_0 | GPIO_OTYPER_OT_5);
    GPIOB->PUPDR &= ~(GPIO_PUPDR_PUPDR0 | GPIO_PUPDR_PUPDR5);
    GPIOB->MODER = (GPIOB->MODER & ~(GPIO_MODER_MODER0 | GPIO_MODER_MODER5))
                 | GPIO_MODER_MODER0_0 | GPIO_MODER_MODER5_0;
    PWM_Buzzer_Init();  /* D12 (PB4). */
    PWM_Buzzer_SetFrequency(0U);

    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM3RST;
    TIM3->PSC = SystemCoreClock / RELOJ_TICKS_HZ - 1U;
    TIM3->ARR = PHY_PERIODO_MS - 1U;
    TIM3->CCR4 = 0U;
    TIM3->CCMR2 = (6U << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0U;

    /* Reloj TX por D6 (PB1), TIM3_CH4 en AF2. */
    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_1;
    GPIOB->PUPDR &= ~GPIO_PUPDR_PUPDR1;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFRL1) | (2U << GPIO_AFRL_AFRL1_Pos);
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER1) | GPIO_MODER_MODER1_1;

    TIM3->CCER = TIM_CCER_CC4E;
    TIM3->DIER = TIM_DIER_UIE;
    TIM3->CCR4 = sync.reloj_ms[0];
    TIM3->CNT = TIM3->ARR;
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM3_STOP;
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CR1 |= TIM_CR1_CEN;
}

void TIM3_IRQHandler(void)
{
    if ((TIM3->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM3->SR = 0U;

    if (ciclo_sync < PHY_CICLOS_BLOQUE) {
        Emitir_Simbolo(sync.datos[ciclo_sync], sync.reloj_ms[ciclo_sync]);
        ++ciclo_sync;
        TIM3->CCR4 = (ciclo_sync < PHY_CICLOS_BLOQUE)
                   ? sync.reloj_ms[ciclo_sync] : phy_tx_reloj_ms[0];
        return;
    }

    if (ciclo_datos == 0U) {
        if (datos_iniciados) {
            ++hola_tx_completos;
        }
        datos_iniciados = true;
        hola_tx_tipo = PHY_TIPO_NUEVO;
    }
    const volatile uint8_t *simbolos = (hola_tx_completos & 1U)
                                     ? phy_tx_datos : simbolos_seq0;
    Emitir_Simbolo(simbolos[ciclo_datos], phy_tx_reloj_ms[ciclo_datos]);
    hola_tx_pulso = ciclo_datos + 1U;
    ciclo_datos = (ciclo_datos + 1U == HOLA_CICLOS) ? 0U : ciclo_datos + 1U;
    TIM3->CCR4 = phy_tx_reloj_ms[ciclo_datos];
}

int main(void)
{
    Reloj_8MHz_Init();
    PHY_TX_Preparar_Control(&sync, PHY_TIPO_SYNC);
    /* Cada vuelta ocupa 12 IDs fisicos. SEQ alterna entre 0..3 y 4..7. */
    PHY_TX_Preparar(mensaje, sizeof mensaje, 0U, 0U, 0U);
    for (uint32_t i = 0U; i < HOLA_CICLOS; ++i) {
        simbolos_seq0[i] = phy_tx_datos[i];
    }
    PHY_TX_Preparar(mensaje, sizeof mensaje, 0U, 0U, sizeof mensaje);
    Canales_TX_Init();

    while (1) {
        __WFI();
    }
}
