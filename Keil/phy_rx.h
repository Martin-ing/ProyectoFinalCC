#ifndef PHY_RX_H
#define PHY_RX_H

#include <stdbool.h>
#include <stdint.h>

#define PHY_RX_MAX_MENSAJE 64U

typedef struct {
    uint32_t reloj;
    uint32_t cabecera;
    uint32_t pilotos;
    uint32_t simbolos;
    uint32_t trama;
    uint32_t crc;
    uint32_t secuencia;
    uint32_t total;
} PHY_RxErrores;

extern volatile bool phy_rx_reloj_adquirido;
extern volatile bool phy_rx_pilotos_medidos;
extern volatile uint8_t phy_rx_mapa;
extern volatile uint8_t phy_rx_ultimo_simbolo;
extern volatile uint8_t phy_rx_ultimo_id;
extern volatile uint8_t phy_rx_ultimo_dato;
extern volatile uint8_t phy_rx_ultimo_seq;
extern volatile uint8_t phy_rx_crc_recibido;
extern volatile uint8_t phy_rx_crc_calculado;
extern volatile uint32_t phy_rx_bloques_ok;
extern volatile uint32_t phy_rx_tramas_ok;
extern volatile uint32_t phy_rx_sync_ok;
extern volatile uint32_t phy_rx_mensajes_ok;
extern volatile PHY_RxErrores phy_rx_errores;

extern volatile char phy_rx_mensaje[PHY_RX_MAX_MENSAJE + 1U];
extern volatile uint8_t phy_rx_longitud;
extern volatile uint8_t phy_rx_control;
extern volatile uint8_t phy_rx_bytes_en_curso;

void PHY_RX_Perder_Reloj(void);
void PHY_RX_Procesar_Pulso(uint32_t periodo_ms, uint32_t ancho_ms, uint8_t simbolo);

#endif
