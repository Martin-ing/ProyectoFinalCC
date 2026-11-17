#include "chat_com.h"

#include <string.h>

static bool Leer_Decimal(const char **linea, uint32_t *numero)
{
    const char *p = *linea;
    uint32_t valor = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    do {
        const uint32_t digito = (uint32_t)(*p++ - '0');
        if (valor > (UINT32_MAX - digito) / 10U) {
            return false;
        }
        valor = valor * 10U + digito;
    } while (*p >= '0' && *p <= '9');
    if (*p != ',') {
        return false;
    }
    *linea = p + 1;
    *numero = valor;
    return true;
}

static int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static ChatComAccion Leer_Linea(const char *linea, ChatComDatos *datos)
{
    if (strcmp(linea, "H") == 0) return CHAT_ABRIR;
    if (strcmp(linea, "I") == 0) return CHAT_ESTADO;
    if (strcmp(linea, "C") == 0) return CHAT_CERRAR;
    if (strncmp(linea, "S,", 2) != 0) return CHAT_INVALIDO;

    const char *p = linea + 2;
    uint32_t control;
    if (!Leer_Decimal(&p, &datos->ticket) || datos->ticket == 0U
        || !Leer_Decimal(&p, &control) || control > 15U || (control & 1U)) {
        return CHAT_INVALIDO;
    }
    const size_t hex = strlen(p);
    if (hex == 0U || hex > 2U * PHY_MAX_MENSAJE || (hex & 1U)) {
        return CHAT_INVALIDO;
    }
    datos->longitud = (uint8_t)(hex / 2U);
    datos->control = (uint8_t)control;
    for (size_t i = 0; i < datos->longitud; ++i) {
        const int a = Hex(p[2U * i]);
        const int b = Hex(p[2U * i + 1U]);
        if (a < 0 || b < 0) {
            return CHAT_INVALIDO;
        }
        datos->bytes[i] = (uint8_t)((a << 4) | b);
    }
    return CHAT_ENVIAR;
}

void ChatCom_Reiniciar(ChatComParser *p)
{
    p->usados = 0;
    p->en_linea = false;
    p->excedido = false;
}

ChatComAccion ChatCom_Recibir(ChatComParser *p, uint8_t byte, ChatComDatos *datos)
{
    if (byte == '@') {
        ChatCom_Reiniciar(p);
        p->en_linea = true;
        return CHAT_NADA;
    }
    if (!p->en_linea || byte == '\r') {
        return CHAT_NADA;
    }
    if (byte != '\n') {
        if (p->usados == CHAT_MAX_LINEA || byte < 32U || byte > 126U) {
            p->excedido = true;
        } else if (!p->excedido) {
            p->linea[p->usados++] = (char)byte;
        }
        return CHAT_NADA;
    }
    p->linea[p->usados] = '\0';
    datos->ticket = 0;
    const ChatComAccion accion = p->excedido ? CHAT_INVALIDO : Leer_Linea(p->linea, datos);
    ChatCom_Reiniciar(p);
    return accion;
}
