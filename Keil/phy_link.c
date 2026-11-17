#include "phy_link.h"

#include <string.h>

enum {
    SIN_COPIA = PHY_COPIAS,
    MAX_REENVIOS = 3,
    PLAZO_PHY_MS = 25000,
    PLAZO_TRAMA_MS = 180000,
    PLAZO_MAPA_MS = 60000,
    PLAZO_RELOJ_MS = 1600
};

static void Registrar(PhyEnlace *e, uint32_t ms, PhyEvento evento)
{
    evento.ms = ms;
    if (e->salida.evento != NULL) {
        e->salida.evento(e->salida.contexto, &evento);
    }
}

static uint8_t Siguiente_Id(uint8_t id)
{
    return (uint8_t)((id + 1U) % PHY_ID_MODULO);
}

static uint8_t Siguiente_Seq(uint8_t seq)
{
    return (uint8_t)((seq + 1U) % PHY_SEQ_MODULO);
}

static bool Debe_Pedir(const PhyEnlace *e)
{
    return e->retenido || (e->ids_fallidos & (1U << e->siguiente_rx)) != 0U;
}

uint8_t PHY_Mapa_TX(const PhyEnlace *e)
{
    return e->reloj_rx && e->retorno_valido ? e->mapa_tx : 0U;
}

static void Fallar(PhyEnlace *e, uint32_t ms, PhyMotivo motivo)
{
    e->falla = motivo;
    uint32_t ticket = 0;
    if (e->estado_envio == PHY_ENVIO_ENCOLADO) {
        e->estado_envio = PHY_ENVIO_FALLIDO;
        ticket = e->ticket_tx;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_FALLA, .ticket = ticket, .motivo = motivo});
}

void PHY_Iniciar(PhyEnlace *e, PhyReceptor salida)
{
    memset(e, 0, sizeof *e);
    e->salida = salida;
    e->copia_tx = SIN_COPIA;
}

bool PHY_Encolar(PhyEnlace *e, uint32_t ticket, uint8_t control, const uint8_t bytes[], uint8_t longitud)
{
    if (ticket == 0U || e->longitud_tx != 0U || e->falla != PHY_SIN_MOTIVO
        || bytes == NULL || longitud == 0U || longitud > PHY_MAX_MENSAJE || (control & 0xf1U) != 0U) {
        return false;
    }
    memcpy(e->bytes_tx, bytes, longitud);
    e->longitud_tx = longitud;
    e->posicion_tx = 0;
    e->control_tx = control;
    e->ticket_tx = ticket;
    e->estado_envio = PHY_ENVIO_ENCOLADO;
    return true;
}

static void Reenviar_Trama(PhyEnlace *e, uint32_t ms, PhyMotivo motivo)
{
    PhyDatoTx *dato = &e->dato_tx;
    if (!dato->existe || !dato->enviado || dato->transmitiendo || dato->listo || e->falla) {
        return;
    }
    if (dato->reenvios == MAX_REENVIOS) {
        Fallar(e, ms, motivo);
        return;
    }
    ++dato->reenvios;
    ++e->reenvios_trama;
    dato->listo = true;
    dato->esperando = false;
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_REENVIO_TRAMA, .ticket = e->ticket_tx,
                                 .trama.seq = e->seq_tx, .motivo = motivo});
}

static void Confirmar_Trama(PhyEnlace *e, uint32_t ms, uint8_t ack)
{
    if (!e->dato_tx.existe || !e->dato_tx.enviado || Siguiente_Seq(e->seq_tx) != ack || e->falla) {
        return;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_ACK_TRAMA, .ticket = e->ticket_tx,
                                 .trama = {.seq = e->seq_tx, .ack = ack}});
    /* Una confirmación de enlace no retira IDs de la ventana PHY. */
    e->seq_tx = ack;
    e->dato_tx = (PhyDatoTx){0};
    if (++e->posicion_tx == e->longitud_tx) {
        e->longitud_tx = 0;
        e->estado_envio = PHY_ENVIO_ENTREGADO;
        Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_ENTREGADO, .ticket = e->ticket_tx});
    }
}

