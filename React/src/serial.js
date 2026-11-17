const encoder = new TextEncoder()

export function leerLineas(alRecibir) {
  let linea = null
  return bytes => {
    for (const byte of bytes) {
      if (byte === 64) {
        linea = '@'
      } else if (byte === 10) {
        if (linea?.length > 1) alRecibir(linea)
        linea = null
      } else if (byte !== 13 && linea !== null) {
        linea = linea.length < 256 ? linea + String.fromCharCode(byte) : null
      }
    }
  }
}

export async function conectarSerie(serial) {
  const puerto = await serial.requestPort()
  await puerto.open({ baudRate: 9600, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' })
  let lector
  let escritor
  try {
    lector = puerto.readable.getReader()
    escritor = puerto.writable.getWriter()
  } catch (error) {
    lector?.releaseLock()
    await puerto.close().catch(() => {})
    throw error
  }

  let cierre = null
  let lectura = Promise.resolve()
  let escritura = Promise.resolve()

  function enviar(linea) {
    if (cierre) return Promise.reject(new Error('El puerto se está cerrando.'))
    const tarea = escritura.then(() => escritor.write(encoder.encode(linea)))
    escritura = tarea.catch(() => {})
    return tarea
  }

  function escuchar(alRecibir, alPerder) {
    const procesar = leerLineas(alRecibir)
    lectura = (async () => {
      try {
        for (;;) {
          const { value, done } = await lector.read()
          if (done || cierre) return
          procesar(value)
        }
      } catch {
        // Tanto un error como EOF terminan la lectura.
      }
    })()
    void lectura.then(() => {
      if (!cierre) alPerder()
    })
  }

  function cerrar(avisar = false) {
    cierre ??= (async () => {
      await escritura
      if (avisar) await escritor.write(encoder.encode('@C\n')).catch(() => {})
      await lector.cancel().catch(() => {})
      await lectura
      lector.releaseLock()
      escritor.releaseLock()
      await puerto.close().catch(() => {})
    })()
    return cierre
  }

  return { enviar, escuchar, cerrar }
}
