#include "Verdes.h"
#include "chat_com.h"
#include "phy_link.h"

#include <string.h>

#ifndef PHY_LADO
#define PHY_LADO 1
#endif
#if PHY_LADO != 1 && PHY_LADO != 2
#error PHY_LADO debe ser 1 o 2
#endif

enum {
    RELOJ_TICKS_HZ = 1000,
    RELOJ_SUBIDA_TICK = 1,
    UART_RX_CAPACIDAD = 256,
    UART_TX_CAPACIDAD = 2048,
    EVENTOS_CAPACIDAD = 32
};

volatile uint8_t lado = PHY_LADO;
volatile uint8_t lado_activo;
volatile uint32_t sonido_tx_hz, sonido_rx_hz;
volatile uint32_t sonido_recibido, luz1_recibida, luz2_recibida;

volatile uint16_t luz_adc, luz_umbral_adc = 2048;
volatile uint16_t luz2_adc, luz2_umbral_adc = 2048;
volatile uint32_t luz_detectada, luz2_detectada;

volatile uint32_t reloj_nivel_rx, reloj_periodo_rx_ms, reloj_ancho_rx_ms;
volatile uint32_t reloj_flancos_subida, reloj_flancos_bajada;
volatile uint32_t reloj_nivel_tx, reloj_ancho_tx_ms;
volatile uint8_t phy_tx_simbolo_actual, phy_rx_simbolo_actual;
volatile uint8_t phy_tx_ciclo, phy_rx_simbolo_sensores;

static volatile uint32_t reloj_ms;
static uint32_t reloj_ultima_subida_ms;
static bool reloj_tiene_subida, reloj_pulso_abierto, sonido_encendido, leyendo_luz2;
static PhyEnlace enlace;

static ChatComParser parser;
static volatile bool usb_activo, uart_error;
static volatile uint16_t uart_rx_entrada, uart_rx_salida, uart_tx_entrada, uart_tx_salida;
static volatile uint8_t uart_rx[UART_RX_CAPACIDAD], uart_tx[UART_TX_CAPACIDAD];

static PhyEvento eventos[EVENTOS_CAPACIDAD];
static volatile uint8_t eventos_entrada, eventos_salida;
static volatile uint32_t eventos_perdidos;
static uint8_t mensaje_rx[PHY_MAX_MENSAJE], mensaje_rx_control;
static volatile uint8_t mensaje_rx_longitud;

static uint32_t Bloquear_IRQ(void)
{
    const uint32_t previo = __get_PRIMASK();
    __disable_irq();
    return previo;
}

static void Restaurar_IRQ(uint32_t previo)
{
    __set_PRIMASK(previo);
}

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
    phy_rx_simbolo_sensores = (sonido_recibido ? 4U : 0U)
                            | (luz1_recibida ? 2U : 0U)
                            | (luz2_recibida ? 1U : 0U);
}

static void Aplicar_Lado(void)
{
    const uint8_t solicitado = lado;
    if (solicitado != 1U && solicitado != 2U) {
        lado = lado_activo;
        return;
    }
    lado_activo = solicitado;
    sonido_tx_hz = solicitado == 1U ? 3000U : 4000U;
    sonido_rx_hz = solicitado == 1U ? 4000U : 3000U;
}

