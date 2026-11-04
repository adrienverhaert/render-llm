// ASSISTANT/src/protocol.ts
//
// File-based protocol client for Data/assistant/. This is the ONLY module
// that touches disk under that directory. See SPEC_ASSISTANT_RONDA_A.md
// section 3 ("EL PROTOCOLO") for the normative contract this implements.
//
// Hard rules this file exists to enforce (do not relax these without
// re-reading section 3.2/3.3 of the spec first):
//   - Everything we write is UTF-8 without BOM, using "\n" (LF).
//   - "One-piece" files (ops_req.json, brain.json) are written to
//     "<file>.part" and then renamed on top -- never written in place.
//   - Queues (chat_in.jsonl / chat_out.jsonl) use claim-by-rename: delete
//     the flag BEFORE renaming the .jsonl to .take, so there is never a
//     window where a producer's write can be silently lost.
//   - We never read or write anything outside of `dir`, except that callers
//     (index.ts) separately read prompts/role.md and the model separately
//     reads PNGs under `dir/preview/` via the built-in Read tool.

import fs from 'node:fs';
import path from 'node:path';

// ---------------------------------------------------------------------------
// Wire types (section 3.4 / 3.5 / 3.7 / 3.6)
// ---------------------------------------------------------------------------

export interface InHello {
  seq: number;
  t: number;
  type: 'hello';
  editor_version?: string;
  project?: string;
}
export interface InUser {
  seq: number;
  t: number;
  type: 'user';
  turn_id: string;
  text: string;
  /** RONDA E: ruta absoluta del .miproject abierto, "" si nunca se ha guardado. */
  project_file?: string;
  /**
   * RONDA G: nombres de fichero (no rutas) dentro de
   * `Data/assistant/attachments/<turn_id>/`, separados por `;`. Cadena vacia
   * = sin adjuntos.
   */
  attachments?: string;
}
export interface InStop {
  seq: number;
  t: number;
  type: 'stop';
  turn_id: string;
}
export interface InModel {
  seq: number;
  t: number;
  type: 'model';
  model: string;
}
/**
 * RONDA UI (F2): nivel de razonamiento elegido por el USUARIO en el composer.
 * Viaja por el protocolo como el modelo -- nunca es una capacidad del modelo.
 */
export interface InEffort {
  seq: number;
  t: number;
  type: 'effort';
  effort: string;
}
/**
 * RONDA UI (F3): el usuario creo o cambio de chat. Cada chat tiene su PROPIA
 * sesion del SDK (chats/<id>/session.json), asi que el contexto no se mezcla.
 */
export interface InChat {
  seq: number;
  t: number;
  type: 'chat';
  action: 'new' | 'switch';
  chat_id: string;
}
export type InRecord = InHello | InUser | InStop | InModel | InEffort | InChat;

export type OutRecordInput =
  | { type: 'status'; state: 'ready' | 'thinking' | 'acting' | 'idle'; model?: string; turn_id?: string }
  | { type: 'token'; turn_id: string; text: string }
  | { type: 'text'; turn_id: string; text: string }
  | {
      type: 'tool';
      turn_id: string;
      name: string;
      phase: 'start' | 'end';
      ok?: boolean;
      ms?: number;
      summary?: string;
    }
  | { type: 'preview'; turn_id: string; path: string; w: number; h: number; kind?: 'preview' | 'quality' }
  | {
      type: 'job';
      turn_id: string;
      job_id: string;
      phase: 'snapshot' | 'instance' | 'copy' | 'render' | 'done' | 'failed';
      detail?: string;
      ms?: number;
    }
  | { type: 'error'; turn_id?: string; message: string }
  | {
      /**
       * RONDA UI (F8/F9): hermano de 'error' pero con semantica de "el editor
       * sabe dibujar esto como una tarjeta". NUNCA lleva texto en ingles a
       * pantalla: el editor decide la frase con text_get. El `text` viaja solo
       * para assistant.log y para el volcado de diagnostico.
       */
      type: 'notice';
      turn_id?: string;
      kind: 'auth_retry' | 'auth_failed' | 'context_full' | 'compacted' | 'compacting';
      text?: string;
    }
  | { type: 'done'; turn_id: string; reason: string; cost_usd?: number; ms?: number }
  | {
      type: 'memory';
      turn_id: string;
      action: 'saved' | 'forgotten' | 'pruned' | 'error';
      scope?: 'global' | 'project';
      id?: string;
      /** Texto de la entrada (saved) o de la que se olvido (forgotten). */
      text?: string;
      /** SOLO action:'pruned': ids descartados, separados por ";" (convencion del proyecto). */
      ids?: string;
      /** SOLO action:'pruned': cuantos. */
      n?: number;
      /** SOLO action:'error': por que no se pudo guardar/olvidar. */
      message?: string;
    }
  | {
      /**
       * RONDA K0: autoreporte de huecos (mi3_report_gap -> gap_log.jsonl).
       * Discreto a proposito -- la pestana puede mostrar un chip ("registrado:
       * no puedo X") sin que el usuario tenga que hacer nada.
       */
      type: 'gap';
      turn_id: string;
      want: string;
      /** Veces que se ha visto esta misma peticion (tras dedupe), incluyendo esta. */
      n: number;
    };

