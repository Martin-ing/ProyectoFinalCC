#include "phy_rx.h"
#include "phy_tx.h"

#define PHY_TIPO_REPITE 2U
#define PHY_MAPA_TRES_LINEAS 7U
#define PHY_SIMBOLO_MARCADOR 4
#define PHY_TOLERANCIA_ANCHO_MS 25U
#define PHY_TOLERANCIA_PERIODO_MS 50U

volatile bool phy_rx_reloj_adquirido = false;
volatile bool phy_rx_pilotos_medidos = false;
volatile uint8_t phy_rx_mapa = 0U;
volatile uint8_t phy_rx_ultimo_simbolo = 0U;
volatile uint8_t phy_rx_ultimo_id = 0U;
volatile uint8_t phy_rx_ultimo_dato = 0U;
volatile uint8_t phy_rx_ultimo_seq = 0U;
volatile uint8_t phy_rx_crc_recibido = 0U;
volatile uint8_t phy_rx_crc_calculado = 0U;
volatile uint32_t phy_rx_bloques_ok = 0U;
volatile uint32_t phy_rx_tramas_ok = 0U;
volatile uint32_t phy_rx_sync_ok = 0U;
volatile uint32_t phy_rx_mensajes_ok = 0U;
volatile PHY_RxErrores phy_rx_errores;

volatile char phy_rx_mensaje[PHY_RX_MAX_MENSAJE + 1U];
volatile uint8_t phy_rx_longitud = 0U;
volatile uint8_t phy_rx_control = 0U;
volatile uint8_t phy_rx_bytes_en_curso = 0U;

static uint8_t ciclos_recibidos = PHY_CICLOS_BLOQUE;
static uint8_t pares[PHY_CICLOS_BLOQUE];
static uint8_t simbolos[PHY_CICLOS_BLOQUE];
static uint8_t mapa_inicial = 0U;
static uint16_t bloques[PHY_BLOQUES_DATOS];
static uint8_t bloques_recibidos = 0U;
static uint8_t trama_primer_id = 0U;
static uint8_t secuencia_esperada = 0U;
static bool secuencia_conocida = false;
static bool inicio_mensaje_conocido = false;
static uint8_t mensaje_control = 0U;
static uint8_t mensaje_parcial[PHY_RX_MAX_MENSAJE];

static void Contar_Error(volatile uint32_t *contador)
{
    ++*contador;
    ++phy_rx_errores.total;
}

static void Descartar_Parcial(void)
{
    bloques_recibidos = 0U;
    phy_rx_bytes_en_curso = 0U;
    inicio_mensaje_conocido = false;
    secuencia_conocida = false;
}

void PHY_RX_Perder_Reloj(void)
{
    Contar_Error(&phy_rx_errores.reloj);
    ciclos_recibidos = PHY_CICLOS_BLOQUE;
    phy_rx_reloj_adquirido = false;
    Descartar_Parcial();
}

static int Par_Del_Reloj(uint32_t ancho_ms)
{
    for (int par = 0; par <= PHY_SIMBOLO_MARCADOR; ++par) {
        const uint32_t nominal = 400U + 100U * (uint32_t)par;
        if (ancho_ms >= nominal - PHY_TOLERANCIA_ANCHO_MS
            && ancho_ms <= nominal + PHY_TOLERANCIA_ANCHO_MS) {
            return par;
        }
    }
    return -1;
}

static bool Leer_Seccion(uint8_t inicio, uint8_t *campos)
{
    uint8_t bits = 0U;
    uint8_t paridad = 0U;
    for (uint32_t i = inicio; i < inicio + 4U; ++i) {
        bits = (uint8_t)((bits << 2U) | pares[i]);
        paridad ^= pares[i];
    }
    *campos = bits;
    return paridad == pares[inicio + 4U];
}

static uint8_t Medir_Pilotos(uint8_t bajo, uint8_t alto)
{
    const uint8_t mapa = (uint8_t)((~bajo & alto) & PHY_MAPA_TRES_LINEAS);
    phy_rx_mapa = mapa;
    phy_rx_pilotos_medidos = true;
    return mapa;
}