static void Procesar_Trama(PhyEnlace *e, uint32_t ms)
{
    PhyTrama trama;
    PhyResultado resultado = PHY_Decodificar_Trama(e->bloques_rx, &trama);
    const bool datos = (trama.control & 1U) == 0U;
    const bool esperado = datos && trama.seq == e->seq_rx;
    if (resultado == PHY_OK && esperado
        && (trama.inicio != (e->longitud_rx == 0U) || e->longitud_rx == PHY_MAX_MENSAJE
            || (!trama.inicio && ((trama.control ^ e->control_rx) & PHY_CONTROL_UTF8) != 0U))) {
        resultado = PHY_ERR_ESTRUCTURA;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_TRAMA_RX, .trama = trama, .resultado = resultado});
    if (resultado != PHY_OK) {
        ++e->errores;
        if (datos && (e->bloques_rx[0] & PHY_MASCARA_SYN_SFD) == PHY_SYN_SFD) {
            e->nack_pendiente = true;
        }
        return;
    }
    ++e->tramas_rx_ok;
    if (!datos) {
        if ((trama.control & PHY_CONTROL_UTF8) == 0U) {
            Confirmar_Trama(e, ms, trama.ack);
        } else if (trama.ack == e->seq_tx) {
            Reenviar_Trama(e, ms, PHY_NACK);
        }
        return;
    }

    Confirmar_Trama(e, ms, trama.ack);
    if (!esperado) {
        if (Siguiente_Seq(trama.seq) == e->seq_rx) {
            e->ack_pendiente = true;
        } else {
            e->nack_pendiente = true;
        }
        return;
    }
    if (trama.inicio) {
        e->control_rx = trama.control;
    }
    e->bytes_rx[e->longitud_rx++] = trama.dato;
    e->seq_rx = Siguiente_Seq(e->seq_rx);
    e->nack_pendiente = false;
    e->ack_pendiente = true;
    if (trama.fin) {
        if (e->salida.mensaje != NULL) {
            e->salida.mensaje(e->salida.contexto, e->control_rx, e->bytes_rx, e->longitud_rx);
        }
        e->longitud_rx = 0;
    }
}

static void Entregar_Bloque(PhyEnlace *e, uint32_t ms, uint16_t valor)
{
    ++e->bloques_rx_ok;
    e->ids_fallidos &= (uint8_t)~(1U << e->siguiente_rx);
    e->siguiente_rx = Siguiente_Id(e->siguiente_rx);
    e->bloques_rx[e->cantidad_rx++] = valor;
    if (e->cantidad_rx == 3U) {
        e->cantidad_rx = 0;
        Procesar_Trama(e, ms);
    }
}

static void Aceptar_Bloque(PhyEnlace *e, uint32_t ms, uint16_t valor)
{
    const uint8_t id = e->cabecera_rx.id;
    PhyOrdenRx orden = PHY_RX_FUERA_DE_VENTANA;
    if (id == e->siguiente_rx) {
        orden = PHY_RX_EN_ORDEN;
    } else if (id == Siguiente_Id(e->siguiente_rx) && !e->retenido) {
        orden = PHY_RX_RETENIDO;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_BLOQUE_RX, .bloque = e->cabecera_rx,
                                 .retorno = e->retorno_rx, .valor = valor, .orden = orden,
                                 .ciclo = e->plan_rx.ciclos - 1U});
    if (orden == PHY_RX_EN_ORDEN) {
        Entregar_Bloque(e, ms, valor);
        if (e->retenido) {
            e->retenido = false;
            Entregar_Bloque(e, ms, e->valor_retenido);
        }
    } else if (orden == PHY_RX_RETENIDO) {
        e->retenido = true;
        e->valor_retenido = valor;
        e->ids_fallidos &= (uint8_t)~(1U << id);
    }
}

static void Error_Bloque(PhyEnlace *e, uint32_t ms, PhyResultado resultado, PhyMotivo motivo)
{
    const PhyCabecera cabecera = e->cabecera_rx_valida ? e->cabecera_rx : (PhyCabecera){.id = PHY_SIN_ID};
    if (cabecera.tipo != PHY_IDLE && (cabecera.id == e->siguiente_rx
        || cabecera.id == Siguiente_Id(e->siguiente_rx))) {
        e->ids_fallidos |= (uint8_t)(1U << cabecera.id);
    }
    if (motivo != PHY_MARCADOR_ANTICIPADO) {
        ++e->errores;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_ERROR_BLOQUE, .resultado = resultado,
                                 .motivo = motivo, .bloque = cabecera,
                                 .ciclo = e->plan_rx.ciclos == 0U ? 0U : e->plan_rx.ciclos - 1U});
    e->bloque_rx = false;
    e->cabecera_rx_valida = false;
}