export interface Op {
  id: string;
  op: string;
  args?: Record<string, unknown>;
}

export interface OpResultEntry {
  id: string;
  op: string;
  ok: 0 | 1;
  error?: string;
  // Op-specific fields (scene_summary's `objects`, camera_preview's `out`, ...).
  [key: string]: unknown;
}

export interface OpsCheckpointInfo {
  created: 0 | 1;
  cp_id?: string;
  name?: string;
  error?: string;
}

export interface OpsRes {
  seq: number;
  turn_id: string;
  ok?: number;
  ms: number;
  ops_total: number;
  ops_ok: number;
  checkpoint?: OpsCheckpointInfo;
  results: OpResultEntry[];
  // Set by the editor when the whole batch was rejected (e.g. stale turn).
  error?: string;
}

interface OpsReqWire {
  seq: number;
  turn_id: string;
  timeout_ms: number;
  ops: Op[];
}

export interface BrainBeat {
  beat: number;
  pid: number;
  state: string;
  model: string;
  session_id: string | null;
  started_at: number;
}

/** RONDA D (robustez): forma minima de brain.json que nos hace falta para detectar un cerebro ya vivo. */
export interface ExistingBrain {
  pid: number;
  state: string;
}

/**
 * RONDA D (robustez): lee brain.json SIN pasar por la clase Protocol (cuyo
 * constructor limpia el directorio entero asumiendo que es la UNICA sesion
 * de proceso). index.ts llama a esto ANTES de construir Protocol, para que
 * un segundo lanzamiento accidental (doble clic en "Launch assistant" justo
 * en el borde de la ventana de heartbeat, p.ej.) no le arranque a un cerebro
 * YA vivo sus chat_in/chat_out/ops_req/ops_res a mitad de un turno real.
 * Nunca lanza: brain.json ausente/corrupto simplemente cuenta como "no hay
 * cerebro previo".
 */
export function readExistingBrain(dir: string): ExistingBrain | undefined {
  try {
    const raw = stripBom(fs.readFileSync(path.join(path.resolve(dir), 'brain.json'), 'utf8'));
    const obj = JSON.parse(raw) as { pid?: unknown; state?: unknown };
    if (typeof obj.pid === 'number' && Number.isFinite(obj.pid) && obj.pid > 0) {
      return { pid: obj.pid, state: typeof obj.state === 'string' ? obj.state : '' };
    }
    return undefined;
  } catch {
    return undefined;
  }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

export class OpsTimeoutError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'OpsTimeoutError';
  }
}

export class OpsAbortedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'OpsAbortedError';
  }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/** Sleep `ms` milliseconds. Resolves early (never rejects) if `signal` aborts. */
export function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    if (signal?.aborted) {
      resolve();
      return;
    }
    const t = setTimeout(resolve, ms);
    signal?.addEventListener(
      'abort',
      () => {
        clearTimeout(t);
        resolve();
      },
      { once: true },
    );
  });
}

function stripBom(s: string): string {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s;
}

/**
 * RONDA UI (F3): id del chat activo segun chats/current.txt (lo escribe el
 * editor). undefined si el fichero no existe todavia -- ese es el caso del
 * arnes fake-editor.mjs y el de un Data/assistant que aun no se ha migrado.
 * Nunca lanza.
 */
