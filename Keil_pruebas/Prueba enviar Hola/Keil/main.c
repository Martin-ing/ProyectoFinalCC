#include "stm32f3xx.h"
#include "Verdes.h"
#include "phy_tx.h"
#include "phy_rx.h"
#include "chat_com.h"

#define RELOJ_TICKS_HZ 1000U
#define CHAT_BAUD       9600U
#define CHAT_RX_COLA      32U
#define RX_PULSOS_COLA      4U
#define RX_SIN_PULSOS_MS 1600U

typedef struct {
    uint32_t periodo_ms;
    uint32_t ancho_ms;
    uint32_t instante_ms;
    uint8_t simbolo;
} PulsoRX;

enum {
    CHAT_EVENTO_LISTO  = 1U << 0U,
    CHAT_EVENTO_INICIO = 1U << 1U,
    CHAT_EVENTO_FIN    = 1U << 2U
};

volatile LadoComunicacion lado_comunicacion = LADO_1;
volatile uint32_t sonido_tx_hz = 3000U;
volatile uint32_t sonido_rx_hz = 4000U;
volatile uint32_t sonido_recibido = 0U;
volatile uint32_t luz1_recibida = 0U;
volatile uint32_t luz2_recibida = 0U;

volatile uint16_t luz_adc = 0U;
volatile uint16_t luz_umbral_adc = 2048U;
volatile uint32_t luz_detectada = 0U;
volatile uint16_t luz2_adc = 0U;
volatile uint16_t luz2_umbral_adc = 2048U;
volatile uint32_t luz2_detectada = 0U;

volatile uint32_t reloj_nivel_rx = 0U;
volatile uint32_t reloj_periodo_rx_ms = 0U;
volatile uint32_t reloj_ancho_rx_ms = 0U;
volatile uint32_t reloj_flancos_subida = 0U;
volatile uint32_t reloj_flancos_bajada = 0U;

volatile uint8_t chat_rx_buffer[PHY_TX_MAX_BYTES];
volatile uint8_t chat_rx_longitud = 0U;
volatile uint8_t chat_rx_control = 0U;
volatile uint32_t chat_rx_total = 0U;
volatile bool phy_tx_en_curso = false;
volatile uint32_t phy_tx_pulsos_iniciados = 0U;
volatile uint32_t phy_tx_total_pulsos = 0U;
volatile uint32_t phy_tx_sync_iniciados = 0U;
volatile uint8_t phy_tx_tipo_actual = PHY_TIPO_APAGADO;
volatile uint16_t phy_tx_ancho_actual_ms = 0U;
volatile uint8_t phy_tx_simbolo_actual = 0U;

static PHY_BloqueControl phy_tx_sync;
static PHY_BloqueControl phy_tx_idle;
static volatile uint32_t phy_tx_ciclo = 0U;
static volatile bool phy_tx_detener = false;
static volatile bool phy_tx_plan_listo = false;
static volatile bool phy_tx_sync_pendiente = false;
static volatile bool chat_sesion_abierta = false;
static bool chat_sesion_lista = false;
static bool chat_escuchando = false;
static bool chat_informar_rx = false;
static uint32_t chat_mensajes_informados = 0U;
static volatile uint8_t chat_eventos = 0U;

static ChatComParser chat_parser;
static volatile uint8_t chat_uart_cola[CHAT_RX_COLA];
static volatile uint8_t chat_uart_entrada = 0U;
static volatile uint8_t chat_uart_salida = 0U;
static volatile bool chat_uart_error = false;
static uint8_t phy_tx_proximo_id = 0U;
static uint8_t chat_proximo_seq = 0U;
static volatile bool sonido_encendido = false;
static LadoComunicacion lado_aplicado = LADO_PRUEBA;
static bool leyendo_luz2 = false;
static volatile uint32_t reloj_ms = 0U;
static uint32_t reloj_ultima_subida_ms = 0U;
static bool reloj_tiene_subida = false;
static bool reloj_pulso_abierto = false;

