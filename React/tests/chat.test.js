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
  assert.equal(descripcion.error, true)
  assert.match(descripcion.detalle, /CRC SMBUS 0A\/27/)
  assert.match(descripcion.detalle, /INICIO.*FIN.*CRC de trama/)
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
