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
  if (tipo === '@G' && args.length === 21) {
    const [ms, codigo, ticket, bloque, id, mapa, pedir, siguiente, retornoMapa, ciclo,
      valor, control, dato, ack, seq, banderas, crcRecibido, crcCalculado,
      motivo, resultado, orden] = args.map(numero)
    return { tipo: 'log', ms, codigo, ticket, bloque, id, mapa, pedir, siguiente, retornoMapa,
      ciclo, valor, control, dato, ack, seq, banderas, crcRecibido, crcCalculado, motivo, resultado, orden }
  }
  return null
}

const EVENTOS = [
  ['Bloque TX', 'PHY'], ['Bloque RX', 'PHY'], ['Cabecera RX', 'PHY'],
  ['Cabecera RX inválida', 'PHY'], ['PEDIR recibido', 'PHY'], ['Corte TX', 'PHY'],
  ['Bloque descartado', 'PHY'], ['Mapa RX', 'PHY'], ['Mapa TX', 'PHY'],
  ['Trama TX', 'Enlace'], ['Trama RX', 'Enlace'], ['Reenvío de trama', 'Enlace'],
  ['ACK de trama', 'Enlace'], ['Mensaje entregado', 'Enlace'], ['Fallo de enlace', 'Enlace'],
  ['ACK PHY', 'PHY'],
]

export function describirEvento(ev) {
  const [nombre, capa] = EVENTOS[ev.codigo] ?? ['Evento', 'PHY']
  const error = (ev.resultado !== 0 && ev.motivo !== 8) || ev.codigo === 14
  let detalle = ''
  switch (ev.codigo) {
    case 0:
    case 1: {
      const bloque = TIPOS[ev.bloque] ?? '?'
      const pedido = ev.pedir ? ' · PEDIR' : ''
      const carga = ev.bloque ? ' #' + ev.id + ' · MAPA_TX ' + bits(ev.mapa) + ' · ' + hex(ev.valor, 3) : ''
      const orden = ev.codigo === 1 ? ['', ' · retenido', ' · duplicado o fuera de ventana'][ev.orden] ?? '' : ''
      const motivo = ev.motivo ? ' · ' + ERRORES[ev.motivo] : ''
      detalle = bloque + carga + ' · SIG_RX ' + ev.siguiente + ' · MAPA_RX ' + bits(ev.retornoMapa) + pedido + orden + motivo
      break
    }
    case 2:
    case 4:
    case 15:
      detalle = (ev.codigo === 15 ? '#' + ev.id + ' · ' : '') + 'SIG_RX ' + ev.siguiente +
        ' · MAPA_RX ' + bits(ev.retornoMapa) + (ev.pedir ? ' · PEDIR' : '')
      break
    case 5:
      detalle = 'Bloque #' + ev.id + ' · ciclo ' + ev.ciclo + ' · reenvío prioritario #' + ev.siguiente
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
      detalle = tipo + ' · SEQ ' + ev.seq + ' · ACK ' + ev.ack + ' · DATA ' + hex(ev.dato) + ' · CRC ' + crc +
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
    default:
      detalle = [
        ev.codigo === 6 && ev.id < 6 ? 'Bloque #' + ev.id : '',
        ERRORES[ev.motivo], RESULTADOS[ev.resultado],
      ].filter(Boolean).join(' · ')
  }
  return { nombre, capa, error, detalle }
}
