export const COM = {
  ABRIR: 'A',
  ESCUCHAR: 'R',
  CERRAR: 'C',
  SYNC: 'S',
  LISTO: 'L',
  DATOS: 'D',
  INICIO: 'T',
  FIN: 'F',
  OCUPADO: 'O',
  INVALIDO: 'E',
};

export const VARIANTES_CRC = ['SMBUS', 'AUTOSAR', 'SAE-J1850', 'MAXIM-DOW'];
export const COM_MAX_RESPUESTA = 2 + 2 * 64;
export const chatInicial = {
  fase: 'desconectado', pruebas: [], recibidos: [], medicion: null, error: '',
};

const utf8 = new TextEncoder();
const decodificadorUtf8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

function hex(valor, ancho) {
  return valor.toString(16).toUpperCase().padStart(ancho, '0');
}

export function hexBytes(bytes) {
  return Array.from(bytes, (byte) => hex(byte, 2)).join(' ');
}

export function prepararEnvio(texto, codificacion, crc) {
  const codigo = texto.codePointAt(0);
  if (codigo === undefined || texto.length !== (codigo > 0xffff ? 2 : 1)
      || (codigo >= 0xd800 && codigo <= 0xdfff)) {
    return null;
  }
  const usaUtf8 = codificacion === 'utf8';
  if (!usaUtf8 && codigo > 0xff) return null;

  const bytes = usaUtf8 ? utf8.encode(texto) : Uint8Array.of(codigo);
  const control = (usaUtf8 ? 8 : 0) | (crc << 1);
  const comando = COM.DATOS + hex(control, 1) + Array.from(bytes, (byte) => hex(byte, 2)).join('');
  return { texto, bytes, control, comando };
}

function decodificar(envio) {
  return (envio.control & 8)
    ? decodificadorUtf8.decode(envio.bytes)
    : String.fromCharCode(...envio.bytes);
}

function actualizarPrueba(chat, fase, cambios, error = '') {
  const pruebas = chat.pruebas.slice();
  pruebas[pruebas.length - 1] = { ...pruebas.at(-1), ...cambios };
  return { ...chat, fase, pruebas, error };
}

function leerMedicion(mensaje) {
  const campos = mensaje.slice(2).split(',');
  if (campos.length !== 8 || !campos.every((campo) => /^\d{1,10}$/.test(campo))) return null;
  const [periodo, ancho, simbolo, mapa, sincronizado, bloques, tramas, errores] = campos.map(Number);
  if (simbolo > 7 || mapa > 8 || sincronizado > 1) return null;
  return { periodo, ancho, simbolo, mapa, sincronizado, bloques, tramas, errores };
}

function recibirPhy(chat, mensaje) {
  if (mensaje.startsWith('P,')) {
    const medicion = leerMedicion(mensaje);
    return medicion ? { ...chat, medicion } : chat;
  }

  const registro = /^M([02468ACE])((?:[0-9A-F]{2}){1,64})$/.exec(mensaje);
  if (!registro) return chat;
  const control = Number.parseInt(registro[1], 16);
  const bytes = Uint8Array.from(registro[2].match(/../g), (byte) => Number.parseInt(byte, 16));
  try {
    const recibido = { texto: decodificar({ control, bytes }), control, bytes };
    return { ...chat, recibidos: [...chat.recibidos, recibido], error: '' };
  } catch {
    return { ...chat, error: 'La K8 entregó bytes que no forman un texto UTF-8 válido.' };
  }
}

function recibirMensaje(chat, mensaje) {
  if (chat.fase === 'verificando' && mensaje === COM.ESCUCHAR) {
    return { ...chat, fase: 'escuchando', medicion: null, recibidos: [] };
  }
  if (chat.fase === 'verificando' && mensaje === COM.SYNC) {
    return { ...chat, fase: 'sincronizando' };
  }
  if (chat.fase === 'sincronizando' && mensaje === COM.LISTO) {
    return { ...chat, fase: 'listo' };
  }
  if (chat.fase === 'escuchando') {
    return recibirPhy(chat, mensaje);
  }

  if (chat.fase === 'esperando') {
    const { envio } = chat.pruebas.at(-1);
    if (mensaje === envio.comando) {
      return actualizarPrueba(chat, 'encolado', {
        recibido: decodificar(envio),
        resultado: 'En cola para el siguiente bloque',
      });
    }
    if (mensaje === COM.OCUPADO) {
      return actualizarPrueba(chat, 'ocupado', { resultado: 'K8 ocupada' });
    }
    if (mensaje === COM.INVALIDO) {
      return actualizarPrueba(chat, 'listo', { resultado: 'Envío rechazado' },
        'La K8 devolvió un error. Si se reinició la placa, vuelve a conectar.');
    }
  }

  if (mensaje === COM.INICIO && chat.fase === 'encolado') {
    return actualizarPrueba(chat, 'transmitiendo', { resultado: 'Transmitiendo por PHY' });
  }
  if (mensaje === COM.FIN) {
    if (chat.fase === 'transmitiendo' || chat.fase === 'encolado') {
      return actualizarPrueba(chat, 'listo', { resultado: 'Emisión PHY terminada' });
    }
    if (chat.fase === 'ocupado') return { ...chat, fase: 'listo' };
  }
  return chat;
}

export function reducirChat(chat, evento) {
  switch (evento.tipo) {
    case 'fase':
      return { ...chat, fase: evento.fase, error: evento.error ?? '' };
    case 'envio':
      return {
        ...chat,
        fase: 'esperando',
        error: '',
        pruebas: [...chat.pruebas, {
          envio: evento.envio,
          recibido: null,
          resultado: 'Esperando eco USB',
        }],
      };
    case 'mensaje':
      return recibirMensaje(chat, evento.mensaje);
    case 'cerrando':
      if (['esperando', 'encolado', 'transmitiendo'].includes(chat.fase)) {
        return actualizarPrueba(chat, 'cerrando', { resultado: 'Conexión interrumpida' });
      }
      return { ...chat, fase: 'cerrando' };
    default:
      return chat;
  }
}
