import test from 'node:test'
import assert from 'node:assert/strict'
import { bits, codificarMensaje, comandoEnvio, describirEvento, leerRespuesta } from '../src/chat.js'
import { leerLineas } from '../src/serial.js'

test('UTF-8 de uno a cuatro bytes, ASCII extendido e integridad del texto', () => {
  assert.deepEqual(codificarMensaje('Añ€👍', 'utf8', 3), { control: 14, bytes: 10, hex: '41C3B1E282ACF09F918D' })
  assert.equal(leerRespuesta('@M,14,41C3B1E282ACF09F918D').texto, 'Añ€👍')
  assert.equal(codificarMensaje('Mañana', 'latin1', 0).hex, '4D61F1616E61')
  assert.equal(leerRespuesta('@M,0,80F1').texto, '\x80ñ')
  assert.equal(leerRespuesta('@M,8,efbbbf41').texto, '\ufeffA')
  assert.throws(() => leerRespuesta('@M,8,f09f91'))
})

test('los cuatro CRC y restricciones de bytes para cada codificación', () => {
  assert.deepEqual([0, 1, 2, 3].map(i => codificarMensaje('o', 'latin1', i).control), [0, 2, 4, 6])
  assert.equal(codificarMensaje('👍'.repeat(16), 'utf8', 0).bytes, 64)
  assert.throws(() => codificarMensaje('👍'.repeat(17), 'utf8', 0), /64 bytes/)
  assert.throws(() => codificarMensaje('👍', 'latin1', 0), /UTF-8/)
  assert.throws(() => codificarMensaje('\ud800', 'utf8', 0), /incompleto/)
  assert.equal(comandoEnvio(4294967295, codificarMensaje('Ñ', 'latin1', 1)), '@S,4294967295,2,D1\n')
})

test('estado del enlace, aceptación y entrega conservan el ticket', () => {
  const h = leerRespuesta('@H,2,2,6,4,1,1000,800,4,8,3,1,2,1,0,4294967295,1,0,10000')
  assert.equal(h.lado, 2)
  assert.equal(bits(h.mapaTx), '110')
  assert.equal(bits(h.mapaRx), '100')
  assert.equal(h.estadoEnvio, 1)
  assert.deepEqual(leerRespuesta('@K,4294967295'), { tipo: 'entregado', ticket: h.ticket })
  assert.deepEqual(leerRespuesta('@Q,4294967295'), { tipo: 'encolado', ticket: h.ticket })
  assert.throws(() => leerRespuesta('@K,4294967296'))
})

test('el registro muestra el CRC recibido frente al calculado', () => {
  const evento = leerRespuesta('@G,18000,10,0,0,0,0,0,0,0,0,0,0,96,0,0,3,10,39,0,7,0')
  const descripcion = describirEvento(evento)
  assert.equal(descripcion.capa, 'Red')
  assert.equal(descripcion.error, true)
  assert.match(descripcion.detalle, /CRC SMBUS 0A\/27/)
  assert.match(descripcion.detalle, /INICIO.*FIN.*CRC de trama/)
})

test('desglosa los bits de ambas secciones del reloj y del CRC PHY', () => {
  const evento = leerRespuesta('@G,17000,0,0,1,1,6,1,0,7,0,1776,0,0,0,0,0,0,0,0,0,0,571,312,160,0,0')
  const log = describirEvento(evento)
  assert.equal(log.capa, 'PHY')
  assert.deepEqual(log.reloj[0].campos, { PEDIR: 1, SIG_RX: 0, MAPA_RX: '111', Reservado: 0 })
  assert.deepEqual(log.reloj[0].pares, ['10', '00', '11', '10', '11'])
  assert.deepEqual(log.reloj[0].pulsosNominalesMs, [600, 400, 700, 600, 700])
  assert.deepEqual(log.reloj[1].campos, { TIPO: 'NUEVO', ID: 1, MAPA_TX: '110' })
  assert.equal(log.reloj[1].paridad, '00')
  assert.equal(log.reloj[1].paridadEsperada, '00')
  assert.match(log.detalle, /Carga 6F0.*CRC_PHY del reloj A0/)

  const actual = leerRespuesta('@G,17000,0,0,1,1,6,1,0,7,0,1776,0,0,0,0,0,0,0,0,0,0,571,312,256,0,0')
  assert.equal(actual.crcPhyReloj, null)
  assert.deepEqual(describirEvento(actual).reloj, log.reloj)
  assert.doesNotMatch(describirEvento(actual).detalle, /CRC_PHY/)

  const retorno = leerRespuesta('@G,5700,3,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,1,0,570,0,0,0,0')
  const invalido = describirEvento(retorno)
  assert.equal(invalido.error, true)
  assert.equal(invalido.reloj[0].paridad, '10')
  assert.equal(invalido.reloj[0].paridadEsperada, '11')
})

test('distingue el fin, el corte PHY y las cabeceras de trama de red', () => {
  const corte = leerRespuesta('@G,23000,5,0,1,1,7,1,0,6,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0')
  assert.match(describirEvento(corte).detalle, /Bloque #1.*111 → 110.*ID prioritario #0/)
  const fin = leerRespuesta('@G,38000,16,0,2,0,6,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,160,0,0')
  assert.equal(describirEvento(fin).nombre, 'Fin de bloque TX')
  assert.match(describirEvento(fin).detalle, /REPITE #0.*ciclo 14.*A0/)

  const trama = leerRespuesta('@G,39000,10,0,0,0,0,0,0,0,0,994,8,97,0,0,3,136,136,0,0,0,0,0,0,0,0')
  const red = describirEvento(trama)
  assert.equal(red.capa, 'Red')
  assert.equal(red.error, false)
  assert.match(red.detalle, /SYN 001.*SFD 111.*CONTROL 1000.*DATA 61.*CRC SMBUS 88\/88/)
})

test('recupera mensajes serie partidos o después de ruido', () => {
  const lineas = []
  const entrada = leerLineas(linea => lineas.push(linea))
  const encode = fragmento => new TextEncoder().encode(fragmento)
  for (const parte of ['ruido@Q', ',9\r', '\n@mal@K,', '9\n', '@' + '0'.repeat(280), '\n@M,0,6F\n']) {
    entrada(encode(parte))
  }
  assert.deepEqual(lineas, ['@Q,9', '@K,9', '@M,0,6F'])
})
