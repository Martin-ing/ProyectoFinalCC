#ifndef CHAT_COM_H
#define CHAT_COM_H

#include <stdbool.h>
#include "phy_tx.h"

#define CHAT_COM_MAX_CUERPO (2U + 2U * PHY_TX_MAX_BYTES)

typedef enum {
    CHAT_COM_NADA,
    CHAT_COM_ABRIR,
    CHAT_COM_ESCUCHAR,
    CHAT_COM_CERRAR,
    CHAT_COM_DATOS,
    CHAT_COM_INVALIDO
} ChatComTipo;

typedef struct {
    uint8_t control;
    uint8_t longitud;
    uint8_t bytes[PHY_TX_MAX_BYTES];
} ChatComDatos;

typedef struct {
    bool recibiendo;
    uint8_t longitud;
    uint8_t cuerpo[CHAT_COM_MAX_CUERPO];
} ChatComParser;

ChatComTipo ChatCOM_Leer(ChatComParser *parser, uint8_t byte, ChatComDatos *datos);

#endif
