export const MAX_BYTES = 64
export const CRC = ['SMBUS', 'AUTOSAR', 'SAE-J1850', 'MAXIM-DOW']
export const ERRORES = [
  '', 'Pedido PHY', 'Bloque interrumpido', 'Plazo de bloque agotado', 'NACK',
  'Plazo de trama agotado', 'Sin mapa útil durante 60 s', 'Reloj perdido',
  'Marcador anticipado', 'Comando serie inválido', 'La K8 tiene un envío pendiente',
]
const RESULTADOS = [
  '', 'Cabecera', 'Pilotos', 'Paridad o complemento', 'XOR por línea',
  'CRC de bloque', 'Estructura de trama', 'CRC de trama',
]
const TIPOS = ['IDLE', 'NUEVO', 'REPITE']

export function bits(mapa) {
  return (mapa & 7).toString(2).padStart(3, '0')
}

function hex(n, ancho = 2) {
  return n.toString(16).toUpperCase().padStart(ancho, '0')
}

export function codificarMensaje(texto, formato, crc) {
  if (!texto.length) throw new Error('Escribe un mensaje.')
  if (!Number.isInteger(crc) || crc < 0 || crc >= CRC.length) {
    throw new Error('Selecciona una variante de CRC.')
  }
  const caracteres = [...texto].map(caracter => caracter.codePointAt(0))
  if (caracteres.some(n => n >= 0xd800 && n <= 0xdfff)) {
    throw new Error('Hay un carácter Unicode incompleto.')
  }
  const utf8 = formato === 'utf8'
  if (!utf8 && caracteres.some(n => n > 255)) {
    throw new Error('Selecciona UTF-8 para estos caracteres.')
  }
  const bytes = utf8 ? new TextEncoder().encode(texto) : Uint8Array.from(caracteres)
  if (bytes.length > MAX_BYTES) {
    throw new Error('El límite es de 64 bytes por mensaje.')
  }
  const control = (utf8 ? 8 : 0) | (crc << 1)
  return { control, bytes: bytes.length, hex: Array.from(bytes, byte => hex(byte)).join('') }
}

export function comandoEnvio(ticket, mensaje) {
  return '@S,' + ticket + ',' + mensaje.control + ',' + mensaje.hex + '\n'
}

function numero(texto) {
  if (!/^\d{1,10}$/.test(texto) || Number(texto) > 0xffffffff) {
    throw new Error('Campo numérico inválido en el puerto serie.')
  }
  return Number(texto)
}

export function leerRespuesta(linea) {
  const [tipo, ...args] = linea.split(',')
  if (tipo === '@H' && args.length === 18) {
    const [version, lado, mapaTx, mapaRx, reloj, periodo, ancho, simbolo, bloques, tramas,
      errores, repitePhy, repiteTrama, falla, ticket, estadoEnvio, logsPerdidos, ms] = args.map(numero)
    if (version !== 2 || ![1, 2].includes(lado)) {
      throw new Error('El firmware de la K8 no corresponde a esta versión del chat.')
    }
    if (mapaTx > 7 || mapaRx > 7 || reloj > 1 || simbolo > 7 || falla > 8 || estadoEnvio > 3) {
      throw new Error('Estado de la K8 inválido.')
    }
    return { tipo: 'estado', lado, mapaTx, mapaRx, reloj: Boolean(reloj), periodo, ancho,
      simbolo, bloques, tramas, errores, repitePhy, repiteTrama, falla, ticket, estadoEnvio, logsPerdidos, ms }
  }
  if (tipo === '@M' && args.length === 2) {
    const control = numero(args[0])
    if (control > 15 || (control & 1) || !/^(?:[0-9a-f]{2}){1,64}$/i.test(args[1])) {
      throw new Error('Mensaje serie inválido.')
    }
    const bytes = Uint8Array.from(args[1].match(/../g), par => Number.parseInt(par, 16))
    const texto = (control & 8)
      ? new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes)
      : String.fromCharCode(...bytes)
    return { tipo: 'mensaje', control, texto, bytes: bytes.length }
  }
  if ((tipo === '@Q' || tipo === '@K') && args.length === 1) {
    return { tipo: tipo === '@Q' ? 'encolado' : 'entregado', ticket: numero(args[0]) }
  }
  if (tipo === '@E' && args.length === 2) {
    return { tipo: 'fallido', ticket: numero(args[0]), motivo: numero(args[1]) }
  }
  if (tipo === '@G' && (args.length === 21 || args.length === 26)) {
    const [ms, codigo, ticket, bloque, id, mapa, pedir, siguiente, retornoMapa, ciclo,
      valor, control, dato, ack, seq, banderas, crcRecibido, crcCalculado,
      motivo, resultado, orden, bitsRetorno, bitsCabecera, crcPhyReloj, anchoMs, periodoMs] = args.map(numero)
    const evento = { tipo: 'log', ms, codigo, ticket, bloque, id, mapa, pedir, siguiente, retornoMapa,
      ciclo, valor, control, dato, ack, seq, banderas, crcRecibido, crcCalculado, motivo, resultado, orden }
    if (args.length === 21) return evento
    if (bitsRetorno > 1023 || bitsCabecera > 1023 || crcPhyReloj > 256) {
      throw new Error('Campos del reloj inválidos en el registro serie.')
    }
    return {
      ...evento,
      bitsRetorno: [0, 2, 3].includes(codigo) ? bitsRetorno : null,
      bitsCabecera: [0, 17].includes(codigo) ? bitsCabecera : null,
      crcPhyReloj: [0, 1, 6, 16].includes(codigo) && crcPhyReloj < 256 ? crcPhyReloj : null,
      anchoMs: codigo === 18 ? anchoMs : null,
      periodoMs: codigo === 18 ? periodoMs : null,
    }
  }
  return null
}

