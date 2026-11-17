import React, { useCallback, useEffect, useReducer, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import {
  chatInicial, COM, COM_MAX_RESPUESTA, hexBytes, prepararEnvio, reducirChat, VARIANTES_CRC,
} from './chat.js';
import './styles.css';

const ESTADOS = {
  desconectado: 'Sin conexión',
  conectando: 'Abriendo puerto',
  verificando: 'Buscando la K8',
  escuchando: 'Escuchando por PHY',
  sincronizando: 'Sincronizando',
  listo: 'K8 lista',
  esperando: 'Esperando eco USB',
  encolado: 'En cola',
  transmitiendo: 'Transmitiendo',
  ocupado: 'K8 ocupada',
  cerrando: 'Cerrando puerto',
};

const ERRORES_ESPERA = {
  verificando: 'No llegó respuesta. Comprueba el COM, carga el firmware actualizado y deja Keil en Run.',
  esperando: 'No llegó el eco del carácter. Reconecta para consultar el estado de la K8.',
};

const comEncoder = new TextEncoder();

async function escribirMensaje(puerto, mensaje, finalizar = false) {
  const escritor = puerto.writable.getWriter();
  try {
    await escritor.write(comEncoder.encode('@' + mensaje + '\n'));
    if (finalizar) await escritor.close();
  } finally {
    escritor.releaseLock();
  }
}

function Historial({ pruebas, receptor }) {
  const historial = useRef(null);
  const entradas = receptor ? pruebas.filter((prueba) => prueba.recibido !== null) : pruebas;
  useEffect(() => {
    historial.current.scrollTop = historial.current.scrollHeight;
  }, [pruebas]);

  return (
    <div className="historial" ref={historial} role="log"
      aria-label={receptor ? 'Recepciones de la K8' : 'Envíos de la página'}>
      {entradas.length === 0 && (
        <p className="vacio">{receptor ? 'Sin recepciones' : 'Sin envíos'}</p>
      )}
      {entradas.map(({ envio, recibido, resultado }, index) => {
        const caracter = receptor ? recibido : envio.texto;
        const codificacion = (envio.control & 8) ? 'UTF-8' : 'ISO-8859-1';
        const crc = VARIANTES_CRC[(envio.control >> 1) & 3];
        return (
          <div className="entrada" key={index}>
            <div className="burbuja">
              <span className="caracter">{caracter === ' ' ? 'Espacio' : caracter}</span>
              <code>{hexBytes(envio.bytes)}</code>
            </div>
            <small>{codificacion} · {crc}</small>
            {(receptor || recibido === null) && <small>{resultado}</small>}
          </div>
        );
      })}
    </div>
  );
}

const CANALES_RX = [
  { nombre: 'L1 · Audio A0', bit: 4 },
  { nombre: 'L2 · Luz A1', bit: 2 },
  { nombre: 'L3 · Luz A2', bit: 1 },
];

function Recepcion({ medicion, recibidos }) {
  const historial = useRef(null);
  useEffect(() => {
    historial.current.scrollTop = historial.current.scrollHeight;
  }, [recibidos]);

  const reloj = medicion?.sincronizado
    ? 'Sincronizado'
    : (medicion?.ancho ? 'Sin sincronía' : 'Esperando pulsos');

  return (
    <div className="conversacion">
      <section className="panel panel-medicion" aria-labelledby="titulo-senales">
        <div className="titulo-panel">
          <h2 id="titulo-senales">Señales recibidas</h2>
          <span>Flanco de bajada</span>
        </div>
        <div className="medicion">
          <p className="reloj-rx">{reloj}</p>
          <dl className="lecturas">
            <div><dt>Período</dt><dd>{medicion?.periodo ? medicion.periodo + ' ms' : 'Pendiente'}</dd></div>
            <div><dt>Ancho en alto</dt><dd>{medicion?.ancho ? medicion.ancho + ' ms' : 'Pendiente'}</dd></div>
            <div><dt>Bloques válidos</dt><dd>{medicion?.bloques ?? 0}</dd></div>
            <div><dt>Tramas válidas</dt><dd>{medicion?.tramas ?? 0}</dd></div>
            <div><dt>Errores</dt><dd>{medicion?.errores ?? 0}</dd></div>
          </dl>
          <table className="canales">
            <thead><tr><th scope="col">Canal</th><th scope="col">Muestra</th><th scope="col">Piloto</th></tr></thead>
            <tbody>
              {CANALES_RX.map(({ nombre, bit }) => (
                <tr key={bit}>
                  <th scope="row">{nombre}</th>
                  <td>{medicion?.ancho ? Number(Boolean(medicion.simbolo & bit)) : 'Pendiente'}</td>
                  <td>{!medicion || medicion.mapa === 8 ? 'Pendiente' : (medicion.mapa & bit ? 'OK' : 'Fallo')}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>
      <section className="panel receptor" aria-labelledby="titulo-mensajes">
        <div className="titulo-panel">
          <h2 id="titulo-mensajes">Mensajes recibidos</h2>
          <span>{recibidos.length === 1 ? '1 completo' : recibidos.length + ' completos'}</span>
        </div>
        <div className="historial" ref={historial} role="log" aria-label="Mensajes recibidos por PHY">
          {recibidos.length === 0 && <p className="vacio">Esperando un mensaje completo</p>}
          {recibidos.map(({ texto, bytes, control }, index) => (
            <div className="entrada" key={index}>
              <div className="burbuja burbuja-rx">
                <span className="caracter">{texto === ' ' ? 'Espacio' : texto}</span>
                <code>{hexBytes(bytes)}</code>
              </div>
              <small>{control & 8 ? 'UTF-8' : 'ISO-8859-1'}</small>
            </div>
          ))}
        </div>
      </section>
    </div>
  );
}

function App() {
  const [chat, cambiarChat] = useReducer(reducirChat, chatInicial);
  const [modo, cambiarModo] = useState('rx');
  const [texto, cambiarTexto] = useState('');
  const [codificacion, cambiarCodificacion] = useState('latin1');
  const [crc, cambiarCrc] = useState(0);
  const conexion = useRef(null);
  const envio = prepararEnvio(texto, codificacion, crc);
  const serial = navigator.serial;

  const cerrar = useCallback(async (sesion, error = '', avisar = false) => {
    if (!sesion || sesion.cerrando || conexion.current !== sesion) return;
    sesion.cerrando = true;
    cambiarChat({ tipo: 'cerrando' });
    await Promise.allSettled([sesion.lector.cancel(), sesion.lectura, sesion.escritura]);
    try {
      if (avisar && sesion.puerto.writable) {
        await escribirMensaje(sesion.puerto, COM.CERRAR, true).catch(() => {});
      }
      await sesion.puerto.close();
    } catch {
      // El puerto puede haber desaparecido por desconexión física.
    }
    conexion.current = null;
    cambiarChat({ tipo: 'fase', fase: 'desconectado', error });
  }, []);

  useEffect(() => {
    const error = ERRORES_ESPERA[chat.fase];
    if (!error) return undefined;
    const sesion = conexion.current;
    const timer = setTimeout(() => void cerrar(sesion, error), 4000);
    return () => clearTimeout(timer);
  }, [chat.fase, cerrar]);

  async function leer(sesion) {
    let mensaje = null;
    try {
      while (!sesion.cerrando) {
        const { value, done } = await sesion.lector.read();
        if (done || sesion.cerrando) break;
        for (const byte of value) {
          if (byte === 0x40) {
            mensaje = '';
          } else if (byte === 0x0a) {
            if (mensaje) cambiarChat({ tipo: 'mensaje', mensaje });
            mensaje = null;
          } else if (mensaje !== null) {
            mensaje = mensaje.length < COM_MAX_RESPUESTA ? mensaje + String.fromCharCode(byte) : null;
          }
        }
      }
    } catch {
      // La lectura puede terminar si se retira el USB.
    } finally {
      sesion.lector.releaseLock();
      if (!sesion.cerrando) void cerrar(sesion, 'Se interrumpió la conexión serie.');
    }
  }

  async function conectar() {
    cambiarChat({ tipo: 'fase', fase: 'conectando' });
    let puerto;
    let abierto = false;
    let sesion;
    try {
      puerto = await serial.requestPort();
      await puerto.open({
        baudRate: 9600,
        dataBits: 8,
        stopBits: 1,
        parity: 'none',
        flowControl: 'none',
      });
      abierto = true;
      sesion = { puerto, lector: puerto.readable.getReader(), cerrando: false };
      conexion.current = sesion;
      cambiarChat({ tipo: 'fase', fase: 'verificando' });
      sesion.lectura = leer(sesion);
      sesion.escritura = escribirMensaje(puerto, modo === 'rx' ? COM.ESCUCHAR : COM.ABRIR);
      await sesion.escritura;
    } catch (error) {
      const mensaje = error.name === 'NotFoundError'
        ? ''
        : 'No se pudo abrir el puerto. Comprueba que el COM del ST-LINK esté libre.';
      if (sesion) {
        await cerrar(sesion, mensaje);
      } else {
        if (abierto) await puerto.close().catch(() => {});
        cambiarChat({ tipo: 'fase', fase: 'desconectado', error: mensaje });
      }
    }
  }

  async function enviar(evento) {
    evento.preventDefault();
    const sesion = conexion.current;
    if (chat.fase !== 'listo' || !envio || !sesion || sesion.cerrando) return;
    cambiarChat({ tipo: 'envio', envio });
    cambiarTexto('');
    sesion.escritura = escribirMensaje(sesion.puerto, envio.comando);
    try {
      await sesion.escritura;
    } catch {
      await cerrar(sesion, 'No se pudo escribir en el puerto serie.');
    }
  }

  const cambiandoConexion = chat.fase === 'conectando' || chat.fase === 'cerrando';
  const conectado = chat.fase !== 'desconectado' && !cambiandoConexion;
  const entradaInvalida = texto !== '' && envio === null;
  const ayudaInvalida = codificacion === 'latin1'
    ? 'Usa un carácter de ISO-8859-1 o selecciona UTF-8.'
    : 'Escribe un solo carácter Unicode.';
  const ayudaEnvio = envio
    ? (envio.bytes.length === 1 ? '1 byte · 33 s por PHY' : `${envio.bytes.length} bytes · ${33 * envio.bytes.length} s por PHY`)
    : 'Un carácter por envío.';

  return (
    <main>
      <header className="cabecera">
        <h1>Chat de prueba</h1>
        <div className="conexion">
          <select className="modo" aria-label="Modo de la K8" value={modo}
            disabled={chat.fase !== 'desconectado'}
            onChange={(evento) => cambiarModo(evento.target.value)}>
            <option value="rx">Recibir por PHY</option>
            <option value="tx">Enviar por PHY</option>
          </select>
          <span className="estado" role="status" data-estado={chat.fase}>
            {ESTADOS[chat.fase]}
          </span>
          <button className="secundario" type="button" disabled={!serial || cambiandoConexion}
            onClick={() => conectado ? void cerrar(conexion.current, '', true) : void conectar()}>
            {conectado || chat.fase === 'cerrando' ? 'Desconectar' : 'Conectar K8'}
          </button>
        </div>
      </header>
      {(!serial || chat.error) && (
        <p className="error" role="alert">
          {serial ? chat.error : 'Abre la página en Chrome o Edge de escritorio desde localhost para usar el COM.'}
        </p>
      )}
      {modo === 'rx' ? <Recepcion
        medicion={chat.fase === 'escuchando' ? chat.medicion : null}
        recibidos={chat.recibidos}
      /> : <div className="conversacion">
        <section className="panel emisor" aria-labelledby="titulo-emisor">
          <div className="titulo-panel">
            <h2 id="titulo-emisor">Emisor</h2>
          </div>
          <Historial pruebas={chat.pruebas} />
          <form onSubmit={enviar}>
            <div className="configuracion">
              <label>
                Codificación
                <select value={codificacion} onChange={(evento) => cambiarCodificacion(evento.target.value)}>
                  <option value="latin1">ASCII extendido</option>
                  <option value="utf8">UTF-8</option>
                </select>
              </label>
              <label>
                CRC de trama
                <select value={crc} onChange={(evento) => cambiarCrc(Number(evento.target.value))}>
                  {VARIANTES_CRC.map((nombre, i) => <option key={nombre} value={i}>{nombre}</option>)}
                </select>
              </label>
            </div>
            <div className="entrada-form">
              <input
                type="text"
                value={texto}
                onChange={(evento) => cambiarTexto(evento.target.value)}
                aria-label="Carácter"
                aria-describedby="ayuda"
                aria-invalid={entradaInvalida}
                autoComplete="off"
                spellCheck={false}
                placeholder="Escribe un carácter"
              />
              <button type="submit" disabled={chat.fase !== 'listo' || !envio}>
                Enviar
              </button>
            </div>
            <p id="ayuda" className={entradaInvalida ? 'ayuda invalido' : 'ayuda'}>
              {entradaInvalida ? ayudaInvalida : ayudaEnvio}
            </p>
          </form>
        </section>
        <section className="panel receptor" aria-labelledby="titulo-receptor">
          <div className="titulo-panel">
            <h2 id="titulo-receptor">Receptor · K8</h2>
            <span>Eco por USB</span>
          </div>
          <Historial pruebas={chat.pruebas} receptor />
        </section>
      </div>}
    </main>
  );
}

createRoot(document.getElementById('root')).render(<App />);