static void PHY_Emitir_Simbolo(uint8_t simbolo)
{
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
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;
    (void)RCC->AHBENR;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    (void)RCC->APB1ENR;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;

    /* D6 (PB1) = TIM3_CH4, AF2. Las dos transiciones son por comparación. */
    RCC->APB1RSTR |= RCC_APB1RSTR_TIM3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM3RST;
    TIM3->PSC = SystemCoreClock / RELOJ_TICKS_HZ - 1U;
    TIM3->ARR = PHY_PERIODO_MS - 1U;
    TIM3->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
    TIM3->CCMR2 = 4U << TIM_CCMR2_OC4M_Pos; /* Referencia inicialmente baja. */
    TIM3->CCR4 = RELOJ_SUBIDA_TICK;
    TIM3->EGR = TIM_EGR_UG;
    TIM3->CCER = TIM_CCER_CC4E;
    TIM3->CCMR2 = 3U << TIM_CCMR2_OC4M_Pos; /* Toggle al coincidir CNT y CCR4. */
    TIM3->SR = 0U;

    GPIOB->OTYPER &= ~GPIO_OTYPER_OT_1;
    GPIOB->PUPDR &= ~GPIO_PUPDR_PUPDR1;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~GPIO_AFRL_AFRL1) | (2U << GPIO_AFRL_AFRL1_Pos);
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER1) | GPIO_MODER_MODER1_1;

    /* SB18 puede unir A4 (PA5) con D4. PA5 queda como entrada flotante. */
    GPIOA->MODER &= ~GPIO_MODER_MODER5;
    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPDR5;
    GPIOB->MODER &= ~GPIO_MODER_MODER7;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~GPIO_PUPDR_PUPDR7) | GPIO_PUPDR_PUPDR7_1;
    SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~SYSCFG_EXTICR2_EXTI7) | SYSCFG_EXTICR2_EXTI7_PB;
    EXTI->RTSR |= EXTI_RTSR_TR7;
    EXTI->FTSR |= EXTI_FTSR_TR7;
    EXTI->PR = EXTI_PR_PR7;
    reloj_nivel_rx = (GPIOB->IDR & GPIO_IDR_7) != 0U;

    /* TIM3 y EXTI7 tienen igual prioridad y no se interrumpen entre sí. */
    NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
    NVIC_SetPriority(EXTI9_5_IRQn, 1U);
    EXTI->IMR |= EXTI_IMR_MR7;
    NVIC_EnableIRQ(EXTI9_5_IRQn);
    NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_SetPriority(TIM3_IRQn, 1U);
    TIM3->DIER = TIM_DIER_CC4IE;
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM3_STOP;
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CR1 |= TIM_CR1_CEN;
}

void TIM3_IRQHandler(void)
{
    if ((TIM3->SR & TIM_SR_CC4IF) == 0U) {
        return;
    }
    TIM3->SR = 0U;
    const uint32_t ms = reloj_ms;
    reloj_nivel_tx ^= 1U;
    if (reloj_nivel_tx == 0U) {
        TIM3->CCR4 = RELOJ_SUBIDA_TICK;
        PHY_TX_Bajada(&enlace, ms);
        return;
    }

    const PhyPulso pulso = PHY_TX_Subida(&enlace, ms);
    TIM3->CCR4 = RELOJ_SUBIDA_TICK + pulso.ancho_ms;
    if (pulso.ciclo == 0U) {
        Aplicar_Lado();
    }
    phy_tx_ciclo = pulso.ciclo;
    reloj_ancho_tx_ms = pulso.ancho_ms;
    PHY_Emitir_Simbolo(pulso.simbolo);
}

void EXTI9_5_IRQHandler(void)
{
    if ((EXTI->PR & EXTI_PR_PR7) == 0U) {
        return;
    }
    EXTI->PR = EXTI_PR_PR7;
    const uint32_t ms = reloj_ms;
    reloj_nivel_rx = (GPIOB->IDR & GPIO_IDR_7) != 0U;
    if (reloj_nivel_rx != 0U) {
        if (reloj_pulso_abierto) {
            PHY_RX_Perder_Reloj(&enlace, ms);
        }
        reloj_periodo_rx_ms = reloj_tiene_subida ? ms - reloj_ultima_subida_ms : 0U;
        reloj_ultima_subida_ms = ms;
        reloj_tiene_subida = true;
        reloj_pulso_abierto = true;
        ++reloj_flancos_subida;
        return;
    }

    ++reloj_flancos_bajada;
    if (reloj_pulso_abierto) {
        reloj_pulso_abierto = false;
        reloj_ancho_rx_ms = ms - reloj_ultima_subida_ms;
        phy_rx_simbolo_actual = phy_rx_simbolo_sensores;
        PHY_RX_Pulso(&enlace, ms, reloj_periodo_rx_ms, reloj_ancho_rx_ms, phy_rx_simbolo_actual);
    }
}

