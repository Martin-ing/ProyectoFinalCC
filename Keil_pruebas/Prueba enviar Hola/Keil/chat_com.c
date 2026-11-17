#include "chat_com.h"

static int Hex_Valor(uint8_t digito)
{
    if (digito >= '0' && digito <= '9') {
        return digito - '0';
    }
    if (digito >= 'A' && digito <= 'F') {
        return digito - 'A' + 10;
    }
    return -1;
}

static bool UTF8_Caracter_Valido(const uint8_t *bytes, uint8_t longitud)
{
    static const uint8_t prefijos[] = {0x00U, 0xC0U, 0xE0U, 0xF0U};
    static const uint8_t mascaras[] = {0x80U, 0xE0U, 0xF0U, 0xF8U};
    static const uint32_t minimos[] = {0U, 0x80U, 0x800U, 0x10000U};
    const uint8_t indice = longitud - 1U;
    const uint8_t mascara = mascaras[indice];
    if ((bytes[0] & mascara) != prefijos[indice]) {
        return false;
    }

    uint32_t codigo = bytes[0] & (uint8_t)~mascara;
    for (uint32_t i = 1U; i < longitud; ++i) {
        if ((bytes[i] & 0xC0U) != 0x80U) {
            return false;
        }
        codigo = (codigo << 6U) | (bytes[i] & 0x3FU);
    }
    return codigo >= minimos[indice] && codigo <= 0x10FFFFU
        && (codigo < 0xD800U || codigo > 0xDFFFU);
}

static ChatComTipo ChatCOM_Interpretar(const ChatComParser *parser, ChatComDatos *datos)
{
    const uint8_t *cuerpo = parser->cuerpo;
    const uint8_t longitud = parser->longitud;
    if (longitud == 1U && cuerpo[0] == 'A') {
        return CHAT_COM_ABRIR;
    }
    if (longitud == 1U && cuerpo[0] == 'R') {
        return CHAT_COM_ESCUCHAR;
    }
    if (longitud == 1U && cuerpo[0] == 'C') {
        return CHAT_COM_CERRAR;
    }
    if (longitud < 4U || (longitud & 1U) != 0U || cuerpo[0] != 'D') {
        return CHAT_COM_INVALIDO;
    }
    const int control = Hex_Valor(cuerpo[1]);
    if (control < 0 || (control & 1) != 0) {
        return CHAT_COM_INVALIDO;
    }

    datos->control = (uint8_t)control;
    datos->longitud = (longitud - 2U) / 2U;
    for (uint32_t i = 0U; i < datos->longitud; ++i) {
        const int alto = Hex_Valor(cuerpo[2U + 2U * i]);
        const int bajo = Hex_Valor(cuerpo[3U + 2U * i]);
        if (alto < 0 || bajo < 0) {
            return CHAT_COM_INVALIDO;
        }
        datos->bytes[i] = (uint8_t)((alto << 4) | bajo);
    }
    const bool valido = (datos->control & PHY_CONTROL_UTF8)
                      ? UTF8_Caracter_Valido(datos->bytes, datos->longitud)
                      : datos->longitud == 1U;
    return valido ? CHAT_COM_DATOS : CHAT_COM_INVALIDO;
}

ChatComTipo ChatCOM_Leer(ChatComParser *parser, uint8_t byte, ChatComDatos *datos)
{
    /* El siguiente '@' descarta un paquete parcial, incluso tras reconectar. */
    if (byte == '@') {
        parser->longitud = 0U;
        parser->recibiendo = true;
    } else if (parser->recibiendo && byte == '\n') {
        parser->recibiendo = false;
        return ChatCOM_Interpretar(parser, datos);
    } else if (parser->recibiendo && byte != '\r') {
        if (parser->longitud == sizeof parser->cuerpo) {
            parser->recibiendo = false;
            return CHAT_COM_INVALIDO;
        }
        parser->cuerpo[parser->longitud++] = byte;
    }
    return CHAT_COM_NADA;
}