const EVENTOS = [
  ['Inicio de bloque TX', 'PHY'], ['Bloque RX validado', 'PHY'], ['Retorno RX', 'PHY'],
  ['Retorno RX inválido', 'PHY'], ['PEDIR recibido', 'PHY'], ['Corte TX', 'PHY'],
  ['Bloque descartado', 'PHY'], ['Mapa RX', 'PHY'], ['Mapa TX', 'PHY'],
  ['Trama TX', 'Red'], ['Trama RX', 'Red'], ['Reenvío de trama', 'Red'],
  ['ACK de trama', 'Red'], ['Mensaje entregado', 'Red'], ['Fallo de enlace', 'Red'],
  ['ACK PHY', 'PHY'], ['Fin de bloque TX', 'PHY'], ['Cabecera de bloque RX', 'PHY'],
  ['Pulso RX inválido', 'PHY'],
]

function seccionReloj(valor, retorno) {
  const pares = [8, 6, 4, 2, 0].map(desplazamiento => (valor >> desplazamiento) & 3)
  const [b0, b1, b2, b3, paridad] = pares
  const binario = n => n.toString(2).padStart(2, '0')
  const campos = valor >> 2
  return {
    nombre: retorno ? 'Retorno · ciclos 1 a 5' : 'Bloque · ciclos 6 a 10',
    campos: retorno
      ? { PEDIR: campos >> 7, SIG_RX: (campos >> 4) & 7, MAPA_RX: bits(campos >> 1), Reservado: campos & 1 }
      : { TIPO: TIPOS[campos >> 6] ?? 'RESERVADO', ID: (campos >> 3) & 7, MAPA_TX: bits(campos) },
    pares: pares.map(binario),
    paridad: binario(paridad),
    paridadEsperada: binario(b0 ^ b1 ^ b2 ^ b3),
    pulsosNominalesMs: pares.map(par => 400 + 100 * par),
  }
}

function nombreBloque(ev) {
  const tipo = TIPOS[ev.bloque] ?? '?'
  return ev.bloque ? tipo + ' #' + ev.id + ' · MAPA_TX ' + bits(ev.mapa) : tipo + ' · MAPA_TX 000'
}