static void Cerrar_Bloque_Rx(PhyEnlace *e, uint32_t ms)
{
    uint16_t valor;
    const PhyResultado resultado = PHY_Decodificar_Bloque(&e->plan_rx, e->cabecera_rx.mapa, &valor);
    if (resultado == PHY_OK) {
        Aceptar_Bloque(e, ms, valor);
        e->bloque_rx = false;
        e->cabecera_rx_valida = false;
    } else {
        Error_Bloque(e, ms, resultado, PHY_SIN_MOTIVO);
    }
}

static void Medir_Mapa(PhyEnlace *e, uint32_t ms, uint8_t ciclo_bajo)
{
    const uint8_t mapa = PHY_Mapa_Pilotos(e->plan_rx.simbolos[ciclo_bajo], e->plan_rx.simbolos[ciclo_bajo + 1U]);
    if (mapa != e->mapa_rx) {
        e->mapa_rx = mapa;
        Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_MAPA_RX, .retorno.mapa = mapa, .ciclo = ciclo_bajo + 1U});
    }
}

static void Confirmar_Bloque(PhyEnlace *e, uint32_t ms, uint8_t id)
{
    PhyPendiente *pendiente = &e->ventana[id];
    PhyCopia *copia = &e->copias[pendiente->copia];
    pendiente->ocupado = false;
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_ACK_PHY, .bloque.id = id, .retorno = e->retorno_rx});
    if (++copia->confirmados == 3U) {
        copia->ocupado = false;
        PhyDatoTx *dato = &e->dato_tx;
        if (dato->existe && copia->intento == dato->intento && !dato->listo) {
            dato->confirmado_phy_ms = ms;
            dato->esperando = true;
        }
    }
}

static void Aplicar_Retorno(PhyEnlace *e, uint32_t ms, PhyRetorno retorno)
{
    const uint8_t anterior = PHY_Mapa_TX(e);
    e->retorno_valido = true;
    e->retorno_rx = retorno;
    e->mapa_tx = retorno.mapa;
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_RETORNO, .retorno = retorno, .ciclo = PHY_CICLO_RETORNO});
    if (anterior != retorno.mapa) {
        Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_MAPA_TX, .retorno = retorno});
    }
    if (e->cantidad_tx == 0U) {
        return;
    }

    const uint8_t distancia = (retorno.siguiente + PHY_ID_MODULO - e->primero_tx) % PHY_ID_MODULO;
    if (distancia > e->cantidad_tx || (retorno.pedir && distancia == e->cantidad_tx)) {
        return;
    }
    uint8_t id = e->primero_tx;
    for (uint8_t i = 0; i < distancia; ++i) {
        if (!e->ventana[id].terminado) {
            return;
        }
        id = Siguiente_Id(id);
    }
    for (uint8_t i = 0; i < distancia; ++i) {
        Confirmar_Bloque(e, ms, e->primero_tx);
        e->primero_tx = Siguiente_Id(e->primero_tx);
    }
    e->cantidad_tx -= distancia;
    if (!retorno.pedir || e->falla) {
        return;
    }

    PhyPendiente *pendiente = &e->ventana[retorno.siguiente];
    const bool repitiendo = e->cabecera_tx.tipo == PHY_REPITE && e->cabecera_tx.id == retorno.siguiente
                         && (e->ciclo_tx < e->plan_tx.ciclos || e->termina_en_bajada);
    if (!pendiente->pedido_atendido && pendiente->motivo != PHY_PEDIDO && !repitiendo) {
        pendiente->motivo = PHY_PEDIDO;
        Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_PEDIR, .retorno = retorno});
    }
}

void PHY_RX_Perder_Reloj(PhyEnlace *e, uint32_t ms)
{
    const bool mapa_util = PHY_Mapa_TX(e) != 0U;
    if (e->reloj_rx) {
        Error_Bloque(e, ms, PHY_ERR_CABECERA, PHY_RELOJ);
    }
    e->reloj_rx = false;
    e->retorno_valido = false;
    if (mapa_util) {
        Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_MAPA_TX, .motivo = PHY_RELOJ});
    }
}

static void Vigilar_Reloj(PhyEnlace *e, uint32_t ms)
{
    if (e->reloj_rx && ms - e->ultimo_pulso_ms > PLAZO_RELOJ_MS) {
        PHY_RX_Perder_Reloj(e, ms);
    }
}