static void Guardar_Byte(uint8_t control, uint8_t dato, uint8_t seq, bool fin)
{
    if (secuencia_conocida && seq != secuencia_esperada) {
        const uint8_t ultima = (secuencia_esperada + PHY_SEQ_MODULO - 1U) % PHY_SEQ_MODULO;
        if (seq == ultima) {
            return;  /* Copia de la ultima trama. */
        }
        Contar_Error(&phy_rx_errores.secuencia);
        Descartar_Parcial();
    }
    secuencia_esperada = (seq + 1U) % PHY_SEQ_MODULO;
    secuencia_conocida = true;
    phy_rx_ultimo_dato = dato;
    phy_rx_ultimo_seq = seq;
    ++phy_rx_tramas_ok;

    /* Un FIN valido marca donde puede comenzar el siguiente mensaje completo. */
    if (!inicio_mensaje_conocido) {
        inicio_mensaje_conocido = fin;
        return;
    }

    const uint8_t indice = phy_rx_bytes_en_curso;
    if (indice == 0U) {
        mensaje_control = control;
    } else if (indice >= PHY_RX_MAX_MENSAJE
               || ((control ^ mensaje_control) & PHY_CONTROL_UTF8) != 0U) {
        Contar_Error(&phy_rx_errores.trama);
        Descartar_Parcial();
        inicio_mensaje_conocido = fin;
        return;
    }

    mensaje_parcial[indice] = dato;
    phy_rx_bytes_en_curso = indice + 1U;
    if (fin) {
        for (uint32_t i = 0U; i <= indice; ++i) {
            phy_rx_mensaje[i] = (char)mensaje_parcial[i];
        }
        phy_rx_mensaje[indice + 1U] = '\0';
        phy_rx_longitud = indice + 1U;
        phy_rx_control = mensaje_control;
        ++phy_rx_mensajes_ok;
        phy_rx_bytes_en_curso = 0U;
    }
}

static void Validar_Trama(void)
{
    const uint8_t control = (bloques[0] >> 2U) & 15U;
    const uint8_t dato = bloques[1] >> 4U;
    const uint16_t cola = bloques[2];
    const uint8_t crc = (uint8_t)(((bloques[1] & 15U) << 4U) | (cola >> 8U));
    const uint8_t secuencia = (cola >> 2U) & 7U;
    const bool fin = (cola & 2U) != 0U;
    const uint8_t entrada_crc[2] = {control, dato};

    phy_rx_crc_recibido = crc;
    phy_rx_crc_calculado = PHY_CRC8(control, entrada_crc, 2U);
    if (phy_rx_crc_calculado != crc) {
        Contar_Error(&phy_rx_errores.crc);
        Descartar_Parcial();
    } else if ((cola & 1U) != 0U) {
        Contar_Error(&phy_rx_errores.trama);
        Descartar_Parcial();
    } else if ((control & 1U) == 0U) {
        Guardar_Byte(control, dato, secuencia, fin);
    }
}

static void Aceptar_Bloque(uint8_t id, uint16_t valor)
{
    phy_rx_ultimo_id = id;
    ++phy_rx_bloques_ok;

    /* Los IDs parten de 0 y se asignan a grupos de tres. B1 queda en 0 o 3. */
    const uint8_t parte = id % PHY_BLOQUES_DATOS;
    const bool misma_trama = id - parte == trama_primer_id;
    if (misma_trama && parte < bloques_recibidos && bloques[parte] == valor) {
        return;
    }

    if (parte == 0U) {
        if (bloques_recibidos != 0U) {
            Contar_Error(&phy_rx_errores.secuencia);
            Descartar_Parcial();
        }
        if ((valor & 0xFC3U) != 0x3C0U) {
            Contar_Error(&phy_rx_errores.trama);
            Descartar_Parcial();
            return;
        }
        trama_primer_id = id;
    } else if (bloques_recibidos == 0U) {
        return;
    } else if (!misma_trama || parte != bloques_recibidos) {
        Contar_Error(&phy_rx_errores.secuencia);
        Descartar_Parcial();
        return;
    }

    bloques[bloques_recibidos++] = valor;
    if (bloques_recibidos == PHY_BLOQUES_DATOS) {
        bloques_recibidos = 0U;
        Validar_Trama();
    }
}

