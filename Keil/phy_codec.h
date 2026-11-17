#ifndef PHY_CODEC_H
#define PHY_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PHY_PERIODO_MS = 1000,
    PHY_ID_MODULO = 6,
    PHY_SEQ_MODULO = 8,
    PHY_CICLOS_CORTOS = 11,
    PHY_CICLOS_LARGOS = 15,
    PHY_MARCADOR = 4,
    PHY_MAPA_COMPLETO = 7,
    PHY_CICLO_RETORNO = 5,
    PHY_CICLO_CABECERA = 10,
    PHY_CONTROL_UTF8 = 8,
    PHY_CONTROL_ACK = 1,
    PHY_CONTROL_NACK = 9,
    PHY_SYN_SFD = 0x3c0,
    PHY_MASCARA_SYN_SFD = 0xfc0
};

typedef enum { PHY_IDLE, PHY_NUEVO, PHY_REPITE } PhyTipo;

typedef enum {
    PHY_OK, PHY_ERR_CABECERA, PHY_ERR_PILOTOS, PHY_ERR_PARIDAD,
    PHY_ERR_XOR, PHY_ERR_ESTRUCTURA = 6, PHY_ERR_CRC_TRAMA
} PhyResultado;

typedef struct {
    bool pedir;
    uint8_t siguiente;
    uint8_t mapa;
} PhyRetorno;

typedef struct {
    PhyTipo tipo;
    uint8_t id;
    uint8_t mapa;
} PhyCabecera;

typedef struct {
    uint8_t ciclos;
    uint8_t pares[PHY_CICLOS_LARGOS];
    uint8_t simbolos[PHY_CICLOS_LARGOS];
} PhyBloque;

typedef struct {
    uint8_t control, dato, ack, seq;
    bool inicio, fin;
    uint8_t crc_recibido, crc_calculado;
} PhyTrama;

uint8_t PHY_CRC8(uint8_t control, const uint8_t bytes[], size_t longitud);
int PHY_Par_Reloj(uint32_t ancho_ms);
uint16_t PHY_Ancho_Reloj(uint8_t par);
uint8_t PHY_Mapa_Pilotos(uint8_t bajo, uint8_t alto);
bool PHY_Leer_Retorno(const uint8_t pares[], PhyRetorno *retorno);
bool PHY_Leer_Cabecera(const uint8_t pares[], PhyCabecera *cabecera);
void PHY_Codificar_Bloque(PhyBloque *bloque, PhyRetorno retorno, PhyCabecera cabecera, uint16_t valor);
PhyResultado PHY_Decodificar_Bloque(const PhyBloque *bloque, uint8_t mapa, uint16_t *valor);
void PHY_Codificar_Trama(PhyTrama trama, uint16_t bloques[3]);
PhyResultado PHY_Decodificar_Trama(const uint16_t bloques[3], PhyTrama *trama);

#endif