static volatile PulsoRX rx_cola[RX_PULSOS_COLA];
static volatile uint8_t rx_entrada = 0U;
static volatile uint8_t rx_salida = 0U;
static volatile bool rx_pulso_perdido = false;
static uint32_t rx_ultimo_flanco_ms = 0U;
static uint32_t rx_periodo_medido = 0U;
static uint32_t rx_ancho_medido = 0U;

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
    PWM_Buzzer_SetFrequency(encender ? sonido_tx_hz : 0U);
}

bool Configurar_Lado(LadoComunicacion lado)
{
    switch (lado) {
    case LADO_PRUEBA:
        sonido_tx_hz = 4000U;
        sonido_rx_hz = 4000U;
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
        PWM_Buzzer_SetFrequency(sonido_tx_hz);
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

static void Aplicar_Lado(void)
{
    const LadoComunicacion lado = lado_comunicacion;
    if (!phy_tx_en_curso && lado != lado_aplicado) {
        /* TIM3 tambien puede encender el buzzer durante los pilotos de IDLE. */
        __disable_irq();
        if (!Configurar_Lado(lado)) {
            lado_comunicacion = lado_aplicado;
        }
        __enable_irq();
    }
}

static void PHY_Emitir_Simbolo(uint8_t simbolo)
{
    /* L1 = buzzer, L2 = laser 1, L3 = laser 2. */
    const bool tono = (simbolo & 4U) != 0U;
    if (tono != sonido_encendido) {
        Sonido_Emitir(tono);
    }
    GPIOB->BSRR = ((simbolo & 2U) ? GPIO_BSRR_BS_0 : GPIO_BSRR_BR_0)
                | ((simbolo & 1U) ? GPIO_BSRR_BS_5 : GPIO_BSRR_BR_5);
    phy_tx_simbolo_actual = simbolo;
}

static void Reloj_Canales_Init(void)
{
    PHY_TX_Preparar_Control(&phy_tx_sync, PHY_TIPO_SYNC);
    PHY_TX_Preparar_Control(&phy_tx_idle, PHY_TIPO_IDLE);

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
    TIM3->ARR = PHY_PERIODO_MS - 1U;
    TIM3->CCR4 = 0U;
    TIM3->CCMR2 = (6U << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0U;

    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_1;
    GPIOB->PUPDR &= ~GPIO_PUPDR_PUPDR1;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFRL1) | (2U << GPIO_AFRL_AFRL1_Pos);
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER1) | GPIO_MODER_MODER1_1;

    /* SB18 puede unir A4 (PA5) con D4. Dejar PA5 como entrada flotante. */
    GPIOA->MODER &= ~GPIO_MODER_MODER5;
    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPDR5;

    /* D4 (PB7) = CLOCK_RX, pull-down interno y EXTI en ambos flancos. */
    GPIOB->MODER &= ~GPIO_MODER_MODER7;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~GPIO_PUPDR_PUPDR7) | GPIO_PUPDR_PUPDR7_1;
    SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~SYSCFG_EXTICR2_EXTI7) | SYSCFG_EXTICR2_EXTI7_PB;
    EXTI->RTSR |= EXTI_RTSR_TR7;
    EXTI->FTSR |= EXTI_FTSR_TR7;
    EXTI->PR = EXTI_PR_PR7;
    NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    NVIC_SetPriority(EXTI9_5_IRQn, (1U << __NVIC_PRIO_BITS) - 1U);
    reloj_nivel_rx = (GPIOB->IDR & GPIO_IDR_7) != 0U;
    EXTI->IMR |= EXTI_IMR_MR7;
    NVIC_EnableIRQ(EXTI9_5_IRQn);

    TIM3->CCER = TIM_CCER_CC4E;
    TIM3->DIER = TIM_DIER_UIE;
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM3_STOP;
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_SetPriority(TIM3_IRQn, 0U);
    NVIC_EnableIRQ(TIM3_IRQn);
}

static void Reloj_TX_Iniciar(void)
{
    phy_tx_tipo_actual = PHY_TIPO_APAGADO;
    phy_tx_ciclo = 0U;
    phy_tx_detener = false;
    /* Con CCR4 activo en cero, la primera subida ocurre en 1 tick. */
    TIM3->CCR4 = PHY_MARCADOR_MS;
    TIM3->CNT = TIM3->ARR;
    TIM3->SR = 0U;
    TIM3->CR1 |= TIM_CR1_CEN;
}