void SysTick_Handler(void)
{
    ++reloj_ms;
}

static void USART_Chat_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;
    RCC->APB1RSTR |= RCC_APB1RSTR_USART2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USART2RST;
    /* ST-LINK VCP: PA2 TX y PA15 RX en AF7. */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFRL2) | (7U << GPIO_AFRL_AFRL2_Pos);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFRH7) | (7U << GPIO_AFRH_AFRH7_Pos);
    GPIOA->OTYPER &= ~GPIO_OTYPER_OT_2;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(GPIO_PUPDR_PUPDR2 | GPIO_PUPDR_PUPDR15)) | GPIO_PUPDR_PUPDR15_0;
    GPIOA->MODER = (GPIOA->MODER & ~(GPIO_MODER_MODER2 | GPIO_MODER_MODER15)) | GPIO_MODER_MODER2_1 | GPIO_MODER_MODER15_1;
    USART2->BRR = (SystemCoreClock + CHAT_BAUD / 2U) / CHAT_BAUD;
    USART2->CR3 = USART_CR3_EIE;
    USART2->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE | USART_CR1_RXNEIE;
    NVIC_ClearPendingIRQ(USART2_IRQn);
    NVIC_SetPriority(USART2_IRQn, 2U);
    NVIC_EnableIRQ(USART2_IRQn);
}