void PHY_RX_Pulso(PhyEnlace *e, uint32_t ms, uint32_t periodo_ms, uint32_t ancho_ms, uint8_t simbolo)
{
    Vigilar_Reloj(e, ms);
    e->ultimo_pulso_ms = ms;
    const int par = PHY_Par_Reloj(ancho_ms);
    if (par < 0 || (periodo_ms != 0U && (periodo_ms < 950U || periodo_ms > 1050U))) {
        PHY_RX_Perder_Reloj(e, ms);
        if (par != PHY_MARCADOR) {
            return;
        }
    }
    if (par == PHY_MARCADOR) {
        if (e->bloque_rx) {
            Error_Bloque(e, ms, PHY_ERR_CABECERA, PHY_MARCADOR_ANTICIPADO);
        }
        e->reloj_rx = true;
        e->bloque_rx = true;
        e->cabecera_rx_valida = false;
        e->retorno_rx = (PhyRetorno){0};
        e->plan_rx.ciclos = 1;
        e->plan_rx.pares[0] = PHY_MARCADOR;
        e->plan_rx.simbolos[0] = simbolo & PHY_MAPA_COMPLETO;
        return;
    }
    if (!e->bloque_rx) {
        return;
    }

    const uint8_t ciclo = e->plan_rx.ciclos++;
    e->plan_rx.pares[ciclo] = (uint8_t)par;
    e->plan_rx.simbolos[ciclo] = simbolo & PHY_MAPA_COMPLETO;
    if (ciclo == 1U) {
        Medir_Mapa(e, ms, 0);
    } else if (ciclo == PHY_CICLO_RETORNO) {
        PhyRetorno retorno;
        if (PHY_Leer_Retorno(e->plan_rx.pares, &retorno)) {
            Aplicar_Retorno(e, ms, retorno);
        } else {
            ++e->errores;
            Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_RETORNO_INVALIDO, .resultado = PHY_ERR_CABECERA, .ciclo = ciclo});
        }
    } else if (ciclo == PHY_CICLO_CABECERA) {
        if (!PHY_Leer_Cabecera(e->plan_rx.pares, &e->cabecera_rx)) {
            Error_Bloque(e, ms, PHY_ERR_CABECERA, PHY_SIN_MOTIVO);
            return;
        }
        e->cabecera_rx_valida = true;
        const uint8_t mapa = e->cabecera_rx.mapa;
        if (mapa == PHY_MAPA_COMPLETO || e->cabecera_rx.tipo == PHY_IDLE) {
            Medir_Mapa(e, ms, 9);
        }
        if (e->cabecera_rx.tipo == PHY_IDLE) {
            e->bloque_rx = false;
            e->cabecera_rx_valida = false;
        } else if ((PHY_Mapa_Pilotos(e->plan_rx.simbolos[0], e->plan_rx.simbolos[1]) & mapa) != mapa) {
            Error_Bloque(e, ms, PHY_ERR_PILOTOS, PHY_SIN_MOTIVO);
        } else if (mapa == PHY_MAPA_COMPLETO) {
            Cerrar_Bloque_Rx(e, ms);
        }
    } else if (ciclo == PHY_CICLOS_LARGOS - 1U) {
        Cerrar_Bloque_Rx(e, ms);
    }
}

void PHY_TX_Bajada(PhyEnlace *e, uint32_t ms)
{
    if (!e->termina_en_bajada) {
        return;
    }
    e->termina_en_bajada = false;
    const PhyCabecera cabecera = e->cabecera_tx;
    PhyPendiente *pendiente = &e->ventana[cabecera.id];
    if (!pendiente->ocupado) {
        return;
    }
    if (!pendiente->terminado) {
        PhyCopia *copia = &e->copias[pendiente->copia];
        pendiente->terminado = true;
        if (++copia->enviados == 3U && e->dato_tx.existe && copia->intento == e->dato_tx.intento) {
            e->dato_tx.enviado = true;
            e->dato_tx.transmitiendo = false;
        }
    }
    if (cabecera.tipo == PHY_REPITE) {
        ++pendiente->reenvios;
        ++e->reenvios_phy;
        pendiente->pedido_atendido = true;
    }
    pendiente->esperando = true;
    pendiente->terminado_ms = ms;
}

