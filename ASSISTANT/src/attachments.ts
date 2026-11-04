// ASSISTANT/src/attachments.ts
//
// RONDA G: imagenes que el USUARIO adjunta desde el dialogo de archivo nativo
// del editor (nunca elegidas por el modelo -- ver el invariante de seguridad
// en SPEC_ASSISTANT_RONDA_G.md S3.0). Este modulo hace DOS cosas y nada mas:
//
//   - readAttachments: convierte los nombres de fichero que trae chat_in
//     ("attachments" en InUser, protocol.ts) en bloques de imagen base64
//     para el mensaje del turno, con defensa de profundidad contra
//     traversal aunque el editor solo escriba nombres pelados ("imgN.png").
//   - pruneAttachments: poda las carpetas de turno viejas de attachments/
//     (nunca "_staging/", que es del editor -- ver mi3_assistant_attach.gml
//     -- y nunca la carpeta del turno recien usado, que siempre es la mas
//     reciente por mtime porque la poda corre DESPUES del turno).
//
// NUNCA se anade 'attachments/' a la jaula de Read de canUseTool (index.ts):
// las imagenes viajan como CONTENIDO DEL MENSAJE, no como algo que el
// modelo pueda pedir, listar o leer por su cuenta. Hay un test que lo
// asegura (run-tests.mjs, "RONDA G: la jaula...").

import fs from 'node:fs';
import path from 'node:path';

/**
 * Forma EXACTA del bloque de imagen del SDK instalado
 * (@anthropic-ai/sdk@0.93.0, resources/messages/messages.d.ts):
 * `ImageBlockParam.source` es `Base64ImageSource | URLImageSource`, y
 * `Base64ImageSource = { data: string; media_type: 'image/jpeg'|'image/png'|
 * 'image/gif'|'image/webp'; type: 'base64' }`. Toda imagen se re-codifica a
 * PNG del lado del editor (S3.1 del spec de la ronda G), asi que aqui
 * `media_type` es SIEMPRE 'image/png' -- una constante, no la union completa
 * que la API admite. Ver README.md para el porque de esa decision.
 */
export interface ImageBlock {
  type: 'image';
  source: { type: 'base64'; media_type: 'image/png'; data: string };
}

// Decisiones S3.1 de SPEC_ASSISTANT_RONDA_G.md (con su porque documentado en
// el README de ASSISTANT): tope de imagenes por turno, tope duro de bytes
// por fichero, y la poda de attachments/ (carpetas de turno, nunca _staging).
export const MAX_ATTACHMENTS = 4;
export const MAX_BYTES = 5 * 1024 * 1024;
export const KEEP_TURNS = 10;
export const KEEP_DAYS = 7;

/**
 * MISMA normalizacion que `mi3_assistant_turn_safe` (GML,
 * mi3_assistant_attach.gml) y que `ctx.turnId.replace(...)` en tools.ts: un
 * turn_id nunca debe poder escapar del directorio
 * `attachments/<turn_id_seguro>/` que le corresponde.
 */
export function safeTurnId(turnId: string): string {
  return turnId.replace(/[^A-Za-z0-9_-]/g, '_');
}

/**
 * Lee los adjuntos de UN turno a partir del campo `attachments` de chat_in
 * (nombres de fichero pelados, separados por ";"). Nunca lanza (regla 7 del
 * spec): cualquier fallo por entrada -- nombre invalido, traversal, fichero
 * ausente, demasiado grande -- se convierte en una nota de texto en vez de
 * una excepcion, y esa entrada simplemente no produce un bloque de imagen.
 */
