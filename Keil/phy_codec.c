#include "phy_codec.h"

typedef struct {
    uint8_t polinomio, inicial, xor_final;
    bool reflejado;
} VarianteCRC8;

static const VarianteCRC8 variantes[] = {
    {0x07, 0x00, 0x00, false}, /* SMBUS */
    {0x2f, 0xff, 0xff, false}, /* AUTOSAR */
    {0x1d, 0xff, 0xff, false}, /* SAE-J1850 */
    {0x8c, 0x00, 0x00, true}   /* MAXIM-DOW, 0x31 reflejado */
};

uint8_t PHY_CRC8(uint8_t control, const uint8_t bytes[], size_t longitud)
{
    const VarianteCRC8 variante = variantes[(control >> 1) & 3U];
    uint8_t crc = variante.inicial;
    for (size_t i = 0; i < longitud; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint8_t sale = variante.reflejado ? crc & 1U : crc >> 7;
            crc = (uint8_t)(variante.reflejado ? crc >> 1 : crc << 1);
            if (sale != 0U) {
                crc ^= variante.polinomio;
            }
        }
    }
    return crc ^ variante.xor_final;
}

uint8_t PHY_CRC_Bloque(uint16_t valor)
{
    const uint8_t bytes[2] = {(uint8_t)((valor >> 8) & 15U), (uint8_t)valor};
    return PHY_CRC8(0, bytes, sizeof bytes);
}

int PHY_Par_Reloj(uint32_t ancho_ms)
{
    for (int par = 0; par <= PHY_MARCADOR; ++par) {
        const uint32_t nominal = PHY_Ancho_Reloj((uint8_t)par);
        if (ancho_ms >= nominal - 25U && ancho_ms <= nominal + 25U) {
            return par;
        }
    }
    return -1;
}

uint16_t PHY_Ancho_Reloj(uint8_t par)
{
    return (uint16_t)(400U + 100U * par);
}

uint8_t PHY_Mapa_Pilotos(uint8_t bajo, uint8_t alto)
{
    return (uint8_t)(~bajo & alto & PHY_MAPA_COMPLETO);
}

static void Codificar_Seccion(uint8_t pares[], uint8_t campos)
{
    uint8_t paridad = 0;
    for (unsigned i = 0; i < 4; ++i) {
        pares[i] = (campos >> (6 - 2 * i)) & 3U;
        paridad ^= pares[i];
    }
    pares[4] = paridad;
}

static bool Leer_Seccion(const uint8_t pares[], uint8_t *campos)
{
    uint8_t bits = 0, paridad = 0;
    for (unsigned i = 0; i < 4; ++i) {
        bits = (uint8_t)((bits << 2) | pares[i]);
        paridad ^= pares[i];
    }
    *campos = bits;
    return paridad == pares[4];
}

bool PHY_Leer_Retorno(const uint8_t pares[], PhyRetorno *retorno)
{
    uint8_t campos;
    if (!Leer_Seccion(&pares[1], &campos) || (campos & 1U) != 0U) {
        return false;
    }
    const uint8_t siguiente = (campos >> 4) & 7U;
    if (siguiente >= PHY_ID_MODULO) {
        return false;
    }
    *retorno = (PhyRetorno){(campos & 128U) != 0U, siguiente, (campos >> 1) & 7U};
    return true;
}

bool PHY_Leer_Cabecera(const uint8_t pares[], PhyCabecera *cabecera)
{
    uint8_t campos;
    if (!Leer_Seccion(&pares[6], &campos)) {
        return false;
    }
    const uint8_t tipo = campos >> 6;
    const uint8_t id = (campos >> 3) & 7U;
    const uint8_t mapa = campos & 7U;
    if (tipo > PHY_REPITE || id >= PHY_ID_MODULO
        || (tipo == PHY_IDLE && (id != 0U || mapa != 0U))
        || (tipo != PHY_IDLE && mapa == 0U)) {
        return false;
    }
    *cabecera = (PhyCabecera){(PhyTipo)tipo, id, mapa};
    return true;
}

static uint8_t Primera_Linea(uint8_t mapa)
{
    uint8_t bit = 4;
    while ((mapa & bit) == 0U) {
        bit >>= 1;
    }
    return bit;
}

void PHY_Codificar_Bloque(PhyBloque *bloque, PhyRetorno retorno, PhyCabecera cabecera, uint16_t valor)
{
    const uint8_t mapa = cabecera.mapa;
    bloque->ciclos = (mapa == 0U || mapa == PHY_MAPA_COMPLETO) ? PHY_CICLOS_CORTOS : PHY_CICLOS_LARGOS;
    bloque->pares[0] = PHY_MARCADOR;
    Codificar_Seccion(&bloque->pares[1], (uint8_t)((retorno.pedir ? 128U : 0U) | (retorno.siguiente << 4) | (retorno.mapa << 1)));
    Codificar_Seccion(&bloque->pares[6], (uint8_t)((cabecera.tipo << 6) | (cabecera.id << 3) | mapa));

    bloque->simbolos[0] = 0;
    bloque->simbolos[1] = PHY_MAPA_COMPLETO;
    uint8_t xor_lineas = 0;
    if (mapa == PHY_MAPA_COMPLETO) {
        for (unsigned i = 0; i < 6; ++i) {
            const uint8_t par = (valor >> (10 - 2 * i)) & 3U;
            const uint8_t paridad = 1U ^ (par >> 1) ^ (par & 1U);
            bloque->simbolos[2 + i] = (uint8_t)((par << 1) | paridad);
            xor_lineas ^= bloque->simbolos[2 + i];
        }
        bloque->simbolos[8] = xor_lineas;
        bloque->simbolos[9] = 0;
        bloque->simbolos[10] = PHY_MAPA_COMPLETO;
    } else if (mapa != 0U) {
        const uint8_t primera = Primera_Linea(mapa);
        const uint8_t segunda = mapa ^ primera;
        for (unsigned i = 0; i < 12; ++i) {
            bloque->simbolos[2 + i] = (valor & (1U << (11 - i))) ? primera : segunda;
            xor_lineas ^= bloque->simbolos[2 + i];
        }
        bloque->simbolos[14] = xor_lineas;
        const uint8_t crc = PHY_CRC_Bloque(valor);
        for (unsigned i = 0; i < 4; ++i) {
            bloque->pares[11 + i] = (crc >> (6 - 2 * i)) & 3U;
        }
    } else {
        for (unsigned i = 2; i <= 9; ++i) {
            bloque->simbolos[i] = 0;
        }
        bloque->simbolos[10] = PHY_MAPA_COMPLETO;
    }
}