static void PHY_Cerrar_Bloque(void)
{
    if (phy_tx_tipo_actual == PHY_TIPO_NUEVO
        && phy_tx_pulsos_iniciados == phy_tx_total_pulsos) {
        phy_tx_plan_listo = false;
        chat_eventos |= CHAT_EVENTO_FIN;
    } else if (phy_tx_tipo_actual == PHY_TIPO_SYNC
               && chat_sesion_abierta && !phy_tx_sync_pendiente) {
        chat_eventos |= CHAT_EVENTO_LISTO;
    }
}

static void PHY_Abrir_Bloque(void)
{
    if (phy_tx_plan_listo) {
        phy_tx_tipo_actual = PHY_TIPO_NUEVO;
        if (phy_tx_pulsos_iniciados == 0U) {
            chat_eventos |= CHAT_EVENTO_INICIO;
        }
    } else if (phy_tx_sync_pendiente) {
        phy_tx_sync_pendiente = false;
        phy_tx_tipo_actual = PHY_TIPO_SYNC;
        ++phy_tx_sync_iniciados;
    } else {
        phy_tx_tipo_actual = PHY_TIPO_IDLE;
    }
}

void TIM3_IRQHandler(void)
{
    if ((TIM3->SR & TIM_SR_UIF) == 0U) {
        return;
    }
    TIM3->SR = 0U;

    if (phy_tx_ciclo == 0U) {
        PHY_Cerrar_Bloque();
        if (phy_tx_detener) {
            /* CCR4=0 se acaba de aplicar. El reloj se queda en bajo. */
            TIM3->CR1 &= ~TIM_CR1_CEN;
            phy_tx_tipo_actual = PHY_TIPO_APAGADO;
            phy_tx_ancho_actual_ms = 0U;
            PHY_Emitir_Simbolo(0U);
            if (chat_sesion_abierta) {
                Reloj_TX_Iniciar();
            }
            return;
        }
        PHY_Abrir_Bloque();
    }

    const volatile uint16_t *anchos;
    const volatile uint8_t *simbolos;
    uint32_t indice;
    if (phy_tx_tipo_actual == PHY_TIPO_NUEVO) {
        anchos = phy_tx_reloj_ms;
        simbolos = phy_tx_datos;
        indice = phy_tx_pulsos_iniciados++;
    } else {
        const PHY_BloqueControl *bloque = (phy_tx_tipo_actual == PHY_TIPO_SYNC)
                                        ? &phy_tx_sync : &phy_tx_idle;
        anchos = bloque->reloj_ms;
        simbolos = bloque->datos;
        indice = phy_tx_ciclo;
    }

    /* El hardware ya aplico el ancho precargado en esta subida. */
    PHY_Emitir_Simbolo(simbolos[indice]);
    phy_tx_ancho_actual_ms = anchos[indice];
    ++phy_tx_ciclo;
    const bool cortar_idle = phy_tx_tipo_actual == PHY_TIPO_IDLE && phy_tx_plan_listo;
    if (phy_tx_ciclo < PHY_CICLOS_BLOQUE && !cortar_idle) {
        TIM3->CCR4 = anchos[indice + 1U];
    } else {
        phy_tx_ciclo = 0U;
        const bool quedan_datos = phy_tx_plan_listo
                              && phy_tx_pulsos_iniciados < phy_tx_total_pulsos;
        phy_tx_detener = !chat_sesion_abierta && !quedan_datos;
        /* El marcador vale lo mismo para SYNC, IDLE y NUEVO. */
        TIM3->CCR4 = phy_tx_detener ? 0U : PHY_MARCADOR_MS;
    }
}

