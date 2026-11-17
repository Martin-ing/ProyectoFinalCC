#ifndef PHY_TX_H
#define PHY_TX_H

#include <stdint.h>

#define PHY_CICLOS_BLOQUE       11U
#define PHY_BLOQUES_DATOS        3U
#define PHY_CICLOS_TRAMA        (PHY_BLOQUES_DATOS * PHY_CICLOS_BLOQUE)
#define PHY_TX_MAX_BYTES         4U
#define PHY_TX_CICLOS_MAX      (PHY_TX_MAX_BYTES * PHY_CICLOS_TRAMA)
#define PHY_ID_MODULO            6U
#define PHY_SEQ_MODULO           8U
#define PHY_PERIODO_MS        1000U
#define PHY_MARCADOR_MS        800U
#define PHY_CONTROL_UTF8         8U
#define PHY_TIPO_IDLE            0U
#define PHY_TIPO_NUEVO           1U
#define PHY_TIPO_SYNC            3U
#define PHY_TIPO_APAGADO       255U

typedef struct {
    uint16_t reloj_ms[PHY_CICLOS_BLOQUE];
    uint8_t datos[PHY_CICLOS_BLOQUE];
} PHY_BloqueControl;

extern volatile uint8_t phy_tx_crc[PHY_TX_MAX_BYTES];
extern volatile uint16_t phy_tx_bloques[PHY_TX_MAX_BYTES * PHY_BLOQUES_DATOS];
extern volatile uint16_t phy_tx_reloj_ms[PHY_TX_CICLOS_MAX];
extern volatile uint8_t phy_tx_datos[PHY_TX_CICLOS_MAX];

uint8_t PHY_CRC8(uint8_t control, const uint8_t *bytes, uint32_t longitud);
void PHY_TX_Preparar_Control(PHY_BloqueControl *bloque, uint8_t tipo);
void PHY_TX_Preparar(const uint8_t *bytes, uint8_t longitud, uint8_t control,
                     uint8_t primer_id, uint8_t primer_seq);

#endif
