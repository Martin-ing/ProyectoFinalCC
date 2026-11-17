#include "phy_tx.h"
#include <stdbool.h>

volatile uint8_t phy_tx_crc[PHY_TX_MAX_BYTES];
volatile uint16_t phy_tx_bloques[PHY_TX_MAX_BYTES * PHY_BLOQUES_DATOS];
volatile uint16_t phy_tx_reloj_ms[PHY_TX_CICLOS_MAX];
volatile uint8_t phy_tx_datos[PHY_TX_CICLOS_MAX];

typedef struct {
    uint8_t polinomio;
    uint8_t inicial;
    uint8_t xor_final;
    bool reflejado;
} VarianteCRC8;

/* Indice C2:C1. Para MAXIM-DOW se usa 0x31 reflejado (0x8C). */
static const VarianteCRC8 variantes[] = {
    {0x07U, 0x00U, 0x00U, false},  /* SMBUS */
    {0x2FU, 0xFFU, 0xFFU, false},  /* AUTOSAR */
    {0x1DU, 0xFFU, 0xFFU, false},  /* SAE-J1850 */
    {0x8CU, 0x00U, 0x00U, true},   /* MAXIM-DOW */
};

uint8_t PHY_CRC8(uint8_t control, const uint8_t *bytes, uint32_t longitud)
{
    const VarianteCRC8 *variante = &variantes[(control >> 1U) & 3U];
    uint8_t crc = variante->inicial;
    for (uint32_t i = 0U; i < longitud; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint8_t saliente = variante->reflejado ? crc & 1U : crc >> 7U;
            crc = (uint8_t)(variante->reflejado ? crc >> 1U : crc << 1U);
            if (saliente != 0U) {
                crc ^= variante->polinomio;
            }
        }
    }
    return crc ^ variante->xor_final;
}

static uint16_t PHY_Ancho_Par(uint8_t par)
{
    return (uint16_t)(400U + 100U * par);
}

static void PHY_Preparar_Seccion(volatile uint16_t *reloj_ms, uint8_t campos)
{
    uint8_t paridad = 0U;
    for (uint32_t i = 0U; i < 4U; ++i) {
        const uint8_t par = (uint8_t)((campos >> (6U - 2U * i)) & 3U);
        reloj_ms[i] = PHY_Ancho_Par(par);
        paridad ^= par;
    }
    reloj_ms[4] = PHY_Ancho_Par(paridad);
}

static void PHY_Preparar_Bloque(volatile uint16_t *reloj_ms, volatile uint8_t *datos,
                               uint8_t tipo, uint8_t id, uint16_t valor12)
{
    const bool nuevo = tipo == PHY_TIPO_NUEVO;
    reloj_ms[0] = PHY_MARCADOR_MS;
    /* PEDIR=0, SIG_RX=000 y MAPA_RX=000 durante esta prueba de TX. */
    PHY_Preparar_Seccion(&reloj_ms[1], 0U);
    const uint8_t campos_tx = (uint8_t)((tipo << 6U) | (id << 3U) | (nuevo ? 7U : 0U));
    PHY_Preparar_Seccion(&reloj_ms[6], campos_tx);

    datos[0] = 0U;
    datos[1] = 7U;
    datos[9] = 0U;
    datos[10] = 7U;

    uint8_t xor_lineas = 0U;
    for (uint32_t i = 0U; i < 6U; ++i) {
        const uint8_t par = (uint8_t)((valor12 >> (10U - 2U * i)) & 3U);
        const uint8_t paridad_impar = (uint8_t)(1U ^ (par >> 1U) ^ (par & 1U));
        const uint8_t simbolo = nuevo ? (uint8_t)((par << 1U) | paridad_impar) : 0U;
        datos[2U + i] = simbolo;
        xor_lineas ^= simbolo;
    }
    datos[8] = xor_lineas;
}

void PHY_TX_Preparar_Control(PHY_BloqueControl *bloque, uint8_t tipo)
{
    PHY_Preparar_Bloque(bloque->reloj_ms, bloque->datos, tipo, 0U, 0U);
}

void PHY_TX_Preparar(const uint8_t *bytes, uint8_t longitud, uint8_t control,
                     uint8_t primer_id, uint8_t primer_seq)
{
    for (uint32_t trama = 0U; trama < longitud; ++trama) {
        const uint8_t entrada_crc[2] = {control, bytes[trama]};
        const uint8_t crc = PHY_CRC8(control, entrada_crc, 2U);
        const uint8_t seq = (uint8_t)((primer_seq + trama) % PHY_SEQ_MODULO);
        const uint8_t fin = (trama + 1U == longitud) ? 2U : 0U;
        const uint32_t base = trama * PHY_BLOQUES_DATOS;

        phy_tx_crc[trama] = crc;
        phy_tx_bloques[base] = (uint16_t)((1U << 9U) | (7U << 6U) | (control << 2U));
        phy_tx_bloques[base + 1U] = (uint16_t)(((uint16_t)bytes[trama] << 4U) | (crc >> 4U));
        /* CRC bajo, ACK=000, SEQ, FIN, relleno=0. */
        phy_tx_bloques[base + 2U] = (uint16_t)(((crc & 15U) << 8U) | (seq << 2U) | fin);

        for (uint32_t i = base; i < base + PHY_BLOQUES_DATOS; ++i) {
            const uint8_t id = (uint8_t)((primer_id + i) % PHY_ID_MODULO);
            const uint32_t posicion = i * PHY_CICLOS_BLOQUE;
            PHY_Preparar_Bloque(&phy_tx_reloj_ms[posicion], &phy_tx_datos[posicion],
                               PHY_TIPO_NUEVO, id, phy_tx_bloques[i]);
        }
    }
}
