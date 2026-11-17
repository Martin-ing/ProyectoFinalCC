#ifndef PHY_LINK_H
#define PHY_LINK_H

#include "phy_codec.h"

enum { PHY_MAX_MENSAJE = 64, PHY_VENTANA = 2, PHY_COPIAS = 3, PHY_SIN_ID = PHY_ID_MODULO };

typedef enum {
    PHY_EV_BLOQUE_TX, PHY_EV_BLOQUE_RX, PHY_EV_RETORNO, PHY_EV_RETORNO_INVALIDO,
    PHY_EV_PEDIR, PHY_EV_CORTE, PHY_EV_ERROR_BLOQUE, PHY_EV_MAPA_RX, PHY_EV_MAPA_TX,
    PHY_EV_TRAMA_TX, PHY_EV_TRAMA_RX, PHY_EV_REENVIO_TRAMA, PHY_EV_ACK_TRAMA,
    PHY_EV_ENTREGADO, PHY_EV_ACK_PHY = 15,
    PHY_EV_FIN_BLOQUE_TX, PHY_EV_CABECERA_RX, PHY_EV_PULSO_INVALIDO
} PhyTipoEvento;

typedef enum {
    PHY_SIN_MOTIVO, PHY_PEDIDO, PHY_INTERRUMPIDO, PHY_PLAZO_BLOQUE, PHY_NACK,
    PHY_PLAZO_TRAMA, PHY_RELOJ = 7, PHY_MARCADOR_ANTICIPADO
} PhyMotivo;

typedef enum { PHY_ENVIO_NINGUNO, PHY_ENVIO_ENCOLADO, PHY_ENVIO_ENTREGADO } PhyEstadoEnvio;
typedef enum { PHY_RX_EN_ORDEN, PHY_RX_RETENIDO, PHY_RX_FUERA_DE_VENTANA } PhyOrdenRx;

typedef struct {
    uint32_t ms, ticket, ancho_ms, periodo_ms;
    PhyTipoEvento tipo;
    PhyCabecera bloque;
    PhyRetorno retorno;
    PhyTrama trama;
    PhyResultado resultado;
    PhyMotivo motivo;
    uint16_t valor;
    uint16_t bits_retorno, bits_cabecera;
    uint8_t ciclo;
    PhyOrdenRx orden;
} PhyEvento;

typedef struct {
    void (*evento)(void *contexto, const PhyEvento *evento);
    void (*mensaje)(void *contexto, uint8_t control, const uint8_t bytes[], uint8_t longitud);
    void *contexto;
} PhyReceptor;

typedef struct {
    bool ocupado, terminado, esperando, pedido_atendido;
    uint8_t copia;
    uint16_t valor;
    uint32_t terminado_ms, reenvios;
    PhyMotivo motivo;
} PhyPendiente;

typedef struct {
    bool ocupado;
    uint8_t asignados, enviados, confirmados;
    uint16_t bloques[3];
    uint32_t intento;
} PhyCopia;

typedef struct {
    bool existe, listo, transmitiendo, enviado, esperando;
    uint32_t intento, confirmado_phy_ms;
} PhyDatoTx;

typedef struct {
    uint16_t ancho_ms;
    uint8_t simbolo, ciclo;
} PhyPulso;

typedef struct {
    PhyReceptor salida;
    PhyBloque plan_tx, plan_rx;
    PhyCabecera cabecera_tx, cabecera_rx;
    PhyRetorno retorno_rx;
    PhyPendiente ventana[PHY_ID_MODULO];
    PhyCopia copias[PHY_COPIAS];
    PhyDatoTx dato_tx;
    uint8_t ciclo_tx, primero_tx, siguiente_tx, cantidad_tx, copia_tx, mapa_tx;
    bool termina_en_bajada;
    uint32_t intento_tx;
    bool reloj_rx, retorno_valido, bloque_rx, cabecera_rx_valida;
    uint32_t ultimo_pulso_ms;
    uint8_t mapa_rx, siguiente_rx, ids_fallidos;
    bool retenido;
    uint16_t valor_retenido;
    uint8_t cantidad_rx;
    uint16_t bloques_rx[3];
    bool ack_pendiente, nack_pendiente;
    uint8_t seq_tx, seq_rx;
    uint8_t bytes_tx[PHY_MAX_MENSAJE], longitud_tx, posicion_tx, control_tx;
    uint32_t ticket_tx;
    PhyEstadoEnvio estado_envio;
    uint8_t bytes_rx[PHY_MAX_MENSAJE], longitud_rx, control_rx;
    uint32_t bloques_rx_ok, tramas_rx_ok, errores, reenvios_phy, reenvios_trama;
} PhyEnlace;

/* Estado compartido: invocar en orden cronológico sin reentrada. */
void PHY_Iniciar(PhyEnlace *enlace, PhyReceptor salida);
bool PHY_Encolar(PhyEnlace *enlace, uint32_t ticket, uint8_t control, const uint8_t bytes[], uint8_t longitud);
PhyPulso PHY_TX_Subida(PhyEnlace *enlace, uint32_t ms);
void PHY_TX_Bajada(PhyEnlace *enlace, uint32_t ms);
void PHY_RX_Pulso(PhyEnlace *enlace, uint32_t ms, uint32_t periodo_ms, uint32_t ancho_ms, uint8_t simbolo);
void PHY_RX_Perder_Reloj(PhyEnlace *enlace, uint32_t ms);
uint8_t PHY_Mapa_TX(const PhyEnlace *enlace);

#endif