export function readCurrentChat(dir: string): string | undefined {
  try {
    const raw = stripBom(fs.readFileSync(path.join(dir, 'chats', 'current.txt'), 'utf8')).trim();
    const safe = raw.replace(/[^A-Za-z0-9_-]/g, '_');
    return safe.length > 0 ? safe : undefined;
  } catch {
    return undefined;
  }
}

/**
 * Espera SINCRONA de pocos ms (sin quemar CPU). Solo se usa entre reintentos de
 * un rename que ha chocado con el editor: el punto de estas escrituras es que
 * sean sincronas y atomicas, asi que no se puede ceder el event loop a mitad.
 */
function sleepSync(ms: number): void {
  try {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
  } catch {
    // SharedArrayBuffer deshabilitado: el reintento inmediato tambien vale.
  }
}

/**
 * Replace C0 control characters that JSON.stringify would otherwise have to
 * emit as a "\uXXXX" escape (they have no short escape like \n/\r/\t). The
 * hand-rolled GML-side line parser only unescapes "\n", "\"" and "\\", so we
 * never want the string form to contain a bare \u escape (see spec section
 * 3.2 / 4.6). Newlines, carriage returns and tabs keep their normal short
 * JSON escapes and are left untouched.
 *
 * Implemented with charCodeAt comparisons rather than a regex literal
 * containing raw control-character escapes, so the intent stays legible
 * and there is no escaping ambiguity to get wrong.
 */
function cleanControlChars(s: string): string {
  let out = '';
  let changed = false;
  for (let i = 0; i < s.length; i++) {
    const code = s.charCodeAt(i);
    const isKeepable = code === 0x09 || code === 0x0a || code === 0x0d; // \t \n \r
    const isProblemControl = code < 0x20 && !isKeepable;
    if (isProblemControl) {
      out += ' ';
      changed = true;
    } else {
      out += s[i];
    }
  }
  return changed ? out : s;
}

function sanitizeForProtocol(value: unknown): unknown {
  if (typeof value === 'string') return cleanControlChars(value);
  if (Array.isArray(value)) return value.map(sanitizeForProtocol);
  if (value && typeof value === 'object') {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) out[k] = sanitizeForProtocol(v);
    return out;
  }
  return value;
}

const U_ESCAPE_RE = /\\u[0-9a-fA-F]{4}/;

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

export interface ProtocolOptions {
  /** Use only the serialized ops channel; do not own chat queues, previews or brain heartbeat. */
  actionOnly?: boolean;
}

export class Protocol {
  readonly dir: string;
  private readonly actionOnly: boolean;

  private readonly chatInPath: string;
  private readonly chatInFlagPath: string;
  private readonly chatInTakePath: string;
  private readonly chatOutPath: string;
  private readonly chatOutFlagPath: string;
  private readonly opsReqPath: string;
  private readonly opsResPath: string;
  private readonly brainPath: string;
  // RONDA UI (F3): deja de ser `readonly` -- setChat() lo repunta a
  // chats/<id>/session.json cuando el usuario cambia de chat.
  private sessionPath: string;
  private readonly logPath: string;
  private readonly previewDir: string;

  private seqOut = 0;
  private lastSeqIn = 0;
  private opSeq = 0;
  private opMutex: Promise<unknown> = Promise.resolve();

  private inCallback: ((rec: InRecord) => void) | undefined;
  private inTimer: ReturnType<typeof setInterval> | undefined;
  private inPolling = false;

  private beatTimer: ReturnType<typeof setInterval> | undefined;
  private beatN = 0;
  private readonly startedAt = Date.now();
  private state = 'starting';
  private model = '';
  private sessionId: string | null = null;

