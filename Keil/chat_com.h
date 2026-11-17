#ifndef CHAT_COM_H
#define CHAT_COM_H

#include "phy_link.h"

enum { CHAT_BAUD = 9600, CHAT_VERSION = 2, CHAT_ERR_FORMATO = 9, CHAT_ERR_OCUPADO = 10 };
enum { CHAT_MAX_LINEA = 2 * PHY_MAX_MENSAJE + 18 };

typedef enum { CHAT_NADA, CHAT_ABRIR, CHAT_ESTADO, CHAT_CERRAR, CHAT_ENVIAR, CHAT_INVALIDO } ChatComAccion;

typedef struct {
    uint32_t ticket;
    uint8_t control, longitud, bytes[PHY_MAX_MENSAJE];
} ChatComDatos;

typedef struct {
    char linea[CHAT_MAX_LINEA + 1];
    uint16_t usados;
    bool en_linea, excedido;
} ChatComParser;

void ChatCom_Reiniciar(ChatComParser *parser);
ChatComAccion ChatCom_Recibir(ChatComParser *parser, uint8_t byte, ChatComDatos *datos);

#endif