void USART2_IRQHandler(void)
{
    const uint32_t estado = USART2->ISR;
    if ((estado & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U) {
        USART2->RQR = USART_RQR_RXFRQ;
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
        uart_error = true;
    } else if ((estado & USART_ISR_RXNE) != 0U) {
        const uint8_t byte = (uint8_t)USART2->RDR;
        const uint16_t siguiente = (uart_rx_entrada + 1U) % UART_RX_CAPACIDAD;
        if (siguiente == uart_rx_salida) {
            uart_error = true;
        } else {
            uart_rx[uart_rx_entrada] = byte;
            uart_rx_entrada = siguiente;
        }
    }
    if ((estado & USART_ISR_TXE) != 0U && (USART2->CR1 & USART_CR1_TXEIE) != 0U) {
        if (uart_tx_salida == uart_tx_entrada) {
            USART2->CR1 &= ~USART_CR1_TXEIE;
        } else {
            USART2->TDR = uart_tx[uart_tx_salida];
            uart_tx_salida = (uart_tx_salida + 1U) % UART_TX_CAPACIDAD;
        }
    }
}

static uint16_t Espacio_UART(void)
{
    return (uint16_t)((uart_tx_salida + UART_TX_CAPACIDAD - uart_tx_entrada - 1U) % UART_TX_CAPACIDAD);
}

static bool Encolar_UART(const char *datos, size_t longitud)
{
    if (longitud > Espacio_UART()) {
        return false;
    }
    uint16_t entrada = uart_tx_entrada;
    for (size_t i = 0; i < longitud; ++i) {
        uart_tx[entrada] = (uint8_t)datos[i];
        entrada = (entrada + 1U) % UART_TX_CAPACIDAD;
    }
    const uint32_t previo = Bloquear_IRQ();
    uart_tx_entrada = entrada;
    USART2->CR1 |= USART_CR1_TXEIE;
    Restaurar_IRQ(previo);
    return true;
}

static char *Poner_Numero(char *puntero, uint32_t valor)
{
    char digitos[10];
    unsigned n = 0;
    do {
        digitos[n++] = (char)('0' + valor % 10U);
        valor /= 10U;
    } while (valor != 0U);
    do {
        *puntero++ = digitos[--n];
    } while (n != 0U);
    return puntero;
}

static void Chat_Enviar_Campos(char tipo, const uint32_t campos[], size_t n)
{
    char linea[256];
    char *p = linea;
    *p++ = '@';
    *p++ = tipo;
    for (size_t i = 0; i < n; ++i) {
        *p++ = ',';
        p = Poner_Numero(p, campos[i]);
    }
    *p++ = '\n';
    if (!Encolar_UART(linea, (size_t)(p - linea))) {
        const uint32_t previo = Bloquear_IRQ();
        ++eventos_perdidos;
        Restaurar_IRQ(previo);
    }
}

static void Chat_Enviar_Error(uint32_t ticket, uint32_t motivo)
{
    const uint32_t campos[] = {ticket, motivo};
    Chat_Enviar_Campos('E', campos, 2);
}

static void Chat_Enviar_Estado(void)
{
    const uint32_t previo = Bloquear_IRQ();
    const uint32_t campos[] = {
        CHAT_VERSION, lado_activo, PHY_Mapa_TX(&enlace), enlace.mapa_rx, enlace.reloj_rx,
        reloj_periodo_rx_ms, reloj_ancho_rx_ms, phy_rx_simbolo_actual, enlace.bloques_rx_ok,
        enlace.tramas_rx_ok, enlace.errores, enlace.reenvios_phy, enlace.reenvios_trama,
        enlace.falla, enlace.ticket_tx, enlace.estado_envio, eventos_perdidos, reloj_ms
    };
    Restaurar_IRQ(previo);
    Chat_Enviar_Campos('H', campos, sizeof campos / sizeof campos[0]);
}

static void Chat_Procesar_Comando(ChatComAccion accion, const ChatComDatos *datos)
{
    if (accion == CHAT_ABRIR) {
        usb_activo = true;
        Chat_Enviar_Estado();
    } else if (accion == CHAT_CERRAR) {
        const uint32_t previo = Bloquear_IRQ();
        usb_activo = false;
        eventos_salida = eventos_entrada;
        Restaurar_IRQ(previo);
    } else if (!usb_activo) {
        return;
    } else if (accion == CHAT_ESTADO) {
        Chat_Enviar_Estado();
    } else if (accion == CHAT_INVALIDO) {
        Chat_Enviar_Error(datos->ticket, CHAT_ERR_FORMATO);
    } else if (accion == CHAT_ENVIAR) {
        const uint32_t previo = Bloquear_IRQ();
        const bool duplicado = enlace.ticket_tx == datos->ticket;
        const bool aceptado = !duplicado && PHY_Encolar(&enlace, datos->ticket, datos->control, datos->bytes, datos->longitud);
        const PhyEstadoEnvio estado = enlace.estado_envio;
        const PhyMotivo falla = enlace.falla;
        Restaurar_IRQ(previo);
        if (aceptado || (duplicado && estado == PHY_ENVIO_ENCOLADO)) {
            Chat_Enviar_Campos('Q', &datos->ticket, 1);
        } else if (duplicado && estado == PHY_ENVIO_ENTREGADO) {
            Chat_Enviar_Campos('K', &datos->ticket, 1);
        } else {
            Chat_Enviar_Error(datos->ticket, falla ? (uint32_t)falla : CHAT_ERR_OCUPADO);
        }
    }
}

static void Chat_Evento_PHY(void *contexto, const PhyEvento *evento)
{
    (void)contexto;
    if (!usb_activo) {
        return;
    }
    const uint8_t siguiente = (eventos_entrada + 1U) % EVENTOS_CAPACIDAD;
    if (siguiente == eventos_salida) {
        ++eventos_perdidos;
    } else {
        eventos[eventos_entrada] = *evento;
        eventos_entrada = siguiente;
    }
}

static void Chat_Recibir_Mensaje(void *contexto, uint8_t control, const uint8_t bytes[], uint8_t longitud)
{
    (void)contexto;
    if (mensaje_rx_longitud != 0U) {
        ++eventos_perdidos;
    }
    memcpy(mensaje_rx, bytes, longitud);
    mensaje_rx_control = control;
    mensaje_rx_longitud = longitud;
}

static void Chat_Enviar_Mensaje_RX(void)
{
    const uint32_t previo = Bloquear_IRQ();
    uint8_t bytes[PHY_MAX_MENSAJE];
    const uint8_t longitud = mensaje_rx_longitud;
    const uint8_t control = mensaje_rx_control;
    memcpy(bytes, mensaje_rx, longitud);
    mensaje_rx_longitud = 0;
    Restaurar_IRQ(previo);
    if (longitud == 0U) {
        return;
    }

    static const char hex[] = "0123456789abcdef";
    char linea[2 * PHY_MAX_MENSAJE + 7];
    char *p = linea;
    *p++ = '@';
    *p++ = 'M';
    *p++ = ',';
    p = Poner_Numero(p, control);
    *p++ = ',';
    for (unsigned i = 0; i < longitud; ++i) {
        *p++ = hex[bytes[i] >> 4];
        *p++ = hex[bytes[i] & 15U];
    }
    *p++ = '\n';
    (void)Encolar_UART(linea, (size_t)(p - linea));
}

static void Chat_Enviar_Evento(void)
{
    const uint32_t previo = Bloquear_IRQ();
    if (eventos_entrada == eventos_salida) {
        Restaurar_IRQ(previo);
        return;
    }
    const PhyEvento ev = eventos[eventos_salida];
    eventos_salida = (eventos_salida + 1U) % EVENTOS_CAPACIDAD;
    Restaurar_IRQ(previo);
    if (ev.tipo == PHY_EV_ENTREGADO) {
        Chat_Enviar_Campos('K', &ev.ticket, 1);
    } else if (ev.tipo == PHY_EV_FALLA) {
        Chat_Enviar_Error(ev.ticket, ev.motivo);
    }
    const uint32_t campos[] = {
        ev.ms, ev.tipo, ev.ticket, ev.bloque.tipo, ev.bloque.id, ev.bloque.mapa,
        ev.retorno.pedir, ev.retorno.siguiente, ev.retorno.mapa, ev.ciclo, ev.valor,
        ev.trama.control, ev.trama.dato, ev.trama.ack, ev.trama.seq,
        (ev.trama.inicio ? 1U : 0U) | (ev.trama.fin ? 2U : 0U),
        ev.trama.crc_recibido, ev.trama.crc_calculado, ev.motivo, ev.resultado, ev.orden
    };
    Chat_Enviar_Campos('G', campos, sizeof campos / sizeof campos[0]);
}

static void Chat_Actualizar(void)
{
    if (uart_error) {
        const uint32_t previo = Bloquear_IRQ();
        uart_rx_salida = uart_rx_entrada;
        uart_error = false;
        Restaurar_IRQ(previo);
        ChatCom_Reiniciar(&parser);
        if (usb_activo) {
            Chat_Enviar_Error(0, CHAT_ERR_FORMATO);
        }
    }
    for (unsigned i = 0; i < 32 && uart_rx_salida != uart_rx_entrada; ++i) {
        const uint8_t byte = uart_rx[uart_rx_salida];
        uart_rx_salida = (uart_rx_salida + 1U) % UART_RX_CAPACIDAD;
        ChatComDatos datos;
        const ChatComAccion accion = ChatCom_Recibir(&parser, byte, &datos);
        if (accion != CHAT_NADA) {
            Chat_Procesar_Comando(accion, &datos);
        }
    }
    if (usb_activo && Espacio_UART() >= 512U) {
        if (mensaje_rx_longitud != 0U) {
            Chat_Enviar_Mensaje_RX();
        }
        if (eventos_entrada != eventos_salida) {
            Chat_Enviar_Evento();
        }
    }
}

int main(void)
{
    Reloj_8MHz_Init();
    PWM_Buzzer_Init();
    Luz_Init();
    Audio_Init();
    Aplicar_Lado();
    Sonido_Emitir(false);
    const PhyReceptor receptor = {Chat_Evento_PHY, Chat_Recibir_Mensaje, NULL};
    PHY_Iniciar(&enlace, receptor);
    SysTick_Config(SystemCoreClock / RELOJ_TICKS_HZ);
    NVIC_SetPriority(SysTick_IRQn, 0U);
    USART_Chat_Init();
    Reloj_Canales_Init();
    for (;;) {
        Audio_Actualizar();
        Luz_Actualizar();
        Canales_Leer();
        Chat_Actualizar();
    }
}