  constructor(dir: string, options: ProtocolOptions = {}) {
    this.dir = path.resolve(dir);
    this.actionOnly = options.actionOnly === true;
    this.chatInPath = path.join(this.dir, 'chat_in.jsonl');
    this.chatInFlagPath = path.join(this.dir, 'chat_in.flag');
    this.chatInTakePath = path.join(this.dir, 'chat_in.take');
    this.chatOutPath = path.join(this.dir, 'chat_out.jsonl');
    this.chatOutFlagPath = path.join(this.dir, 'chat_out.flag');
    this.opsReqPath = path.join(this.dir, 'ops_req.json');
    this.opsResPath = path.join(this.dir, 'ops_res.json');
    this.brainPath = path.join(this.dir, 'brain.json');
    // RONDA D (persistencia): session_id del SDK, para "resume" entre
    // RELANZAMIENTOS del proceso (no solo turnos dentro de uno). A
    // diferencia de brain.json/chat_*/ops_* (de UNA sesion de proceso, ver
    // el borrado mas abajo) este fichero NUNCA se borra al arrancar -- ese
    // es el punto: sobrevivir a cerrar y reabrir el editor.
    this.sessionPath = path.join(this.dir, 'session.json');
    this.logPath = path.join(this.dir, 'assistant.log');
    this.previewDir = path.join(this.dir, 'preview');

    // RONDA UI (F3): si el editor ya migro la conversacion a chats/, arrancar
    // apuntando al chat ACTIVO. Si chats/current.txt no existe se mantiene la
    // ruta antigua -- eso es lo que deja funcionando al arnes fake-editor.mjs,
    // que no conoce chats.
    const currentChat = readCurrentChat(this.dir);
    if (currentChat) this.sessionPath = path.join(this.dir, 'chats', currentChat, 'session.json');

    fs.mkdirSync(this.dir, { recursive: true });

    // RONDA E: memory/ NO se toca aqui, NUNCA. Es del USUARIO (la edita a mano)
    // y su razon de ser es sobrevivir a cerrar el editor -- igual que
    // session.json y history.jsonl, y al reves que preview/ y render_job/.

    if (!this.actionOnly) {
      // Clean stale preview images from a previous session (section 5.2).
      fs.rmSync(this.previewDir, { recursive: true, force: true });
      fs.mkdirSync(this.previewDir, { recursive: true });

      // RONDA B: the native assistant owns session render jobs. Action-only
      // clients must never delete them merely by connecting over MCP.
      fs.rmSync(path.join(this.dir, 'render_job'), { recursive: true, force: true });
      fs.mkdirSync(path.join(this.dir, 'render_job'), { recursive: true });
    }

    // RONDA A2 (verificacion, gate d): lo mismo para los ops_res_<seq>.json del
    // camino alternativo. El editor los barre en SU arranque, pero si solo se
    // relanza el cerebro (el editor sigue vivo) opSeq vuelve a 1 y un fichero
    // viejo con ese mismo numero se leeria como respuesta valida: runOpsInner no
    // valida resp.seq en el camino alternativo, precisamente porque asume que
    // nadie mas pudo escribir ese nombre durante esta sesion. Este barrido es lo
    // que sostiene esa suposicion.
    for (const f of fs.readdirSync(this.dir)) {
      if (/^ops_res_\d+\.json$/.test(f)) this.safeUnlink(path.join(this.dir, f));
    }

    this.log('protocol iniciado, dir=' + this.dir);

    // Heartbeat: rewritten every 2s regardless of state changes (section 3.6).
    // Deliberately NOT unref()'d: this is a long-running daemon whose main
    // loop otherwise just awaits a queue that may never resolve, and an
    // unref'd timer would let Node consider the event loop empty and exit.
    if (!this.actionOnly) this.beatTimer = setInterval(() => this.writeBeat(), 2000);
  }

  // -------------------------------------------------------------------
  // chat_out (producer) -- section 3.5
  // -------------------------------------------------------------------

  emit(rec: OutRecordInput): void {
    if (this.actionOnly) return;
    const withMeta = { seq: ++this.seqOut, t: Date.now(), ...rec };
    const clean = sanitizeForProtocol(withMeta);
    const line = JSON.stringify(clean);
    if (U_ESCAPE_RE.test(line)) {
      // Should be unreachable after sanitizeForProtocol; kept as a safety net.
      this.log('WARN: linea de chat_out contenia un escape \\u tras sanear: ' + line.slice(0, 160));
    }
    try {
      fs.appendFileSync(this.chatOutPath, line + '\n', { encoding: 'utf8' });
      this.touch(this.chatOutFlagPath);
    } catch (e) {
      this.log('ERROR escribiendo chat_out: ' + String(e));
    }
  }

  // -------------------------------------------------------------------
  // chat_in (consumer) -- section 3.3 / 3.4, claim by rename
  // -------------------------------------------------------------------

  onIn(cb: (rec: InRecord) => void): void {
    this.inCallback = cb;
    if (this.inTimer) return;
    this.inTimer = setInterval(() => this.pollIn(), 150);
    // Pick up anything already waiting instead of sitting idle for up to 150ms.
    this.pollIn();
  }