PhyResultado PHY_Decodificar_Bloque(const PhyBloque *bloque, uint8_t mapa, uint16_t *valor)
{
    if (mapa == 0U || mapa > PHY_MAPA_COMPLETO) {
        return PHY_ERR_CABECERA;
    }
    const uint8_t *s = bloque->simbolos;
    if ((PHY_Mapa_Pilotos(s[0], s[1]) & mapa) != mapa) {
        return PHY_ERR_PILOTOS;
    }
    uint8_t xor_lineas = 0;
    uint16_t bits = 0;
    if (mapa == PHY_MAPA_COMPLETO) {
        if (bloque->ciclos != PHY_CICLOS_CORTOS) {
            return PHY_ERR_CABECERA;
        }
        if (PHY_Mapa_Pilotos(s[9], s[10]) != mapa) {
            return PHY_ERR_PILOTOS;
        }
        for (unsigned i = 2; i <= 7; ++i) {
            if (((s[i] ^ (s[i] >> 1) ^ (s[i] >> 2)) & 1U) != 1U) {
                return PHY_ERR_PARIDAD;
            }
            bits = (uint16_t)((bits << 2) | (s[i] >> 1));
            xor_lineas ^= s[i];
        }
        if (xor_lineas != s[8]) {
            return PHY_ERR_XOR;
        }
    } else {
        if (bloque->ciclos != PHY_CICLOS_LARGOS) {
            return PHY_ERR_CABECERA;
        }
        const uint8_t primera = Primera_Linea(mapa);
        const uint8_t segunda = mapa ^ primera;
        for (unsigned i = 2; i <= 13; ++i) {
            const uint8_t simbolo = s[i] & mapa;
            if (segunda && simbolo != primera && simbolo != segunda) {
                return PHY_ERR_PARIDAD;
            }
            bits = (uint16_t)((bits << 1) | ((simbolo & primera) != 0U));
            xor_lineas ^= simbolo;
        }
        if (xor_lineas != (s[14] & mapa)) {
            return PHY_ERR_XOR;
        }
        const uint8_t *p = bloque->pares;
        const uint8_t crc = (uint8_t)((p[11] << 6) | (p[12] << 4) | (p[13] << 2) | p[14]);
        if (crc != PHY_CRC_Bloque(bits)) {
            return PHY_ERR_CRC_BLOQUE;
        }
    }
    *valor = bits;
    return PHY_OK;
}

void PHY_Codificar_Trama(PhyTrama trama, uint16_t bloques[3])
{
    const uint8_t bytes[2] = {trama.control, trama.dato};
    const uint8_t crc = PHY_CRC8(trama.control, bytes, sizeof bytes);
    bloques[0] = (uint16_t)(PHY_SYN_SFD | (trama.control << 2) | (trama.inicio ? 2U : 0U));
    bloques[1] = (uint16_t)((trama.dato << 4) | (crc >> 4));
    bloques[2] = (uint16_t)(((crc & 15U) << 8) | (trama.ack << 5) | (trama.seq << 2) | (trama.fin ? 2U : 0U));
}

PhyResultado PHY_Decodificar_Trama(const uint16_t bloques[3], PhyTrama *trama)
{
    trama->control = (bloques[0] >> 2) & 15U;
    trama->dato = (uint8_t)(bloques[1] >> 4);
    trama->inicio = (bloques[0] & 2U) != 0U;
    trama->ack = (bloques[2] >> 5) & 7U;
    trama->seq = (bloques[2] >> 2) & 7U;
    trama->fin = (bloques[2] & 2U) != 0U;
    trama->crc_recibido = (uint8_t)(((bloques[1] & 15U) << 4) | (bloques[2] >> 8));
    const uint8_t bytes[2] = {trama->control, trama->dato};
    trama->crc_calculado = PHY_CRC8(trama->control, bytes, sizeof bytes);
    if ((bloques[0] & (PHY_MASCARA_SYN_SFD | 1U)) != PHY_SYN_SFD || (bloques[2] & 1U) != 0U) {
        return PHY_ERR_ESTRUCTURA;
    }
    if (trama->crc_calculado != trama->crc_recibido) {
        return PHY_ERR_CRC_TRAMA;
    }
    if ((trama->control & 1U) != 0U && (trama->dato != trama->ack || trama->inicio || trama->fin || trama->seq != 0U)) {
        return PHY_ERR_ESTRUCTURA;
    }
    return PHY_OK;
}