export function describirEvento(ev) {
  let [nombre, capa] = EVENTOS[ev.codigo] ?? ['Evento', 'PHY']
  if (ev.codigo === 14 && [3, 6].includes(ev.motivo)) capa = 'PHY'
  if (ev.codigo === 6 && ev.motivo === 8) nombre = 'Marcador anticipado RX'
  const error = (ev.resultado !== 0 && ev.motivo !== 8) || ev.codigo === 14
  const extendido = ev.bitsRetorno !== undefined
  const reloj = []
  if (ev.bitsRetorno != null) reloj.push(seccionReloj(ev.bitsRetorno, true))
  if (ev.bitsCabecera != null) reloj.push(seccionReloj(ev.bitsCabecera, false))
  let detalle = ''
  switch (ev.codigo) {
    case 0:
    case 1: {
      const carga = ev.bloque ? ' · Carga ' + hex(ev.valor, 3) : ''
      const orden = ev.codigo === 1 ? ['', ' · retenido', ' · duplicado o fuera de ventana'][ev.orden] ?? '' : ''
      const motivo = ev.motivo ? ' · ' + ERRORES[ev.motivo] : ''
      const retorno = ev.codigo === 0 ? ' · PEDIR ' + ev.pedir + ' · SIG_RX ' + ev.siguiente +
        ' · MAPA_RX ' + bits(ev.retornoMapa) : ' · ciclo ' + ev.ciclo
      detalle = nombreBloque(ev) + carga + retorno + orden + motivo
      break
    }
    case 2:
      detalle = 'PEDIR ' + ev.pedir + ' · SIG_RX ' + ev.siguiente + ' · MAPA_RX ' + bits(ev.retornoMapa)
      break
    case 3:
      detalle = 'Sección de retorno descartada · ciclo ' + ev.ciclo
      break
    case 4: {
      detalle = 'ID pedido #' + ev.siguiente + ' · MAPA_RX ' + bits(ev.retornoMapa)
      if (extendido && ev.bloque) {
        detalle += (ev.mapa & ev.retornoMapa) === ev.mapa ? ' · termina bloque #' + ev.id
          : ' · faltan líneas del bloque #' + ev.id
      }
      break
    }
    case 15:
      detalle = '#' + ev.id + ' · SIG_RX ' + ev.siguiente + ' · MAPA_RX ' + bits(ev.retornoMapa)
      break
    case 5:
      detalle = (ev.bloque ? 'Bloque #' + ev.id : 'IDLE') + ' · antes del ciclo ' + ev.ciclo
      if (extendido && ev.bloque) {
        detalle += ' · pérdida de línea ' + bits(ev.mapa) + ' → ' + bits(ev.retornoMapa)
      }
      detalle += ev.siguiente < 6 ? ' · ID prioritario #' + ev.siguiente : ' · datos listos'
      if (extendido && ev.retornoMapa === 0) detalle += ' · espera en IDLE'
      break
    case 7:
    case 8:
      detalle = bits(ev.retornoMapa) + (ev.motivo ? ' · ' + ERRORES[ev.motivo] : '')
      break
    case 9:
    case 10: {
      const esControl = Boolean(ev.control & 1)
      const tipo = esControl ? (ev.control & 8 ? 'NACK' : 'ACK') : (ev.control & 8 ? 'UTF-8' : 'ISO-8859-1')
      const crc = CRC[(ev.control >> 1) & 3] + ' ' +
        (ev.codigo === 10 ? hex(ev.crcRecibido) + '/' : '') + hex(ev.crcCalculado)
      const cabecera = extendido ? ' · SYN ' + bits(ev.valor >> 9) + ' · SFD ' + bits(ev.valor >> 6) +
        ' · CONTROL ' + ev.control.toString(2).padStart(4, '0') : ''
      detalle = tipo + cabecera + ' · SEQ ' + ev.seq + ' · ACK ' + ev.ack + ' · DATA ' + hex(ev.dato) + ' · CRC ' + crc +
        (ev.banderas & 1 ? ' · INICIO' : '') + (ev.banderas & 2 ? ' · FIN' : '') +
        (ev.resultado ? ' · ' + RESULTADOS[ev.resultado] : '')
      break
    }
    case 11:
      detalle = 'SEQ ' + ev.seq + ' · ' + ERRORES[ev.motivo]
      break
    case 12:
      detalle = 'SEQ ' + ev.seq + ' · ACK ' + ev.ack
      break
    case 13:
      detalle = 'Envío ' + ev.ticket
      break
    case 16:
      detalle = nombreBloque(ev) + ' · última bajada en ciclo ' + ev.ciclo
      break
    case 17:
      detalle = (ev.resultado ? 'Sección de bloque descartada' : nombreBloque(ev)) + ' · ciclo ' + ev.ciclo
      break
    case 18:
      detalle = 'Pulso en alto ' + ev.anchoMs + ' ms'
        + (ev.periodoMs ? ' · período ' + ev.periodoMs + ' ms' : '')
      break
    default:
      detalle = [
        ev.codigo === 6 && ev.id < 6 ? 'Bloque #' + ev.id : '',
        ERRORES[ev.motivo], RESULTADOS[ev.resultado],
        ev.codigo === 6 ? 'ciclo ' + ev.ciclo : '',
      ].filter(Boolean).join(' · ')
  }
  if (ev.crcPhyReloj != null) detalle += ' · CRC_PHY del reloj ' + hex(ev.crcPhyReloj)
  return { nombre, capa, error, detalle, ...(reloj.length ? { reloj } : {}) }
}