  private pollIn(): void {
    if (this.inPolling) return;
    this.inPolling = true;
    try {
      if (!fs.existsSync(this.chatInFlagPath)) return;
      // The flag must be deleted BEFORE the rename attempt (section 3.3).
      this.safeUnlink(this.chatInFlagPath);
      try {
        fs.renameSync(this.chatInPath, this.chatInTakePath);
      } catch {
        // Producer likely still had the file open, or nothing to claim yet.
        // Recreate the flag so the next tick retries; no window of loss.
        this.touch(this.chatInFlagPath);
        return;
      }
      let raw: string;
      try {
        raw = stripBom(fs.readFileSync(this.chatInTakePath, 'utf8'));
      } finally {
        this.safeUnlink(this.chatInTakePath);
      }
      for (const rawLine of raw.split('\n')) {
        const line = rawLine.replace(/\r$/, '').trim();
        if (!line) continue;
        let rec: InRecord;
        try {
          rec = JSON.parse(line) as InRecord;
        } catch {
          this.log('WARN linea invalida en chat_in: ' + line.slice(0, 200));
          continue;
        }
        if (typeof rec.seq === 'number') {
          // "hello" marks a NEW editor session, and the editor's seq counter
          // (mi3_as_seq_in) restarts at 1 on every launch. Without this reset
          // a brain that outlives an editor restart -- the normal case, since
          // it is spawned detached -- would silently drop every message of the
          // new session as "already seen".
          if (rec.type === 'hello') {
            this.lastSeqIn = rec.seq;
          } else if (rec.seq !== 0) {
            if (rec.seq <= this.lastSeqIn) continue; // already seen
            this.lastSeqIn = rec.seq;
          }
        }
        try {
          this.inCallback?.(rec);
        } catch (e) {
          this.log('ERROR en callback de chat_in: ' + String(e));
        }
      }
    } finally {
      this.inPolling = false;
    }
  }

  // -------------------------------------------------------------------
  // ops_req.json / ops_res.json -- section 3.7
  // -------------------------------------------------------------------

  async runOps(ops: Op[], turnId: string, timeoutMs = 15000, signal?: AbortSignal): Promise<OpsRes> {
    const run = () => this.runOpsInner(ops, turnId, timeoutMs, signal);
    const chained = this.opMutex.then(run, run);
    this.opMutex = chained.then(
      () => undefined,
      () => undefined,
    );
    return chained;
  }