static void USART_Chat_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_USART2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USART2RST;

    /* ST-LINK VCP en NUCLEO-F303K8: USART2 AF7, PA2 TX / PA15 RX. */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFRL2) | (7U << GPIO_AFRL_AFRL2_Pos);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFRH7) | (7U << GPIO_AFRH_AFRH7_Pos);
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT_2;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(GPIO_PUPDR_PUPDR2 | GPIO_PUPDR_PUPDR15))
                 | GPIO_PUPDR_PUPDR15_0;
    GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODER2 | GPIO_MODER_MODER15))
                 | GPIO_MODER_MODER2_1 | GPIO_MODER_MODER15_1;

    /* En este K8 USART2 recibe PCLK1, igual a HSI de 8 MHz. */
    USART2->BRR = (SystemCoreClock + CHAT_BAUD / 2U) / CHAT_BAUD;
    USART2->CR3 = USART_CR3_EIE;
    USART2->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE | USART_CR1_RXNEIE;
    NVIC_ClearPendingIRQ(USART2_IRQn);
    NVIC_SetPriority(USART2_IRQn, 1U);
    NVIC_EnableIRQ(USART2_IRQn);
}

void USART2_IRQHandler(void)
{
    const uint32_t estado = USART2->ISR;
    if ((estado & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U) {
        USART2->RQR = USART_RQR_RXFRQ;
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
        chat_uart_error = true;
    } else if ((estado & USART_ISR_RXNE) != 0U) {
        const uint8_t byte = (uint8_t)USART2->RDR;
        const uint8_t siguiente = (chat_uart_entrada + 1U) % CHAT_RX_COLA;
        if (siguiente == chat_uart_salida) {
            chat_uart_error = true;
        } else {
            chat_uart_cola[chat_uart_entrada] = byte;
            chat_uart_entrada = siguiente;
        }
    }
}

static void USART_Chat_Escribir(uint8_t dato)
{
    while ((USART2->ISR & USART_ISR_TXE) == 0U) { }
    USART2->TDR = dato;
}

static void Chat_Responder(uint8_t codigo)
{
    USART_Chat_Escribir('@');
    USART_Chat_Escribir(codigo);
    USART_Chat_Escribir('\n');
}

static void Chat_Abrir_Sesion(void)
{
    chat_escuchando = false;
    chat_sesion_lista = false;
    __disable_irq();
    chat_sesion_abierta = true;
    phy_tx_sync_pendiente = true;
    chat_eventos &= (uint8_t)~CHAT_EVENTO_LISTO;
    if ((TIM3->CR1 & TIM_CR1_CEN) == 0U) {
        Reloj_TX_Iniciar();
    }
    __enable_irq();
    Chat_Responder('S');
}

static void Chat_Cerrar_Sesion(void)
{
    chat_escuchando = false;
    chat_sesion_lista = false;
    __disable_irq();
    chat_sesion_abierta = false;
    phy_tx_sync_pendiente = false;
    chat_eventos &= (uint8_t)~CHAT_EVENTO_LISTO;
    __enable_irq();
}

static void Chat_Abrir_Recepcion(void)
{
    Chat_Cerrar_Sesion();
    chat_escuchando = true;
    chat_informar_rx = true;
    chat_mensajes_informados = phy_rx_mensajes_ok == 0U ? 0U : phy_rx_mensajes_ok - 1U;
    Chat_Responder('R');
}

static void Chat_Escribir_Bytes(uint8_t codigo, uint8_t control,
                                const volatile uint8_t *bytes, uint8_t longitud)
{
    static const char hex[] = "0123456789ABCDEF";
    USART_Chat_Escribir('@');
    USART_Chat_Escribir(codigo);
    USART_Chat_Escribir(hex[control]);
    for (uint32_t i = 0U; i < longitud; ++i) {
        const uint8_t byte = bytes[i];
        USART_Chat_Escribir(hex[byte >> 4U]);
        USART_Chat_Escribir(hex[byte & 15U]);
    }
    USART_Chat_Escribir('\n');
}

static void Chat_Iniciar_Envio(const ChatComDatos *datos)
{
    chat_rx_longitud = datos->longitud;
    chat_rx_control = datos->control;
    for (uint32_t i = 0U; i < PHY_TX_MAX_BYTES; ++i) {
        chat_rx_buffer[i] = (i < datos->longitud) ? datos->bytes[i] : 0U;
    }
    ++chat_rx_total;

    PHY_TX_Preparar(datos->bytes, datos->longitud, datos->control,
                    phy_tx_proximo_id, chat_proximo_seq);
    phy_tx_proximo_id = (uint8_t)((phy_tx_proximo_id + PHY_BLOQUES_DATOS * datos->longitud)
                                % PHY_ID_MODULO);
    chat_proximo_seq = (uint8_t)((chat_proximo_seq + datos->longitud) % PHY_SEQ_MODULO);
    phy_tx_total_pulsos = PHY_CICLOS_TRAMA * datos->longitud;
    phy_tx_pulsos_iniciados = 0U;
    phy_tx_en_curso = true;

    __disable_irq();
    phy_tx_plan_listo = true;
    /* En esta prueba TX, un marcador anticipado puede cerrar el IDLE.
     * CCR4 se precarga, asi que el pulso en curso conserva su ancho.
     * Dejar 2 ms de margen al update. Si ya esta cerca, la ISR arma
     * el marcador para la siguiente subida disponible.
     */
    if (phy_tx_tipo_actual == PHY_TIPO_IDLE && phy_tx_ciclo != 0U
        && TIM3->CNT + 1U < TIM3->ARR && (TIM3->SR & TIM_SR_UIF) == 0U) {
        TIM3->CCR4 = PHY_MARCADOR_MS;
        phy_tx_ciclo = 0U;
    }
    __enable_irq();

    Chat_Escribir_Bytes('D', chat_rx_control, chat_rx_buffer, chat_rx_longitud);
}

static void Chat_Notificar_Eventos(void)
{
    __disable_irq();
    const uint8_t eventos = chat_eventos;
    chat_eventos = 0U;
    __enable_irq();

    if ((eventos & CHAT_EVENTO_FIN) != 0U) {
        phy_tx_en_curso = false;
        if (chat_sesion_abierta) {
            Chat_Responder('F');
        }
    }
    if (chat_sesion_abierta && (eventos & CHAT_EVENTO_LISTO) != 0U) {
        chat_sesion_lista = true;
        Chat_Responder('L');
    }
    if (chat_sesion_lista && (eventos & CHAT_EVENTO_INICIO) != 0U) {
        Chat_Responder('T');
    }
}

static void Chat_Actualizar(void)
{
    Chat_Notificar_Eventos();

    while (!chat_uart_error && chat_uart_salida != chat_uart_entrada) {
        const uint8_t byte = chat_uart_cola[chat_uart_salida];
        chat_uart_salida = (chat_uart_salida + 1U) % CHAT_RX_COLA;

        ChatComDatos datos;
        switch (ChatCOM_Leer(&chat_parser, byte, &datos)) {
        case CHAT_COM_ABRIR:
            Chat_Abrir_Sesion();
            break;
        case CHAT_COM_ESCUCHAR:
            Chat_Abrir_Recepcion();
            break;
        case CHAT_COM_CERRAR:
            Chat_Cerrar_Sesion();
            break;
        case CHAT_COM_DATOS:
            if (!chat_sesion_lista) {
                Chat_Responder('E');
            } else if (phy_tx_en_curso) {
                Chat_Responder('O');
            } else {
                Chat_Iniciar_Envio(&datos);
            }
            break;
        case CHAT_COM_INVALIDO:
            Chat_Responder('E');
            break;
        case CHAT_COM_NADA:
            break;
        }
    }

    if (chat_uart_error) {
        __disable_irq();
        chat_uart_salida = chat_uart_entrada;
        chat_uart_error = false;
        __enable_irq();
        chat_parser.recibiendo = false;
        Chat_Responder('E');
    }
}

static void Chat_Escribir_Decimal(uint32_t valor)
{
    uint8_t digitos[10];
    uint32_t cantidad = 0U;
    do {
        digitos[cantidad++] = (uint8_t)('0' + valor % 10U);
        valor /= 10U;
    } while (valor != 0U);
    while (cantidad != 0U) {
        USART_Chat_Escribir(digitos[--cantidad]);
    }
}

static void Chat_Informar_Medicion(void)
{
    const uint32_t valores[] = {
        rx_periodo_medido, rx_ancho_medido, phy_rx_ultimo_simbolo,
        phy_rx_pilotos_medidos ? phy_rx_mapa : 8U,
        phy_rx_reloj_adquirido, phy_rx_bloques_ok, phy_rx_tramas_ok, phy_rx_errores.total
    };
    USART_Chat_Escribir('@');
    USART_Chat_Escribir('P');
    for (uint32_t i = 0U; i < sizeof valores / sizeof valores[0]; ++i) {
        USART_Chat_Escribir(',');
        Chat_Escribir_Decimal(valores[i]);
    }
    USART_Chat_Escribir('\n');
}

static void Recepcion_Actualizar(void)
{
    bool hubo_medicion = false;
    if (rx_pulso_perdido) {
        __disable_irq();
        rx_salida = rx_entrada;
        rx_pulso_perdido = false;
        __enable_irq();
        PHY_RX_Perder_Reloj();
        hubo_medicion = true;
    }
    while (rx_salida != rx_entrada) {
        const PulsoRX pulso = rx_cola[rx_salida];
        rx_salida = (rx_salida + 1U) % RX_PULSOS_COLA;
        rx_ultimo_flanco_ms = pulso.instante_ms;
        rx_periodo_medido = pulso.periodo_ms;
        rx_ancho_medido = pulso.ancho_ms;
        PHY_RX_Procesar_Pulso(pulso.periodo_ms, pulso.ancho_ms, pulso.simbolo);
        hubo_medicion = true;
    }
    if (phy_rx_reloj_adquirido && reloj_ms - rx_ultimo_flanco_ms > RX_SIN_PULSOS_MS) {
        PHY_RX_Perder_Reloj();
        hubo_medicion = true;
    }

    if (chat_escuchando && (hubo_medicion || chat_informar_rx)) {
        chat_informar_rx = false;
        Chat_Informar_Medicion();
    }
    if (chat_escuchando && phy_rx_mensajes_ok != chat_mensajes_informados) {
        chat_mensajes_informados = phy_rx_mensajes_ok;
        Chat_Escribir_Bytes('M', phy_rx_control,
                           (const volatile uint8_t *)phy_rx_mensaje, phy_rx_longitud);
    }
}

void SysTick_Handler(void)
{
    ++reloj_ms;
}

void EXTI9_5_IRQHandler(void)
{
    if ((EXTI->PR & EXTI_PR_PR7) == 0U) {
        return;
    }
    EXTI->PR = EXTI_PR_PR7;

    const uint32_t ahora_ms = reloj_ms;
    reloj_nivel_rx = (GPIOB->IDR & GPIO_IDR_7) != 0U;
    if (reloj_nivel_rx != 0U) {
        if (reloj_pulso_abierto) {
            rx_pulso_perdido = true;
        }
        reloj_periodo_rx_ms = reloj_tiene_subida ? ahora_ms - reloj_ultima_subida_ms : 0U;
        reloj_ultima_subida_ms = ahora_ms;
        reloj_tiene_subida = true;
        reloj_pulso_abierto = true;
        ++reloj_flancos_subida;
        return;
    }

    ++reloj_flancos_bajada;
    if (!reloj_pulso_abierto) {
        return;
    }
    reloj_pulso_abierto = false;
    reloj_ancho_rx_ms = ahora_ms - reloj_ultima_subida_ms;

    const uint8_t siguiente = (rx_entrada + 1U) % RX_PULSOS_COLA;
    if (siguiente == rx_salida) {
        rx_pulso_perdido = true;
        return;
    }
    const uint8_t simbolo = (sonido_recibido ? 4U : 0U)
                          | (luz1_recibida ? 2U : 0U)
                          | (luz2_recibida ? 1U : 0U);
    rx_cola[rx_entrada].periodo_ms = reloj_periodo_rx_ms;
    rx_cola[rx_entrada].ancho_ms = reloj_ancho_rx_ms;
    rx_cola[rx_entrada].instante_ms = ahora_ms;
    rx_cola[rx_entrada].simbolo = simbolo;
    rx_entrada = siguiente;
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
    Sonido_Emitir(false);
    USART_Chat_Init();

    while (1) {
        Aplicar_Lado();
        Chat_Actualizar();
        Audio_Actualizar();
        Luz_Actualizar();
        Canales_Leer();
        Recepcion_Actualizar();
    }
}
