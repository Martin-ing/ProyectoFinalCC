#ifndef VERDES_H
#define VERDES_H

#include "stm32f3xx.h"
#include <stdbool.h>
#include <stdint.h>

//pot
void ADC1_Init_Discontinuous_TIM2(void);
void TIM2_TRGO_Init(uint16_t periodo_ms);
void intToChar4(int value, uint8_t mode, char *outStr);

//lcd
void LCD_PrintCommand(const char *args);

//pot2lcd

uint16_t POT2LCD_ReadRaw(void);
extern uint8_t inPot2Seg;
extern uint8_t inPot2SegMode;
void POT2LCD_Init(void);
void POT2LCD_Task(void);      

//RGB
void PWM_PA8_config(void);
void PWM_PA11_config(void);
void PWM_PB5_config(void);
void PWM_SetDuty_RED(uint16_t duty);
void PWM_SetDuty_GREEN(uint16_t duty);
void PWM_SetDuty_BLUE(uint16_t duty);
void PWM_SetColor(uint32_t hexColor);
void PWM_Transition(uint32_t startColor, uint32_t endColor, uint16_t steps, uint16_t delay_time);

//sound
void PWM_Buzzer_Init(void);
void PWM_Buzzer_SetFrequency(uint32_t freq);

void Audio_Init(void);
void Audio_Actualizar(void);
extern volatile uint32_t nivel_entrada;
extern volatile uint32_t detectado_3khz;
extern volatile uint32_t detectado_4khz;
extern volatile uint32_t energia_3khz;
extern volatile uint32_t energia_4khz;
extern volatile uint32_t umbral_3khz;
extern volatile uint32_t umbral_4khz;

// Canales de datos
typedef enum {
    LADO_PRUEBA = 0,
    LADO_1 = 1,
    LADO_2 = 2
} LadoComunicacion;

extern volatile LadoComunicacion lado_comunicacion;
extern volatile uint32_t sonido_tx_hz;
extern volatile uint32_t sonido_rx_hz;
bool Configurar_Lado(LadoComunicacion lado);
void Sonido_Emitir(bool encender);
void Canales_Leer(void);
extern volatile uint32_t sonido_recibido;
extern volatile uint32_t luz1_recibida;
extern volatile uint32_t luz2_recibida;

// Luz
void Laser1_Encender(void);
void Laser1_Apagar(void);
void Laser2_Encender(void);
void Laser2_Apagar(void);
void Laser_Encender(void);  /* Alias del canal 1. */
void Laser_Apagar(void);
extern volatile uint16_t luz_adc;
extern volatile uint16_t luz_umbral_adc;
extern volatile uint32_t luz_detectada;
extern volatile uint16_t luz2_adc;
extern volatile uint16_t luz2_umbral_adc;
extern volatile uint32_t luz2_detectada;

// Reloj entre placas
extern volatile uint32_t reloj_nivel_rx;
extern volatile uint32_t reloj_periodo_rx_ms;
extern volatile uint32_t reloj_ancho_rx_ms;
extern volatile uint32_t reloj_flancos_subida;
extern volatile uint32_t reloj_flancos_bajada;

//voltaje
void Init_DAC(void);

//segment
extern char number[4];
void num_to_seg(char number);

//lcdwatch
typedef enum {
    LCDWATCH_FMT_DEC = 0,
    LCDWATCH_FMT_HEX = 1
} LCDWATCH_Format;

typedef enum {
    LCDWATCH_SIZE_BYTE     = 0,   // 8 bits
    LCDWATCH_SIZE_HALFWORD = 1,   // 16 bits
    LCDWATCH_SIZE_WORD     = 2    // 32 bits
} LCDWATCH_Size;

void LCDWATCH_Init(void);
void LCDWATCH_ClearAll(void);
void LCDWATCH_Add(uint32_t addr, LCDWATCH_Format fmt, LCDWATCH_Size size);
void LCDWATCH_Task(void);

// Handler para el comando del monitor v?a USART
void CMD_LCDWATCH(char *args);


#endif