  private async runOpsInner(ops: Op[], turnId: string, timeoutMs: number, signal?: AbortSignal): Promise<OpsRes> {
    if (signal?.aborted) throw new OpsAbortedError(`turno ${turnId} abortado antes de enviar ops`);

    const seq = ++this.opSeq;
    const req: OpsReqWire = { seq, turn_id: turnId, timeout_ms: timeoutMs, ops };
    this.atomicWriteJson(this.opsReqPath, req);
    // RONDA A2 (verificacion, gate d): nombre alternativo que el editor usa
    // cuando 25 intentos de delete+rename sobre ops_res.json fallan por
    // contienda de archivo (nosotros mismos con el fs.readFileSync de abajo
    // somos la causa mas comun -- ver mi3_assistant_publish_pending en
    // mi3_assistant.gml). Sondearlo aqui es lo que evita que esa respuesta se
    // pierda en silencio: NUNCA lo escribe nadie mas que ese fallback, asi que
    // no hace falta comprobar resp.seq como con opsResPath (que puede traer el
    // eco de una peticion abandonada).
    const fallbackPath = path.join(this.dir, `ops_res_${seq}.json`);

    // RESCATE K5: un ops_res.json ILEGIBLE (JSON invalido) se trataba como una
    // lectura rota y se reintentaba en silencio hasta agotar el timeout. Eso
    // convirtio un bug de una linea del editor (una ruta de Windows sin escapar
    // en assistant.paths, ver mi3_hl_json_save_var en mi3_headless.gml) en
    // "el editor no respondio", y costo una oleada entera de diagnostico
    // equivocado. Un fichero corrupto ESTABLE (mismo tamano y mismo error dos
    // sondeos seguidos, 80 ms) ya NO es una lectura rota: se registra en la
    // bitacora con el motivo y el trozo exacto, y el mensaje de timeout lo
    // repite. Nunca se descarta la respuesta ni se cambia el flujo: solo se
    // deja de mentir sobre la causa.
    let parseFails = 0;
    let lastParseErr = '';
    let lastParseSize = -1;
    let parseReported = false;

    const deadline = Date.now() + timeoutMs;
    for (;;) {
      if (signal?.aborted) {
        this.safeUnlink(this.opsReqPath);
        this.safeUnlink(fallbackPath);
        throw new OpsAbortedError(`turno ${turnId} abortado esperando ops_res (seq=${seq})`);
      }
      if (Date.now() > deadline) {
        this.safeUnlink(this.opsReqPath);
        this.safeUnlink(fallbackPath);
        const why =
          parseFails > 0
            ? ` (ops_res ILEGIBLE ${parseFails} veces, ultimo error: ${lastParseErr})`
            : '';
        this.emit({
          type: 'error',
          turn_id: turnId,
          message: parseFails > 0 ? 'el editor respondio con un JSON ilegible' : 'el editor no respondio (timeout)',
        });
        throw new OpsTimeoutError(`ops_req seq=${seq} turno=${turnId} timeout tras ${timeoutMs}ms${why}`);
      }

      await sleep(40, signal);
      if (signal?.aborted) continue; // loop head throws with the right message

      const usingFallback = !fs.existsSync(this.opsResPath) && fs.existsSync(fallbackPath);
      const sourcePath = usingFallback ? fallbackPath : this.opsResPath;
      if (!usingFallback && !fs.existsSync(this.opsResPath)) continue;

      let resp: OpsRes;
      try {
        resp = JSON.parse(stripBom(fs.readFileSync(sourcePath, 'utf8'))) as OpsRes;
      } catch (e) {
        // Torn read should not happen given atomic rename on the editor side,
        // but do not crash the brain over it -- just wait for the next tick.
        // RESCATE K5: si el fichero es ESTABLE y sigue sin parsear, no es una
        // lectura rota; dejar constancia (una sola vez por peticion).
        const msg = e instanceof Error ? e.message : String(e);
        let size = -1;
        let raw = '';
        try {
          raw = stripBom(fs.readFileSync(sourcePath, 'utf8'));
          size = raw.length;
        } catch {
          /* desaparecio entre medias: eso SI es una carrera normal */
        }
        if (parseFails > 0 && !parseReported && size >= 0 && size === lastParseSize && msg === lastParseErr) {
          parseReported = true;
          const m = /position (\d+)/.exec(msg);
          const at = m ? Number(m[1]) : -1;
          const snippet = at >= 0 ? raw.slice(Math.max(0, at - 40), at + 40).replace(/\s+/g, ' ') : raw.slice(0, 80);
          this.log(
            `ERROR ops_res seq=${seq} ILEGIBLE y estable (${size} bytes): ${msg} | ...${snippet}... ` +
              '-- el editor escribio JSON invalido (tipicamente una cadena sin escapar); la respuesta NO se puede usar',
          );
        }
        parseFails++;
        lastParseErr = msg;
        lastParseSize = size;
        continue;
      }

      if (!usingFallback && resp.seq !== seq) {
        // Echo of an abandoned request; discard and keep waiting (section 3.7).
        this.safeUnlink(this.opsResPath);
        continue;
      }

      this.safeUnlink(sourcePath);
      return resp;
    }
  }

  // -------------------------------------------------------------------
  // brain.json heartbeat -- section 3.6
  // -------------------------------------------------------------------

  beat(state: string, extra?: { model?: string; session_id?: string }): void {
    this.state = state;
    if (extra?.model !== undefined) this.model = extra.model;
    if (extra?.session_id !== undefined) this.sessionId = extra.session_id;
    this.writeBeat();
  }

  private writeBeat(): void {
    this.beatN++;
    const obj: BrainBeat = {
      beat: this.beatN,
      pid: process.pid,
      state: this.state,
      model: this.model,
      session_id: this.sessionId,
      started_at: this.startedAt,
    };
    // RONDA B (verificacion, gate B2): este metodo lo llama un setInterval. Una
    // excepcion aqui NO la ve nadie: sube como uncaught y MATA el proceso del
    // cerebro entero a mitad de turno (medido: EPERM en el rename de
    // brain.json.part mientras el editor lo leia; el render en curso se quedo
    // huerfano y el turno nunca termino). El latido es una pista de vitalidad,
    // no un dato critico: el editor ya tolera un brain.json rancio (~5 s).
    try {
      this.atomicWriteJson(this.brainPath, obj);
    } catch (e) {
      this.log(`WARN latido no escrito (se reintenta en el proximo): ${String(e)}`);
    }
  }