static void Temporizadores(PhyEnlace *e, uint32_t ms)
{
    Vigilar_Reloj(e, ms);
    if (e->falla) {
        return;
    }

    const bool trabajo = e->longitud_tx || e->cantidad_tx || e->copia_tx != SIN_COPIA || e->ack_pendiente || e->nack_pendiente;
    if (PHY_Mapa_TX(e) == 0U && trabajo) {
        if (!e->sin_mapa) {
            e->sin_mapa = true;
            e->sin_mapa_ms = ms;
        } else if (ms - e->sin_mapa_ms >= PLAZO_MAPA_MS) {
            Fallar(e, ms, PHY_SIN_MAPA);
            return;
        }
    } else {
        e->sin_mapa = false;
    }
    for (uint8_t id = 0; id < PHY_ID_MODULO; ++id) {
        PhyPendiente *pendiente = &e->ventana[id];
        if (pendiente->ocupado && pendiente->esperando && ms - pendiente->terminado_ms >= PLAZO_PHY_MS) {
            pendiente->esperando = false;
            if (pendiente->reenvios >= MAX_REENVIOS) {
                Fallar(e, ms, PHY_PLAZO_BLOQUE);
                return;
            }
            if (pendiente->motivo == PHY_SIN_MOTIVO) {
                pendiente->motivo = PHY_PLAZO_BLOQUE;
            }
        }
    }
    if (e->dato_tx.esperando && ms - e->dato_tx.confirmado_phy_ms >= PLAZO_TRAMA_MS) {
        Reenviar_Trama(e, ms, PHY_PLAZO_TRAMA);
    }
}

static uint8_t Reservar_Copia(const PhyEnlace *e)
{
    for (uint8_t i = 0; i < PHY_COPIAS; ++i) {
        if (!e->copias[i].ocupado) {
            return i;
        }
    }
    return SIN_COPIA;
}

static void Preparar_Trama_Tx(PhyEnlace *e, uint32_t ms)
{
    const uint8_t indice = Reservar_Copia(e);
    if (indice == SIN_COPIA) {
        return;
    }
    PhyTrama trama = {.ack = e->seq_rx};
    PhyDatoTx *dato = &e->dato_tx;
    uint32_t intento = 0;
    if (e->nack_pendiente) {
        trama.control = PHY_CONTROL_NACK;
        trama.dato = trama.ack;
        e->nack_pendiente = false;
    } else {
        if (!dato->existe && e->longitud_tx != 0U) {
            *dato = (PhyDatoTx){.existe = true, .listo = true};
        }
        if (dato->listo) {
            dato->listo = false;
            dato->transmitiendo = true;
            if (++e->intento_tx == 0U) {
                ++e->intento_tx;
            }
            intento = e->intento_tx;
            dato->intento = intento;
            trama.control = e->control_tx;
            trama.dato = e->bytes_tx[e->posicion_tx];
            trama.seq = e->seq_tx;
            trama.inicio = e->posicion_tx == 0U;
            trama.fin = e->posicion_tx + 1U == e->longitud_tx;
            e->ack_pendiente = false;
        } else if (e->ack_pendiente) {
            trama.control = PHY_CONTROL_ACK;
            trama.dato = trama.ack;
            e->ack_pendiente = false;
        } else {
            return;
        }
    }
    PhyCopia *copia = &e->copias[indice];
    *copia = (PhyCopia){.ocupado = true, .intento = intento};
    PHY_Codificar_Trama(trama, copia->bloques);
    trama.crc_calculado = (uint8_t)(((copia->bloques[1] & 15U) << 4) | (copia->bloques[2] >> 8));
    e->copia_tx = indice;
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_TRAMA_TX, .ticket = intento ? e->ticket_tx : 0U, .trama = trama});
}

static uint8_t Abrir_Bloque_Nuevo(PhyEnlace *e, uint32_t ms)
{
    if (e->cantidad_tx >= PHY_VENTANA) {
        return PHY_SIN_ID;
    }
    if (e->copia_tx == SIN_COPIA) {
        Preparar_Trama_Tx(e, ms);
    }
    if (e->copia_tx == SIN_COPIA) {
        return PHY_SIN_ID;
    }

    const uint8_t id = e->siguiente_tx;
    PhyCopia *copia = &e->copias[e->copia_tx];
    e->ventana[id] = (PhyPendiente){.ocupado = true, .copia = e->copia_tx, .valor = copia->bloques[copia->asignados++]};
    if (e->cantidad_tx == 0U) {
        e->primero_tx = id;
    }
    ++e->cantidad_tx;
    e->siguiente_tx = Siguiente_Id(id);
    if (copia->asignados == 3U) {
        e->copia_tx = SIN_COPIA;
    }
    return id;
}

