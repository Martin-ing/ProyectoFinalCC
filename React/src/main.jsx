import React, { useEffect, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { bits, codificarMensaje, comandoEnvio, CRC, describirEvento, ERRORES, leerRespuesta, MAX_BYTES } from './chat.js'
import { conectarSerie } from './serial.js'
import './styles.css'

const ESTADOS_ENVIO = ['escribiendo', 'encolado', 'entregado', 'fallido']
const ETIQUETAS_ENVIO = {
  escribiendo: 'Enviando a la K8', encolado: 'En transmisión', entregado: 'Entregado',
  fallido: 'Falló', sin_confirmar: 'Sin confirmar',
}
const MAX_LOGS = 5000

function Mapa({ nombre, mascara }) {
  return <div className="mapa">
    <span>{nombre}</span>
    <code>{mascara === undefined ? '···' : bits(mascara)}</code>
  </div>
}

function descargarLogs(logs) {
  const url = URL.createObjectURL(new Blob([JSON.stringify(logs, null, 2)], { type: 'application/json' }))
  const link = document.createElement('a')
  link.href = url
  link.download = 'registro-phy.json'
  link.click()
  setTimeout(() => URL.revokeObjectURL(url), 1000)
}

function Registro({ logs, placa }) {
  const [filtro, setFiltro] = useState('todos')
  const visibles = logs.filter(ev =>
    filtro === 'todos' || (filtro === 'errores' ? ev.error : ev.capa === filtro))

  return <section className="panel registro" aria-labelledby="titulo-registro">
    <header className="panel-cabecera">
      <div>
        <h2 id="titulo-registro">Monitor del enlace</h2>
        <p>{logs.length} {logs.length === 1 ? 'evento' : 'eventos'}</p>
      </div>
      <button className="boton boton-neutro" onClick={() => descargarLogs(logs)} disabled={!logs.length}>
        Descargar registro
      </button>
    </header>
    <dl className="mediciones">
      <div><dt>Período RX</dt><dd>{placa?.periodo ? placa.periodo + ' ms' : '·'}</dd></div>
      <div><dt>Pulso en alto</dt><dd>{placa?.ancho ? placa.ancho + ' ms' : '·'}</dd></div>
      <div><dt>Bloques / tramas</dt><dd>{placa ? placa.bloques + ' / ' + placa.tramas : '·'}</dd></div>
      <div><dt>Errores</dt><dd>{placa?.errores ?? '·'}</dd></div>
      <div><dt>Reenvíos PHY</dt><dd>{placa?.repitePhy ?? '·'}</dd></div>
      <div><dt>Reenvíos de trama</dt><dd>{placa?.repiteTrama ?? '·'}</dd></div>
    </dl>
    <div className="filtros">
      <label htmlFor="filtro">Eventos</label>
      <select id="filtro" value={filtro} onChange={evt => setFiltro(evt.target.value)}>
        <option value="todos">Todos</option>
        <option value="PHY">Capa física</option>
        <option value="Enlace">Enlace</option>
        <option value="errores">Errores</option>
      </select>
      {placa?.logsPerdidos > 0 && <span className="texto-error">{placa.logsPerdidos} registros omitidos</span>}
    </div>
    <div className="eventos">
      {!visibles.length && <p className="vacio">Sin eventos en esta vista.</p>}
      {[...visibles].reverse().map(ev => <article key={ev.uid} className={ev.error ? 'evento evento-error' : 'evento'}>
        <div><strong>{ev.nombre}</strong><time>{(ev.ms / 1000).toFixed(3)} s</time></div>
        {ev.detalle && <p>{ev.detalle}</p>}
      </article>)}
    </div>
  </section>
}

function App() {
  const conexion = useRef(null)
  const ticket = useRef(crypto.getRandomValues(new Uint32Array(1))[0])
  const id = useRef(0)
  const historial = useRef(null)
  const [fase, setFase] = useState('cerrado')
  const [placa, setPlaca] = useState(null)
  const [mensajes, setMensajes] = useState([])
  const [logs, setLogs] = useState([])
  const [aviso, setAviso] = useState('')
  const [texto, setTexto] = useState('')
  const [formato, setFormato] = useState('utf8')
  const [crc, setCrc] = useState(0)

  let preparado
  let invalido = ''
  if (texto) {
    try {
      preparado = codificarMensaje(texto, formato, crc)
    } catch (error) {
      invalido = error.message
    }
  }

  useEffect(() => {
    if (historial.current) historial.current.scrollTop = historial.current.scrollHeight
  }, [mensajes])

  function actualizarEnvio(ticketLocal, estado) {
    setMensajes(antes => antes.map(m => {
      if (!m.propio || m.ticket !== ticketLocal) return m
      if (estado === 'encolado' && ['entregado', 'fallido'].includes(m.estado)) return m
      return { ...m, estado }
    }))
  }

  function confirmarComando(sesion, ticketLocal) {
    if (sesion.ticketPendiente !== ticketLocal) return
    clearTimeout(sesion.plazoComando)
    sesion.ticketPendiente = null
  }

  async function desconectar(sesion, mensaje = '', avisar = false) {
    if (!sesion || conexion.current !== sesion || sesion.cerrando) return
    sesion.cerrando = true
    clearInterval(sesion.sondeo)
    clearTimeout(sesion.plazoComando)
    setFase('cerrando')
    await sesion.serie.cerrar(avisar)
    if (conexion.current !== sesion) return
    conexion.current = null
    setFase('cerrado')
    setPlaca(null)
    setAviso(mensaje)
    setMensajes(antes => antes.map(m => ['escribiendo', 'encolado'].includes(m.estado)
      ? { ...m, estado: 'sin_confirmar' } : m))
  }

  function recibir(sesion, linea) {
    if (conexion.current !== sesion || sesion.cerrando) return
    let dato
    try {
      dato = leerRespuesta(linea)
    } catch (error) {
      setAviso(error.message)
      return
    }
    if (!dato) return
    switch (dato.tipo) {
      case 'estado': {
        if (sesion.placa && ((dato.ms - sesion.placa.ms) | 0) < -1000) {
          setAviso('La K8 se reinició. Reinicia ambas placas para alinear el enlace.')
          setMensajes(antes => antes.map(m => ['escribiendo', 'encolado'].includes(m.estado)
            ? { ...m, estado: 'sin_confirmar' } : m))
        }
        sesion.placa = dato
        sesion.ultimoEstado = performance.now()
        setPlaca(dato)
        setFase('conectado')
        if (dato.ticket && dato.estadoEnvio) {
          confirmarComando(sesion, dato.ticket)
          actualizarEnvio(dato.ticket, ESTADOS_ENVIO[dato.estadoEnvio])
        }
        break
      }
      case 'mensaje': {
        const mensaje = { ...dato, uid: ++id.current, propio: false, hora: new Date() }
        setMensajes(antes => [...antes, mensaje])
        break
      }
      case 'log': {
        const evento = { ...dato, ...describirEvento(dato), uid: ++id.current, lado: sesion.placa?.lado }
        setLogs(antes => [...antes.slice(1 - MAX_LOGS), evento])
        break
      }
      case 'encolado':
      case 'entregado':
        confirmarComando(sesion, dato.ticket)
        actualizarEnvio(dato.ticket, dato.tipo)
        break
      case 'fallido':
        confirmarComando(sesion, dato.ticket)
        if (dato.ticket) actualizarEnvio(dato.ticket, 'fallido')
        setAviso(ERRORES[dato.motivo] || 'El envío falló.')
        break
    }
  }

  async function conectar() {
    if (!navigator.serial) {
      setAviso('Abre el chat en Chrome o Edge desde localhost para usar Web Serial.')
      return
    }
    setFase('conectando')
    setAviso('')
    let sesion
    try {
      const serie = await conectarSerie(navigator.serial)
      sesion = { serie, ultimoEstado: performance.now() }
      conexion.current = sesion
      setFase('verificando')
      serie.escuchar(linea => recibir(sesion, linea),
        () => void desconectar(sesion, 'Se interrumpió el puerto serie.'))
      sesion.sondeo = setInterval(() => {
        if (performance.now() - sesion.ultimoEstado > 5000) {
          void desconectar(sesion, 'La K8 no responde. Revisa el COM y deja el firmware en ejecución.')
        } else {
          void serie.enviar('@H\n').catch(() => desconectar(sesion, 'Se interrumpió el puerto serie.'))
        }
      }, 1400)
      await serie.enviar('@H\n')
    } catch (error) {
      const motivo = error.name === 'NotFoundError' ? '' : 'No se pudo abrir el puerto. Revisa que el COM esté libre.'
      if (sesion) {
        await desconectar(sesion, motivo)
      } else {
        setFase('cerrado')
        setAviso(motivo)
      }
    }
  }

  const ocupado = placa?.estadoEnvio === 1 || mensajes.some(m => m.estado === 'escribiendo' || m.estado === 'encolado')
  const habilitado = fase === 'conectado' && placa && !placa.falla && !ocupado && Boolean(preparado)

  async function enviar(evento) {
    evento.preventDefault()
    const sesion = conexion.current
    if (!habilitado || !sesion || sesion.cerrando || sesion.ticketPendiente) return
    const ticketLocal = ticket.current = (ticket.current + 1) >>> 0 || 1
    sesion.ticketPendiente = ticketLocal
    const mensaje = {
      propio: true, uid: ++id.current, hora: new Date(), ticket: ticketLocal, texto,
      bytes: preparado.bytes, control: preparado.control, estado: 'escribiendo',
    }
    setAviso('')
    setMensajes(antes => [...antes, mensaje])
    sesion.plazoComando = setTimeout(() => {
      confirmarComando(sesion, ticketLocal)
      actualizarEnvio(ticketLocal, 'sin_confirmar')
      setAviso('No llegó la confirmación de la cola de la K8.')
    }, 4500)
    try {
      await sesion.serie.enviar(comandoEnvio(ticketLocal, preparado))
      setTexto('')
    } catch {
      confirmarComando(sesion, ticketLocal)
      actualizarEnvio(ticketLocal, 'fallido')
      await desconectar(sesion, 'No se pudo escribir en el puerto serie.')
    }
  }

  const estados = {
    cerrado: 'Sin puerto', conectando: 'Abriendo puerto', verificando: 'Buscando K8',
    cerrando: 'Cerrando puerto',
  }
  const estado = placa?.falla ? 'Fallo de enlace' : placa?.mapaTx ? 'Enlace listo'
    : placa ? 'Buscando canales' : estados[fase]
  const puedeCerrar = fase === 'conectado' || fase === 'verificando'
  const puedeConectar = fase === 'cerrado'

  return (
    <main className="app">
      <header className="barra">
        <div className="marca">
          <span className="simbolo" aria-hidden="true"><i /><i /><i /></span>
          <div><h1>Chat PHY</h1><p>{placa
            ? 'Lado ' + placa.lado + ' · audio TX ' + (placa.lado === 1 ? '3' : '4') + ' kHz / RX ' + (placa.lado === 1 ? '4' : '3') + ' kHz'
            : 'NUCLEO-F303K8'}</p></div>
        </div>
        <div className="conector">
          <Mapa nombre="TX" mascara={placa?.mapaTx} />
          <Mapa nombre="RX" mascara={placa?.mapaRx} />
          <span className="estado" data-listo={Boolean(placa?.mapaTx && !placa.falla)} role="status">{estado}</span>
          <button className="boton" disabled={!puedeCerrar && !puedeConectar}
            onClick={puedeCerrar ? () => void desconectar(conexion.current, '', true) : conectar}>
            {puedeCerrar ? 'Desconectar' : 'Conectar K8'}
          </button>
        </div>
      </header>
      <div className="columnas">
        <section className="panel conversacion" aria-labelledby="titulo-chat">
          <header className="panel-cabecera">
            <div>
              <h2 id="titulo-chat">Conversación</h2>
              <p>{placa ? 'Lado ' + placa.lado + ' ↔ Lado ' + (3 - placa.lado) : 'Conecta una placa por USB'}</p>
            </div>
            <span className="reloj">{placa?.reloj ? 'Reloj RX sincronizado' : 'Reloj RX en espera'}</span>
          </header>
          <div className="historial" ref={historial} role="log" aria-label="Mensajes del chat">
            {!mensajes.length && <p className="vacio">Todavía no hay mensajes.</p>}
            {mensajes.map(m => <div className={'mensaje ' + (m.propio ? 'propio' : '')} key={m.uid}>
              <article>
                <header>
                  <strong>{m.propio ? 'Tú' : 'Otra placa'}</strong>
                  <time>{m.hora.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}</time>
                </header>
                <p>{m.texto}</p>
                <small data-estado={m.estado}>{m.propio ? ETIQUETAS_ENVIO[m.estado] + ' · ' : ''}{m.bytes} B</small>
              </article>
            </div>)}
          </div>
          <form className="compositor" onSubmit={enviar}>
            <div className="opciones">
              <label>Codificación
                <select value={formato} onChange={evt => setFormato(evt.target.value)}>
                  <option value="utf8">UTF-8</option>
                  <option value="latin1">ASCII extendido</option>
                </select>
              </label>
              <label>CRC-8
                <select value={crc} onChange={evt => setCrc(Number(evt.target.value))}>
                  {CRC.map((variante, indice) => <option key={variante} value={indice}>{variante}</option>)}
                </select>
              </label>
              <output>{preparado?.bytes ?? 0} / {MAX_BYTES} B</output>
            </div>
            <label className="campo">Mensaje
              <textarea value={texto} onChange={evt => setTexto(evt.target.value)} rows="2"
                placeholder="Escribe un mensaje"
                onKeyDown={evt => {
                  if (evt.key === 'Enter' && !evt.shiftKey && !evt.nativeEvent.isComposing) {
                    evt.preventDefault()
                    evt.currentTarget.form.requestSubmit()
                  }
                }} />
            </label>
            <div className="acciones">
              <span className="texto-error" role="status">{aviso || invalido ||
                (placa?.falla ? (ERRORES[placa.falla] || 'Fallo de enlace') + '. Reinicia ambas placas.' : '')}</span>
              <button type="submit" className="boton" disabled={!habilitado}>Enviar</button>
            </div>
          </form>
        </section>
        <Registro logs={logs} placa={placa} />
      </div>
    </main>
  )
}

createRoot(document.getElementById('root')).render(<App />)