  // -------------------------------------------------------------------
  // session.json -- RONDA D (persistencia)
  // -------------------------------------------------------------------

  /** Ultimo session_id persistido por un proceso anterior, si lo hay. Nunca lanza. */
  readSessionId(): string | undefined {
    try {
      const raw = stripBom(fs.readFileSync(this.sessionPath, 'utf8'));
      const obj = JSON.parse(raw) as { session_id?: unknown };
      return typeof obj.session_id === 'string' && obj.session_id.length > 0 ? obj.session_id : undefined;
    } catch {
      return undefined;
    }
  }

  /**
   * RONDA UI (F3): repunta session.json al chat `chatId`. El PROCESO no se
   * reinicia al cambiar de chat: lo unico que cambia es de donde se lee y
   * donde se escribe el session_id del SDK.
   * `chatId` se normaliza igual que en el editor (solo [A-Za-z0-9_-]) para
   * que un id inventado no pueda salirse de chats/.
   */
  setChat(chatId: string): void {
    const safe = chatId.replace(/[^A-Za-z0-9_-]/g, '_');
    if (safe === '') return;
    const chatDir = path.join(this.dir, 'chats', safe);
    try {
      fs.mkdirSync(chatDir, { recursive: true });
    } catch {
      // Si no se puede crear, readSessionId devolvera undefined y el chat
      // arranca sin continuidad: degradado, no roto.
    }
    this.sessionPath = path.join(chatDir, 'session.json');
  }

  /** Persiste el session_id actual (sobreescribe el anterior; escritura atomica). */
  writeSessionId(sessionId: string): void {
    try {
      this.atomicWriteJson(this.sessionPath, { session_id: sessionId });
    } catch (e) {
      // No critico: en el peor caso el siguiente proceso arranca sin continuidad.
      this.log(`WARN no se pudo persistir session_id: ${String(e)}`);
    }
  }

  // -------------------------------------------------------------------
  // Diagnostics + lifecycle
  // -------------------------------------------------------------------

  log(msg: string): void {
    try {
      const line = `[${new Date().toISOString()}] ${msg}\n`;
      fs.appendFileSync(this.logPath, line, { encoding: 'utf8' });
    } catch {
      // assistant.log is a diagnostic convenience; never let it crash the app.
    }
  }

  close(): void {
    if (this.inTimer) clearInterval(this.inTimer);
    if (this.beatTimer) clearInterval(this.beatTimer);
    this.inTimer = undefined;
    this.beatTimer = undefined;
  }

  // -------------------------------------------------------------------
  // Internal file helpers
  // -------------------------------------------------------------------

  private atomicWriteJson(finalPath: string, obj: unknown): void {
    const tmp = finalPath + '.part';
    const line = JSON.stringify(sanitizeForProtocol(obj));
    fs.writeFileSync(tmp, line, { encoding: 'utf8' });
    // Windows no permite renombrar SOBRE un fichero que otro proceso tiene
    // abierto: el editor lee brain.json a 1 Hz y ops_res/ops_req en su tick, y
    // el rename revienta con EPERM/EACCES/EBUSY. Es la misma contienda que la
    // ronda A2 arreglo del lado del editor (mi3_assistant_publish_pending, 25
    // intentos + nombre alternativo); aqui faltaba el lado del cerebro.
    // Reintentar deja la escritura igual de atomica (el rename sigue siendo el
    // unico punto de publicacion) y evita perder el fichero entero por una
    // ventana de milisegundos.
    let lastErr: unknown;
    for (let i = 0; i < 12; i++) {
      try {
        fs.renameSync(tmp, finalPath);
        return;
      } catch (e) {
        lastErr = e;
        const code = (e as NodeJS.ErrnoException).code;
        if (code !== 'EPERM' && code !== 'EACCES' && code !== 'EBUSY') break;
        sleepSync(20);
      }
    }
    throw lastErr;
  }

  private touch(p: string): void {
    fs.writeFileSync(p, '', { encoding: 'utf8' });
  }

  private safeUnlink(p: string): void {
    try {
      fs.unlinkSync(p);
    } catch {
      // Already gone, or a transient lock -- either way, not fatal here.
    }
  }
}