static uint8_t Buscar_Reenvio(const PhyEnlace *e, bool solo_pedido)
{
    uint8_t id = e->primero_tx;
    for (uint8_t i = 0; i < e->cantidad_tx; ++i) {
        const PhyMotivo motivo = e->ventana[id].motivo;
        if (motivo != PHY_SIN_MOTIVO && (!solo_pedido || motivo == PHY_PEDIDO)) {
            return id;
        }
        id = Siguiente_Id(id);
    }
    return PHY_SIN_ID;
}

static bool Hay_Datos_Listos(const PhyEnlace *e)
{
    return Buscar_Reenvio(e, false) != PHY_SIN_ID
        || (e->cantidad_tx < PHY_VENTANA && (e->copia_tx != SIN_COPIA || e->dato_tx.listo
            || (!e->dato_tx.existe && e->longitud_tx) || e->ack_pendiente || e->nack_pendiente));
}

static void Elegir_Bloque(PhyEnlace *e, uint32_t ms, uint8_t pedido)
{
    PhyCabecera cabecera = {PHY_IDLE, 0, 0};
    const PhyRetorno retorno = {Debe_Pedir(e), e->siguiente_rx, e->mapa_rx};
    const uint8_t mapa = PHY_Mapa_TX(e);
    PhyMotivo motivo = PHY_SIN_MOTIVO;
    uint16_t valor = 0;
    if (mapa && !e->falla) {
        uint8_t id = pedido != PHY_SIN_ID ? pedido : Buscar_Reenvio(e, false);
        PhyTipo tipo = PHY_REPITE;
        if (id == PHY_SIN_ID) {
            id = Abrir_Bloque_Nuevo(e, ms);
            tipo = PHY_NUEVO;
        }
        if (id != PHY_SIN_ID) {
            PhyPendiente *pendiente = &e->ventana[id];
            motivo = pendiente->motivo;
            pendiente->motivo = PHY_SIN_MOTIVO;
            pendiente->esperando = false;
            valor = pendiente->valor;
            cabecera = (PhyCabecera){tipo, id, mapa};
        }
    }
    e->ciclo_tx = 0;
    e->cabecera_tx = cabecera;
    PHY_Codificar_Bloque(&e->plan_tx, retorno, cabecera, valor);
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_BLOQUE_TX, .bloque = cabecera, .retorno = retorno, .valor = valor, .motivo = motivo});
}

static bool Adelantar_Marcador(PhyEnlace *e, uint32_t ms, uint8_t pedido)
{
    if (e->ciclo_tx <= PHY_CICLO_RETORNO || e->falla) {
        return false;
    }
    if (e->cabecera_tx.tipo == PHY_IDLE) {
        return PHY_Mapa_TX(e) != 0U && Hay_Datos_Listos(e);
    }
    if (pedido == PHY_SIN_ID || (e->cabecera_tx.tipo == PHY_REPITE && e->cabecera_tx.id == pedido)) {
        return false;
    }
    PhyPendiente *pendiente = &e->ventana[e->cabecera_tx.id];
    if (pendiente->ocupado && pendiente->motivo == PHY_SIN_MOTIVO) {
        pendiente->motivo = PHY_INTERRUMPIDO;
    }
    Registrar(e, ms, (PhyEvento){.tipo = PHY_EV_CORTE, .bloque = e->cabecera_tx,
                                 .retorno.siguiente = pedido, .ciclo = e->ciclo_tx});
    return true;
}

PhyPulso PHY_TX_Subida(PhyEnlace *e, uint32_t ms)
{
    Temporizadores(e, ms);
    const uint8_t pedido = Buscar_Reenvio(e, true);
    if (e->ciclo_tx >= e->plan_tx.ciclos || Adelantar_Marcador(e, ms, pedido)) {
        Elegir_Bloque(e, ms, pedido);
    }
    const uint8_t ciclo = e->ciclo_tx++;
    e->termina_en_bajada = e->ciclo_tx == e->plan_tx.ciclos && e->cabecera_tx.tipo != PHY_IDLE;
    return (PhyPulso){PHY_Ancho_Reloj(e->plan_tx.pares[ciclo]), e->plan_tx.simbolos[ciclo], ciclo};
}