export function readAttachments(
  dataDir: string,
  turnId: string,
  list: string,
  log: (m: string) => void,
): { blocks: ImageBlock[]; notes: string[] } {
  const blocks: ImageBlock[] = [];
  const notes: string[] = [];

  const names = list
    .split(';')
    .map((s) => s.trim())
    .filter((s) => s.length > 0);

  const wanted = names.slice(0, MAX_ATTACHMENTS);
  const over = names.length - wanted.length;
  if (over > 0) {
    notes.push(
      `RONDA G: el turno traia ${names.length} adjuntos; solo se usan los primeros ${MAX_ATTACHMENTS} (${over} ignorados).`,
    );
  }

  const turnDir = path.resolve(dataDir, 'attachments', safeTurnId(turnId));
  const turnDirWithSep = turnDir + path.sep;

  for (const name of wanted) {
    try {
      // Defensa de profundidad (S3.3.2 punto 2 del spec): el editor solo
      // escribe nombres pelados tipo "imgN.png", pero este modulo no confia
      // en el productor -- rechaza cualquier cosa que huela a ruta.
      if (name.includes('/') || name.includes('\\') || name.includes(':') || name.includes('..')) {
        notes.push(`RONDA G: adjunto rechazado (nombre invalido): ${name}`);
        continue;
      }

      const full = path.resolve(turnDir, name);
      if (!full.startsWith(turnDirWithSep)) {
        notes.push(`RONDA G: adjunto rechazado (fuera de la carpeta del turno): ${name}`);
        continue;
      }

      const st = fs.statSync(full);
      if (!st.isFile()) {
        notes.push(`RONDA G: adjunto rechazado (no es un fichero): ${name}`);
        continue;
      }
      if (st.size > MAX_BYTES) {
        notes.push(`RONDA G: adjunto rechazado (${st.size} bytes > ${MAX_BYTES}): ${name}`);
        continue;
      }

      const data = fs.readFileSync(full).toString('base64');
      // RONDA G: traza INCONDICIONAL (mismo criterio que readPngForModel en
      // tools.ts) -- es lo que verifican los gates G2.3/G2.5, no una
      // afirmacion del modelo sobre lo que vio.
      log(`RONDA G: adjunto ${name} (${st.size} bytes, ${data.length} b64)`);
      blocks.push({ type: 'image', source: { type: 'base64', media_type: 'image/png', data } });
    } catch (e) {
      // Nunca lanza: cualquier error de disco se convierte en nota.
      notes.push(`RONDA G: adjunto ${name} no se pudo leer: ${e instanceof Error ? e.message : String(e)}`);
    }
  }

  return { blocks, notes };
}

/**
 * Poda `attachments/` a las KEEP_TURNS carpetas de turno mas recientes (por
 * mtime), y borra cualquiera mas vieja que KEEP_DAYS dias aunque quepa en
 * ese tope. Nunca toca "_staging" (es del editor, ver
 * mi3_assistant_attach.gml) ni puede tocar la carpeta del turno en curso:
 * esta funcion se llama DESPUES de que el turno termino, asi que esa carpeta
 * -- si el turno tuvo adjuntos -- es siempre la mas reciente por mtime y
 * queda dentro del tope.
 */
export function pruneAttachments(dataDir: string, log: (m: string) => void): void {
  try {
    const root = path.join(path.resolve(dataDir), 'attachments');
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(root, { withFileTypes: true });
    } catch {
      return; // todavia no existe attachments/: nada que podar
    }

    const dirs = entries
      .filter((e) => e.isDirectory() && e.name !== '_staging')
      .map((e) => {
        const full = path.join(root, e.name);
        let mtimeMs = 0;
        try {
          mtimeMs = fs.statSync(full).mtimeMs;
        } catch {
          mtimeMs = 0;
        }
        return { name: e.name, full, mtimeMs };
      })
      .sort((a, b) => b.mtimeMs - a.mtimeMs); // mas reciente primero

    const cutoff = Date.now() - KEEP_DAYS * 86400e3;
    let removed = 0;
    for (let i = 0; i < dirs.length; i++) {
      const d = dirs[i];
      const tooMany = i >= KEEP_TURNS;
      const tooOld = d.mtimeMs < cutoff;
      if (!tooMany && !tooOld) continue;
      try {
        fs.rmSync(d.full, { recursive: true, force: true });
        removed++;
      } catch (e) {
        log(`RONDA G: WARN no se pudo podar ${d.full}: ${e instanceof Error ? e.message : String(e)}`);
      }
    }
    log(`RONDA G: poda de adjuntos, ${removed} carpetas borradas`);
  } catch (e) {
    log(`RONDA G: WARN poda de adjuntos fallo: ${e instanceof Error ? e.message : String(e)}`);
  }
}