static void Validar_Bloque(void)
{
    uint8_t recepcion;
    if (!Leer_Seccion(1U, &recepcion) || (recepcion & 1U) != 0U
        || ((recepcion >> 4U) & 7U) >= PHY_ID_MODULO) {
        Contar_Error(&phy_rx_errores.cabecera);
    }

    uint8_t transmision;
    const bool cabecera_valida = Leer_Seccion(6U, &transmision);
    const uint8_t tipo = transmision >> 6U;
    const uint8_t id = (transmision >> 3U) & 7U;
    const uint8_t mapa = transmision & 7U;
    const bool es_control = tipo == PHY_TIPO_SYNC || tipo == PHY_TIPO_IDLE;
    if (!cabecera_valida || id >= PHY_ID_MODULO
        || (es_control && (id != 0U || mapa != 0U))
        || (!es_control && mapa != PHY_MAPA_TRES_LINEAS)) {
        Contar_Error(&phy_rx_errores.cabecera);
        Descartar_Parcial();
        return;
    }

    const uint8_t mapa_final = Medir_Pilotos(simbolos[9], simbolos[10]);
    const bool pilotos_validos = (mapa_inicial & mapa_final) == PHY_MAPA_TRES_LINEAS;
    if (!pilotos_validos) {
        Contar_Error(&phy_rx_errores.pilotos);
    }

    if (es_control) {
        if (tipo == PHY_TIPO_SYNC) {
            ++phy_rx_sync_ok;
            Descartar_Parcial();
            inicio_mensaje_conocido = true;
        }
        return;
    }
    if (!pilotos_validos) {
        Descartar_Parcial();
        return;
    }

    uint16_t valor = 0U;
    uint8_t xor_lineas = 0U;
    for (uint32_t i = 2U; i <= 7U; ++i) {
        const uint8_t simbolo = simbolos[i];
        if (((simbolo ^ (simbolo >> 1U) ^ (simbolo >> 2U)) & 1U) == 0U) {
            Contar_Error(&phy_rx_errores.simbolos);
            Descartar_Parcial();
            return;
        }
        valor = (uint16_t)((valor << 2U) | (simbolo >> 1U));
        xor_lineas ^= simbolo;
    }
    if (xor_lineas != simbolos[8]) {
        Contar_Error(&phy_rx_errores.simbolos);
        Descartar_Parcial();
        return;
    }
    Aceptar_Bloque(id, valor);
}

void PHY_RX_Procesar_Pulso(uint32_t periodo_ms, uint32_t ancho_ms, uint8_t simbolo)
{
    const uint8_t muestra = simbolo & PHY_MAPA_TRES_LINEAS;
    phy_rx_ultimo_simbolo = muestra;
    const int par = Par_Del_Reloj(ancho_ms);
    if (par < 0 || (periodo_ms != 0U
        && (periodo_ms < PHY_PERIODO_MS - PHY_TOLERANCIA_PERIODO_MS
            || periodo_ms > PHY_PERIODO_MS + PHY_TOLERANCIA_PERIODO_MS))) {
        PHY_RX_Perder_Reloj();
        if (par != PHY_SIMBOLO_MARCADOR) {
            return;
        }
    }

    if (par == PHY_SIMBOLO_MARCADOR) {
        if (ciclos_recibidos > 6U && ciclos_recibidos < PHY_CICLOS_BLOQUE
            && (pares[6] == PHY_TIPO_NUEVO || pares[6] == PHY_TIPO_REPITE)) {
            Contar_Error(&phy_rx_errores.trama);
            Descartar_Parcial();
        }
        simbolos[0] = muestra;
        ciclos_recibidos = 1U;
        phy_rx_reloj_adquirido = true;
    } else if (ciclos_recibidos < PHY_CICLOS_BLOQUE) {
        pares[ciclos_recibidos] = (uint8_t)par;
        simbolos[ciclos_recibidos] = muestra;
        if (ciclos_recibidos == 1U) {
            mapa_inicial = Medir_Pilotos(simbolos[0], simbolos[1]);
        }
        if (++ciclos_recibidos == PHY_CICLOS_BLOQUE) {
            Validar_Bloque();
        }
    } else if (phy_rx_reloj_adquirido) {
        PHY_RX_Perder_Reloj();  /* El ciclo siguiente debia ser un marcador. */
    }
}
