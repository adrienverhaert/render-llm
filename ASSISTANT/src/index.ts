// ASSISTANT/src/index.ts
//
// Entry point and main loop. See SPEC_ASSISTANT_RONDA_A.md section 5.4.
//
//   node dist/index.js --dir "<Data/assistant abs path>" --model <id> [--fake] [--debug]
//
// Normal mode resolves an explicit provider adapter once per process and drives
// its streamed query once per user turn, translating events into chat_out. --fake
// swaps in the scripted, network-free brain from fake.ts instead (used by
// gate G1 so the protocol + live ops can be exercised without spending the
// user's API quota).

import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs, type Config, type EffortLevel } from './config.js';
import {
  getVisualAgentProvider,
  type ProviderCanUseTool,
  type ProviderMessage,
  type ProviderPermissionResult,
  type ProviderQueryFn,
  type ProviderUserMessage,
} from './providers.js';
import { Protocol, readExistingBrain, sleep, type InRecord } from './protocol.js';
import { classifyFailure, isMaxTurns, type TurnFailure } from './failure.js';
import { buildRenderLlmMcp, RENDER_LLM_TOOL_NAMES, type ActionTurnCtx } from './renderllm-tools.js';
import { MemoryStore } from './memory.js';
import { pruneAttachments, readAttachments } from './attachments.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const DISALLOWED_BUILT_INS = ['Bash', 'Write', 'Edit', 'WebSearch', 'WebFetch', 'Glob', 'Grep', 'Task'];

// RONDA D (streaming fino): repintar la pestana en cada content_block_delta
// (medido: decenas por segundo) le hacia hacer una escritura a disco y una
// copia completa del string acumulado (GML, S.O.) por CADA token. Se
// acumulan y se sueltan como mucho cada TOKEN_COALESCE_MS (~3 frames a
// 60 fps) -- o antes si el buffer crece demasiado, para que una respuesta
// muy verbosa no acumule latencia visible.
const TOKEN_COALESCE_MS = 50;
const TOKEN_COALESCE_MAX_CHARS = 800;

// RONDA UI (F8): un fallo de autenticacion puede ser transitorio. Se espera
// un poco y se reintenta UNA vez, de forma transparente para el usuario.
const AUTH_RETRY_MS = 1500;

// ---------------------------------------------------------------------------
// FIX max-turns: los DOS numeros del mecanismo de auto-continuacion, juntos y
// en un unico sitio (no hay ningun otro literal de esto en el codigo).
//
// El SDK corta un query() al llegar a `maxTurns` y devuelve un result
// `error_max_turns` (terminal_reason 'max_turns'). Una tarea de escena real
// -- mirar, posar, iluminar, renderizar y CORREGIR -- pasa de 24 turnos con
// facilidad, asi que ese corte le salia al usuario constantemente como si
// fuera un error suyo. Ahora el cerebro CONTINUA solo la misma sesion (ver
// CONTINUE_PROMPT) y el usuario solo ve el indicador de actividad seguir vivo.
//
// MAX_TURNS_PER_LEG no se sube y ya: es el tope POR TRAMO, y tenerlo bajo es
// lo que da un punto de control periodico (el Stop del usuario, el tope global
// de abajo) en vez de una sola llamada gigante e ininterrumpible.
export const MAX_TURNS_PER_LEG = 24;
// Tope global de seguridad POR MENSAJE DEL USUARIO: 5 continuaciones = hasta 6
// tramos = ~144 turnos. Al agotarse NO se continua mas: el turno se cierra con
// la tarjeta de siempre y con el coste acumulado de todos los tramos, para que
// una tarea que no converge no gaste sin techo.
//
// OJO: los topes de tools.ts (CREATE_CAP_PER_TURN y companeros) son del TURNO
// -- del mensaje del usuario -- y una continuacion NO los reinicia: el TurnCtx
// se construye una sola vez, antes del bucle. Es a proposito. Continuar sola no
// puede multiplicar por seis lo que el asistente crea, exporta o abre sin que
// el usuario haya vuelto a pedir nada.
export const MAX_AUTO_CONTINUATIONS = 5;
// La instruccion MINIMA que abre cada continuacion. Va sola, sin re-adjuntar
// imagenes y sin repetir el mensaje del usuario: el contexto entero ya viaja
// en el `resume` de la misma sesion, y duplicar el mensaje haria que el modelo
// creyera que se le ha pedido lo mismo dos veces. En ingles como role.md.
export const CONTINUE_PROMPT =
  'Continue. You hit the per-leg turn limit mid-task; pick up exactly where you left off and ' +
  'finish the work. Do not start over, do not re-ask the user, do not summarize what you already did.';

// ---------------------------------------------------------------------------
// RONDA UI (F8): inyeccion de query() SOLO para pruebas. Produccion nunca llama
// a __setQueryForTests, asi que el adaptador conserva su query real. Existe
// porque el bucle de reintento de auth no se puede verificar sin red de otra
// forma (gates G1.b / G1.c).
// ---------------------------------------------------------------------------
export type QueryFn = ProviderQueryFn;
let queryImpl: QueryFn | undefined;
export function __setQueryForTests(q: QueryFn): void {
  queryImpl = q;
}
export function __resetQueryForTests(): void {
  queryImpl = undefined;
}

// ---------------------------------------------------------------------------
// Tiny single-consumer async queue for chat_in "user"/"model"/"hello" records.
// "stop" never goes through this queue -- it must take effect immediately
// even while the main loop is blocked awaiting a turn in flight.
// ---------------------------------------------------------------------------

class AsyncQueue<T> {
  private items: T[] = [];
  private waiters: Array<(v: T) => void> = [];

  push(item: T): void {
    const waiter = this.waiters.shift();
    if (waiter) waiter(item);
    else this.items.push(item);
  }

  take(): Promise<T> {
    if (this.items.length > 0) return Promise.resolve(this.items.shift() as T);
    return new Promise<T>((resolve) => this.waiters.push(resolve));
  }
}

/** Product role only. The retired rig/Minecraft reference is deliberately not
 * loaded into the native visual agent's context. */
function readRole(): string {
  return readFileSync(path.join(__dirname, '..', 'prompts', 'render-llm-role.md'), 'utf8');
}

/**
 * Backstop permission handler (spec A 5.4 + quirk Q17). `allowedTools` solo
 * AUTO-APRUEBA; no restringe lo que se ofrece. `canUseTool` es la ultima linea:
 * Read SOLO bajo Data/assistant/preview/ y Data/assistant/render_job/ (RONDA B:
 * ahi es donde caen los renders de calidad que el modelo tiene que MIRAR),
 * mcp__render-llm__* permitido, todo lo demas denegado.
 * Cada decision QUE LLEGA AQUI deja traza en assistant.log.
 *
 * CUIDADO (medido en la verificacion C, SDK 0.3.222): `query()` corre con
 * `cwd: cfg.dir` (= Data/assistant), y el SDK AUTO-APRUEBA las lecturas de
 * ficheros que caen dentro de ese cwd SIN pasar por este callback. Es decir:
 * un Read de `preview/*.png` o de `render_job/<job>/out.png` -- justo los dos
 * que si queremos permitir -- NO deja ninguna linea "permiso: Read ..." en
 * assistant.log. Comprobado a mano con el cerebro real: se le pidio Read de un
 * preview por ruta absoluta, el modelo devolvio una descripcion correcta de la
 * imagen y el log quedo vacio.
 *
 * Consecuencia practica: un assistant.log SIN lineas "permiso: Read" NO
 * demuestra que el modelo no haya mirado el PNG (el gate B5 de la ronda B
 * interpreto esa ausencia como prueba de lo contrario). La traza de que el
 * modelo VIO la imagen es la linea "imagen adjuntada al resultado de la tool"
 * de readPngForModel, que si es incondicional. Lo que este callback si
 * demuestra, y sigue siendo su razon de ser, es la JAULA: cualquier Read
 * FUERA del cwd (p.ej. C:\Windows\win.ini) si aterriza aqui y se deniega con
 * traza -- ver evid/v_gate_cage.py.
 *
 * RONDA E: esta funcion NO lee la memoria del usuario, ni recibe nada derivado
 * de ella, y no debe hacerlo nunca. Su firma es (toolName, input) sobre un
 * `dataDir` fijo: ninguna preferencia guardada puede alterar una sola de sus
 * decisiones. Esa independencia ESTRUCTURAL es lo que hace demostrable el gate
 * adversarial de la ronda E; hay un test que comprueba que ni el modulo ni el
 * cierre mencionan la memoria.
 */
export function denyByDefault(p: Protocol, dataDir: string): ProviderCanUseTool {
  const roots = [
    path.resolve(path.join(dataDir, 'preview')),
    path.resolve(path.join(dataDir, 'render_job')),
  ];
  return async (toolName, input): Promise<ProviderPermissionResult> => {
    if (toolName === 'Read') {
      const raw =
        input && typeof (input as Record<string, unknown>).file_path === 'string'
          ? ((input as Record<string, unknown>).file_path as string)
          : undefined;
      if (raw) {
        const resolved = path.resolve(raw);
        for (const root of roots) {
          if (resolved === root || resolved.startsWith(root + path.sep)) {
            p.log(`permiso: Read ALLOW ${resolved}`);
            return { behavior: 'allow' };
          }
        }
      }
      p.log(`permiso: Read DENY ${raw ?? '(sin file_path)'}`);
      return {
        behavior: 'deny',
        message: 'Read is only allowed under Data/assistant/preview/ and Data/assistant/render_job/.',
      };
    }
    if (toolName.startsWith('mcp__render-llm__')) {
      return { behavior: 'allow' };
    }
    p.log(`permiso: ${toolName} DENY`);
    return { behavior: 'deny', message: `Tool "${toolName}" is not available to the render-llm assistant.` };
  };
}

// Exportada SOLO para que los gates G1.b/G1.c/G8.1 puedan conducir el bucle
// real con un query() doble (ver __setQueryForTests). Produccion la llama desde
// main() como siempre.
export async function runReal(p: Protocol, cfg: Config): Promise<void> {
  const provider = getVisualAgentProvider(cfg.provider ?? 'codex');
  const roleText = readRole();
  p.beat('starting', { model: cfg.model });

  const memory = new MemoryStore(cfg.dir, (m) => p.log(m));

  // RONDA D (persistencia): retomar la sesion del SDK de un cerebro
  // ANTERIOR (relanzamientos de PROCESO, no solo turnos dentro de uno), para
  // que el modelo conserve contexto tras cerrar y reabrir el editor.
  let sessionId: string | undefined = p.readSessionId();
  if (sessionId) p.log(`RONDA D: retomando sesion persistida ${sessionId}`);

  let currentTurnId: string | undefined;
  let currentAbort: AbortController | undefined;

  const queue = new AsyncQueue<InRecord>();

  p.onIn((rec) => {
    if (rec.type === 'stop') {
      if (rec.turn_id === currentTurnId && currentAbort) {
        p.log(`stop recibido para turno activo ${rec.turn_id}`);
        currentAbort.abort();
      } else {
        p.log(`stop para turno inactivo/desconocido ${rec.turn_id} (activo=${currentTurnId ?? '(ninguno)'})`);
      }
      return;
    }
    queue.push(rec);
  });

  p.beat('idle', { model: cfg.model });

  for (;;) {
    const rec = await queue.take();

    if (rec.type === 'model') {
      cfg.model = rec.model;
      p.log(`modelo cambiado a ${rec.model} (aplica al siguiente turno)`);
      p.beat('idle', { model: cfg.model });
      continue;
    }
    // RONDA UI (F2): el nivel de razonamiento es del USUARIO y viaja como el
    // modelo -- se aplica al SIGUIENTE turno, nunca a mitad de uno en vuelo.
    if (rec.type === 'effort') {
      cfg.effort = rec.effort as EffortLevel;
      p.log(`effort cambiado a ${rec.effort} (aplica al siguiente turno)`);
      p.beat('idle', { model: cfg.model });
      continue;
    }
    // RONDA UI (F3): cambio de chat. El proceso NO se reinicia: solo se
    // repunta session.json y se recoge (o se descarta) la sesion del SDK.
    if (rec.type === 'chat') {
      p.setChat(rec.chat_id);
      sessionId = rec.action === 'new' ? undefined : p.readSessionId();
      p.log(`chat ${rec.action} -> ${rec.chat_id} (sesion ${sessionId ?? 'nueva'})`);
      p.emit({ type: 'status', state: 'ready', model: cfg.model });
      p.beat('idle', { model: cfg.model });
      continue;
    }
    if (rec.type === 'hello') {
      memory.setProject(rec.project); // RONDA E
      p.emit({ type: 'status', state: 'ready', model: cfg.model });
      p.beat('idle', { model: cfg.model });
      continue;
    }
    if (rec.type !== 'user') continue;
    if (rec.project_file !== undefined) memory.setProject(rec.project_file); // RONDA E

    currentTurnId = rec.turn_id;
    currentAbort = new AbortController();
    const abort = currentAbort;

    p.emit({ type: 'status', state: 'thinking', turn_id: rec.turn_id, model: cfg.model });
    p.beat('thinking', { model: cfg.model });

    const ctx: ActionTurnCtx = {
      turnId: rec.turn_id,
      signal: abort.signal,
    };

    // RONDA D (streaming fino): ver el comentario de TOKEN_COALESCE_MS mas
    // arriba. flushTokens() SIEMPRE se llama antes de "done" (si no, el
    // filtro por turn_id de S3.8 tira el resto en silencio) y en cualquier
    // salida del turno (abort, error).
    let tokenBuf = '';
    let tokenTimer: ReturnType<typeof setTimeout> | undefined;
    const flushTokens = () => {
      if (tokenTimer) {
        clearTimeout(tokenTimer);
        tokenTimer = undefined;
      }
      if (tokenBuf.length === 0) return;
      const text = tokenBuf;
      tokenBuf = '';
      p.emit({ type: 'token', turn_id: rec.turn_id, text });
    };
    const onTextDelta = (text: string) => {
      tokenBuf += text;
      if (tokenBuf.length >= TOKEN_COALESCE_MAX_CHARS) {
        flushTokens();
        return;
      }
      if (!tokenTimer) tokenTimer = setTimeout(flushTokens, TOKEN_COALESCE_MS);
    };

    // RONDA D (robustez): si session.json trae un id que el servidor ya no
    // reconoce (expirado, de otra maquina, SDK reinstalado...) el turno no
    // debe fallar en silencio con un "ERROR" opaco. Un solo reintento SIN
    // resume, y SOLO si no llego a procesarse ni un mensaje -- asi no hay
    // riesgo de duplicar nada que el usuario ya haya visto en pantalla.
    let resumeFor = sessionId;
    let attempt = 0;

    // FIX max-turns: estado del turno COMPLETO (todos sus tramos), no de un
    // tramo suelto -- por eso vive fuera del turnLoop.
    //  - `continuations`: cuantas veces se ha continuado solo (tope
    //    MAX_AUTO_CONTINUATIONS).
    //  - `costAcc`/`msAcc`: el coste y el tiempo SUMADOS de todos los tramos.
    //    El editor recibe UN solo "done" al final del turno, asi que si no se
    //    sumaran aqui el usuario veria el precio del ultimo tramo como si
    //    fuera el de la tarea entera.
    let continuations = 0;
    let costAcc = 0;
    let costObserved = false;
    let msAcc = 0;

    // RONDA E: la memoria se RELEE en cada turno, a proposito -- el usuario
    // puede haber editado el .md a mano o borrado una entrada desde la pestana
    // mientras el proceso seguia vivo (gate 3 y 4). Se loguea el bloque
    // EFECTIVO: es la unica forma honesta de verificar que lo inyectado es lo
    // que creemos (gates 2 y 3 lo grepean).
    const memoryBlock = memory.block();
    p.log(
      `RONDA E: bloque de memoria inyectado (${memoryBlock.split('\n').length} lineas, ` +
        `${Buffer.byteLength(memoryBlock, 'utf8')} bytes):\n${memoryBlock}`,
    );
    const systemPrompt = `${roleText}\n\n---\n\n${memoryBlock}`;

    // RONDA G: adjuntos de imagen que el USUARIO eligio para ESTE turno (ver
    // el invariante de seguridad en SPEC_ASSISTANT_RONDA_G.md S3.0 -- el
    // modelo no tiene ninguna tool para pedir/listar/adjuntar imagenes, esto
    // es contenido del mensaje que ya llego en chat_in).
    const { blocks, notes } = readAttachments(cfg.dir, rec.turn_id, rec.attachments ?? '', (m) => p.log(m));

    // Guarda de vision: los tres niveles del desplegable la tienen. Si algun
    // dia se anade uno sin vision, se AVISA en el
    // chat en vez de mandar bloques que el modelo no puede interpretar.
    let imgBlocks = blocks;
    if (imgBlocks.length > 0 && !provider.supportsVision(cfg.model)) {
      p.emit({
        type: 'text',
        turn_id: rec.turn_id,
        text: `El nivel actual no admite imagenes: las ${imgBlocks.length} adjuntas se ignoran en este turno. Cambia a Medio o Alto y vuelve a enviarlas.`,
      });
      p.log(`RONDA G: modelo ${cfg.model} sin vision, ${imgBlocks.length} adjuntos descartados`);
      imgBlocks = [];
    }
    for (const n of notes) p.emit({ type: 'text', turn_id: rec.turn_id, text: n });

    // RONDA G: un generador async solo se puede recorrer UNA vez, y el
    // turnLoop de abajo puede reintentar el turno sin `resume` si la sesion
    // persistida no se pudo retomar -- por eso esto es una FACTORIA que se
    // llama de nuevo en CADA vuelta del bucle, no un valor construido una
    // sola vez fuera de el (si no, el reintento mandaria un iterable ya
    // agotado y el turno se quedaria colgado esperando mensajes que nunca
    // llegan).
    // IMAGENES ANTES DEL TEXTO: es lo recomendado cuando la pregunta es
    // SOBRE las imagenes.
    // FIX max-turns: en una CONTINUACION el prompt es solo la instruccion de
    // seguir. Ni el texto del usuario ni sus imagenes se reenvian: ya estan en
    // la sesion que se reanuda, y repetirlos le diria al modelo que se le ha
    // pedido lo mismo otra vez.
    const makePrompt = (): string | AsyncIterable<ProviderUserMessage> =>
      continuations > 0
        ? CONTINUE_PROMPT
        : imgBlocks.length === 0
        ? rec.text
        : (async function* () {
            yield {
              type: 'user' as const,
              session_id: '',
              parent_tool_use_id: null,
              message: { role: 'user' as const, content: [...imgBlocks, { type: 'text' as const, text: rec.text }] },
            };
          })();

    // RONDA UI (F8/F9): que clase de fallo termino el turno, si termino mal.
    // Lo mira el bloque de poda de adjuntos de mas abajo: un turno perdido por
    // auth o por contexto conserva sus imagenes para poder reenviarlo.
    let lastFailure: TurnFailure | undefined;

    // RONDA UI (F2, traza verificable del gate G2): la linea que demuestra que
    // el modelo Y el nivel de razonamiento elegidos en el composer llegan a la
    // llamada REAL, una vez por turno.
    p.log(`turno ${rec.turn_id}: model=${cfg.model} effort=${cfg.effort}`);

    turnLoop: for (;;) {
      attempt++;
      const q = (queryImpl ?? provider.query)({
        prompt: makePrompt(),
        options: {
          abortController: abort,
          cwd: cfg.dir,
          model: cfg.model,
          // RONDA UI (F2): nivel de razonamiento del usuario. 'xhigh'/'max'
          // caen a 'high' solos en los modelos que no los soportan (lo hace el
          // propio SDK); el editor lo dice en el tooltip para no prometer de mas.
          effort: cfg.effort,
          systemPrompt,
          mcpServers: { 'render-llm': buildRenderLlmMcp(p, ctx) },
          // FIX #102 (ronda K0): mcpServers ya es EXPLICITO (arriba) pero eso
          // solo dice "estos son los que TU pasas" -- no evita que el SDK
          // sume mas de otras fuentes. strictMcpConfig (sdk.d.ts) ignora
          // .mcp.json de proyecto, settings de usuario, plugins y frontmatter
          // de agentes: la UNICA fuente de servers pasa a ser este objeto.
          strictMcpConfig: true,
          // Los conectores cloud de la CUENTA de Antonio (Gmail/Calendar/
          // Indeed...) no vienen de settingSources ni de mcpServers: el SDK
          // los auto-descubre desde las credenciales de la cuenta que lanza
          // el proceso. settingSources:[] y strictMcpConfig no los tocan
          // (no son "settings sources" ni "mcp config"). Este flag va en la
          // capa "flag settings" (maxima prioridad) y corta el auto-fetch en
          // origen. Medido en la ronda J: sin esto, 29 tools ajenas se colaban
          // en el catalogo de la sesion de forma intermitente.
          // RONDA UI (F9): autoCompactEnabled es la PREVENCION del "Prompt is
          // too long" que quemaba turnos en produccion. El SDK compacta la
          // conversacion antes de desbordar; la compactacion se ve en el log
          // (compact_boundary) y en el chat, nunca en silencio.
          settings: { disableClaudeAiConnectors: true, autoCompactEnabled: true },
          tools: ['Read'],
          // "Read" NO va en allowedTools a proposito: una entrada "pelada" en
          // allowedTools auto-aprueba la herramienta ANTES de consultar
          // canUseTool (el SDK lo avisa: CLAUDE_SDK_CAN_USE_TOOL_SHADOWED), y
          // entonces el filtro de ruta a Data/assistant/preview/ NUNCA se
          // aplicaba: el modelo podia leer cualquier archivo del disco. Al
          // dejarlo fuera, Read cae en canUseTool y SI se comprueba la ruta.
          // Las acciones mcp__render-llm__* se pre-aprueban: son nuestras y el ejecutor del
          // editor es su frontera real.
          allowedTools: [...RENDER_LLM_TOOL_NAMES],
          disallowedTools: DISALLOWED_BUILT_INS,
          canUseTool: denyByDefault(p, cfg.dir),
          permissionMode: 'default',
          settingSources: [], // isolation: no CLAUDE.md / settings.json from disk (quirk Q18)
          includePartialMessages: true,
          // FIX max-turns: tope POR TRAMO. Agotarlo ya no mata el turno --
          // ver MAX_AUTO_CONTINUATIONS y handleTurnFailure.
          maxTurns: MAX_TURNS_PER_LEG,
          resume: resumeFor,
          persistSession: true,
          stderr: (d: string) => p.log(`[sdk stderr] ${d}`),
          renderLlmTurnId: rec.turn_id,
        },
      });

      let sawStreamEvent = false;
      let sawAnyMessage = false;
      // RONDA UI (F8/F9): senal ESTRUCTURADA del fallo, recogida mientras se
      // itera. Tiene prioridad sobre el texto del error (que es ingles del
      // servidor y cambia).
      let structuredError: string | undefined;
      let terminalReason: string | undefined;
      // FIX max-turns: el subtype del result de error, guardado aparte para que
      // handleTurnFailure pueda mirar la senal estructurada ('error_max_turns')
      // en vez del texto ingles del servidor.
      let resultSubtype: string | undefined;
      // Un `result` de subtype != success TAMBIEN es un fallo del turno, no
      // solo la excepcion: asi es como llega "Prompt is too long".
      let resultFailure: string | undefined;

      /**
       * RONDA UI (F8/F9): tratamiento UNICO de un turno fallido, venga de una
       * excepcion o de un `result` de error. Orden: max_turns -> auth ->
       * context -> bad_resume -> otros. Devuelve 'retry' si hay que dar otra
       * vuelta al turnLoop (makePrompt() se vuelve a llamar solo, con un
       * iterable NUEVO -- incluidos los bloques de imagen).
       */
      const handleTurnFailure = async (message: string): Promise<'retry' | 'stop'> => {
        // FIX max-turns: PRIMERO, porque no es un fallo. Agotar el tope del
        // tramo solo significa que la tarea era mas larga que un tramo: se
        // continua la MISMA sesion, en silencio, y el usuario no ve ninguna
        // costura (ni "done", ni burbuja nueva, ni tarjeta).
        if (isMaxTurns(message, terminalReason, resultSubtype)) {
          if (abort.signal.aborted) {
            // El Stop gana: se corta el turno aqui y NO se lanza otro tramo.
            p.log(`max-turns: stop del usuario en el turno ${rec.turn_id}; no se continua`);
            p.emit(
              costAcc > 0
                ? { type: 'done', turn_id: rec.turn_id, reason: 'stopped', cost_usd: costAcc, ms: msAcc }
                : { type: 'done', turn_id: rec.turn_id, reason: 'stopped' },
            );
            return 'stop';
          }
          if (continuations < MAX_AUTO_CONTINUATIONS && sessionId) {
            continuations++;
            // Cada tramo estrena su propio presupuesto de reintentos (el
            // reintento unico de auth, el de resume): son de TRAMO, no de turno.
            attempt = 0;
            // La sesion a reanudar es la del tramo que acaba de terminar -- la
            // que el system/init de ESTE tramo dejo en `sessionId`, no la que
            // traia el turno al empezar.
            resumeFor = sessionId;
            p.log(
              `max-turns: tramo agotado en el turno ${rec.turn_id}; continuacion automatica ` +
                `${continuations}/${MAX_AUTO_CONTINUATIONS} reanudando ${sessionId}`,
            );
            // El indicador de actividad sigue vivo: es lo UNICO que ve el usuario.
            p.emit({ type: 'status', state: 'thinking', turn_id: rec.turn_id, model: cfg.model });
            p.beat('thinking', { model: cfg.model, session_id: sessionId });
            return 'retry';
          }
          // Tope global agotado (o ningun session_id que reanudar): se cierra el
          // turno con la tarjeta de SIEMPRE -- el mismo record "error" con el
          // texto del SDK que el editor ya sabe pintar -- y con el coste
          // acumulado de todos los tramos.
          p.log(
            `max-turns: tope global agotado en el turno ${rec.turn_id} ` +
              `(${continuations}/${MAX_AUTO_CONTINUATIONS} continuaciones${sessionId ? '' : ', sin sesion que reanudar'})`,
          );
          p.emit({ type: 'error', turn_id: rec.turn_id, message });
          p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'max_turns', cost_usd: costAcc, ms: msAcc });
          return 'stop';
        }

        const kind = classifyFailure(message, structuredError ?? terminalReason);
        lastFailure = kind;

        if (kind === 'auth' && attempt === 1) {
          p.log(`AUTH: fallo de autenticacion en el turno ${rec.turn_id}; reintentando en ${AUTH_RETRY_MS} ms`);
          p.emit({ type: 'status', state: 'thinking', turn_id: rec.turn_id, model: cfg.model });
          p.emit({ type: 'notice', turn_id: rec.turn_id, kind: 'auth_retry', text: '' });
          await sleep(AUTH_RETRY_MS, abort.signal);
          return 'retry';
        }
        if (kind === 'auth') {
          p.log(`AUTH: fallo persistente tras el reintento: ${message}`);
          p.emit({ type: 'notice', turn_id: rec.turn_id, kind: 'auth_failed', text: message });
          p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'auth', cost_usd: 0 });
          return 'stop';
        }
        if (kind === 'context') {
          // Sin reintentar: el mismo prompt gigante vuelve a fallar y cuesta.
          p.log(`CONTEXTO: el turno ${rec.turn_id} no cabe: ${message}`);
          p.emit({ type: 'notice', turn_id: rec.turn_id, kind: 'context_full', text: message });
          p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'context_full', cost_usd: 0 });
          return 'stop';
        }
        // FIX max-turns: `continuations === 0` es OBLIGATORIO aqui. En una
        // continuacion el prompt es solo "sigue": reintentarlo SIN resume
        // mandaria esa frase a una sesion vacia, sin nada que seguir. Si la
        // sesion de una continuacion no se puede retomar, el turno se cierra
        // por el camino de abajo (error honesto) en vez de responder a ciegas.
        if (kind === 'bad_resume' && !!resumeFor && !sawStreamEvent && attempt === 1 && continuations === 0) {
          // RONDA UI: dos arreglos medidos con el cerebro real (un session.json
          // con un id que el servidor no reconoce hacia fallar TODOS los turnos
          // siguientes, no solo el primero):
          //  - la guarda era `!sawAnyMessage`, y el SDK emite un system/init
          //    ANTES de rechazar el --resume, asi que el reintento no saltaba
          //    NUNCA. Lo que de verdad hay que proteger es no duplicar lo que
          //    el usuario YA vio en pantalla: eso es `!sawStreamEvent`.
          //  - `sessionId` se quedaba envenenado en memoria: aunque este turno
          //    se reintentara sin resume, el siguiente volvia a intentarlo con
          //    el mismo id malo. Se descarta de una vez.
          p.log(`WARN sesion persistida ${resumeFor} no se pudo retomar (${message}); se descarta y se reintenta este turno sin resume`);
          resumeFor = undefined;
          sessionId = undefined;
          return 'retry';
        }
        p.log(`ERROR en turno ${rec.turn_id}: ${message}`);
        p.emit({ type: 'error', turn_id: rec.turn_id, message });
        p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'error' });
        return 'stop';
      };

      try {
        for await (const m of q as AsyncIterable<ProviderMessage>) {
          sawAnyMessage = true;
          if (m.type === 'render_llm_tool') {
            sawStreamEvent = true;
            p.emit({
              type: 'tool',
              turn_id: rec.turn_id,
              name: m.name,
              phase: m.phase,
              ...(m.ok === undefined ? {} : { ok: m.ok }),
              ...(m.ms === undefined ? {} : { ms: m.ms }),
            });
          } else if (m.type === 'system' && m.subtype === 'init') {
            sessionId = m.session_id;
            p.writeSessionId(sessionId); // RONDA D (persistencia)
            p.beat('thinking', { model: cfg.model, session_id: sessionId });
            // RONDA E (gate 5): dejar constancia del catalogo EFECTIVO de tools
            // de la sesion. Una memoria adversarial no puede anadir ninguna, y
            // esta linea es como se demuestra en vez de afirmarse.
            const initTools = (m as unknown as { tools?: unknown }).tools;
            p.log(`RONDA E: tools de la sesion: ${Array.isArray(initTools) ? initTools.join(',') : '(no reportadas)'}`);
          } else if (m.type === 'system' && m.subtype === 'compact_boundary') {
            // RONDA UI (F9): la compactacion deja de ser invisible.
            const meta = m.compact_metadata;
            p.log(`compactacion ${meta.trigger}: ${meta.pre_tokens} -> ${meta.post_tokens ?? '?'} tokens`);
            p.emit({ type: 'notice', turn_id: rec.turn_id, kind: 'compacted' });
          } else if (m.type === 'system' && m.subtype === 'status' && m.status === 'compacting') {
            p.emit({ type: 'status', state: 'thinking', turn_id: rec.turn_id, model: cfg.model });
            p.emit({ type: 'notice', turn_id: rec.turn_id, kind: 'compacting' });
          } else if (m.type === 'auth_status') {
            // RONDA UI (F8): SDKAuthStatusMessage. `error` presente = la
            // re-autenticacion del SDK no salio bien.
            if (m.error) structuredError = 'authentication_failed';
            p.log(`auth_status: authenticating=${m.isAuthenticating} error=${m.error ?? ''}`);
          } else if (m.type === 'stream_event') {
            const ev = m.event;
            if (ev.type === 'content_block_delta' && ev.delta.type === 'text_delta') {
              sawStreamEvent = true;
              onTextDelta(ev.delta.text);
            }
          } else if (m.type === 'assistant') {
            // RONDA UI (F8): SDKAssistantMessage.error es la senal estructurada
            // del fallo (sdk.d.ts, SDKAssistantMessageError). Se recoge SIEMPRE,
            // haya habido stream_event o no.
            if (m.error) structuredError = m.error;
            // RONDA UI (F8, visto con el cerebro real): cuando el mensaje trae
            // `error`, su CONTENIDO es el texto del fallo en ingles del
            // servidor ("Failed to authenticate: OAuth session expired..."). Si
            // se emite como respuesta normal acaba en el chat Y en
            // history.jsonl, que es justo el error criptico que Antonio veia
            // tres veces seguidas. El fallo se cuenta abajo, como notice.
            if (!sawStreamEvent && !m.error) {
              // Fallback for when partial messages did not arrive for this turn.
              flushTokens();
              for (const block of m.message.content) {
                if (block.type === 'text' && block.text) {
                  p.emit({ type: 'text', turn_id: rec.turn_id, text: block.text });
                }
              }
            }
          } else if (m.type === 'result') {
            flushTokens(); // RONDA D: nunca dejar texto colgado detras de "done"
            terminalReason = m.terminal_reason;
            resultSubtype = m.subtype;
            // FIX max-turns: el coste y el tiempo son de TODOS los tramos del
            // turno. Se suman aqui, en el unico sitio por el que pasan todos
            // los result (exito o no), y el "done" del final reporta el total.
            if (typeof m.total_cost_usd === 'number') {
              costObserved = true;
              costAcc += m.total_cost_usd;
            }
            msAcc += m.duration_ms ?? 0;
            if (m.subtype !== 'success' || structuredError) {
              // RONDA UI (F8/F9): un result de error NO emite "done" aqui --
              // se trata abajo por el MISMO camino que la excepcion, para que
              // "Prompt is too long" (que llega asi) sea una tarjeta y no un
              // error seco que quema el turno. El SDK tambien puede emitir un
              // SDKAssistantMessage con `error`, seguido por un result
              // superficialmente success/stop_sequence: la senal estructurada
              // manda para que no se cierre como una respuesta vacia valida.
              resultFailure = structuredError || ('errors' in m ? (m.errors ?? []).join('; ') : '') || m.subtype;
              break;
            }
            p.emit({
              type: 'done',
              turn_id: rec.turn_id,
              reason: m.stop_reason ?? 'end_turn',
              ...(costObserved ? { cost_usd: costAcc } : {}),
              ms: msAcc,
            });
          }
        }
        if (resultFailure === undefined) break turnLoop;
        // Cae al tratamiento comun con el mensaje del result de error.
        const outcome = await handleTurnFailure(resultFailure);
        if (outcome === 'retry') continue turnLoop;
        break turnLoop;
      } catch (e) {
        flushTokens();
        if (abort.signal.aborted) {
          // FIX max-turns: un Stop en la 3a continuacion tiene coste de los dos
          // tramos anteriores; se reporta. Si no hubo ninguno, el "done" va
          // pelado como siempre (un cost_usd:0 pintaria "$0.0000" en la fila
          // de herramientas de un turno que el usuario acaba de cortar).
          p.emit(
            costAcc > 0
              ? { type: 'done', turn_id: rec.turn_id, reason: 'stopped', cost_usd: costAcc, ms: msAcc }
              : { type: 'done', turn_id: rec.turn_id, reason: 'stopped' },
          );
          break turnLoop;
        }
        const outcome = await handleTurnFailure(e instanceof Error ? e.message : String(e));
        if (outcome === 'retry') continue turnLoop;
        break turnLoop;
      }
    }

    currentTurnId = undefined;
    currentAbort = undefined;
    // RONDA G: podar carpetas de turno viejas de attachments/ tras CADA
    // turno (barato: solo lista directorios). Nunca la carpeta del turno
    // recien usado -- ver el comentario de pruneAttachments en attachments.ts.
    // RONDA UI (F8/F9): salvo si el turno se PERDIO por auth o por contexto --
    // entonces sus imagenes tienen que seguir ahi para poder reenviarlo.
    if (lastFailure !== 'auth' && lastFailure !== 'context') pruneAttachments(cfg.dir, (m) => p.log(m));
    p.emit({ type: 'status', state: 'idle', model: cfg.model });
    p.beat('idle', { model: cfg.model });
  }
}

/**
 * RONDA D (robustez): ¿hay un proceso vivo con este pid? `process.kill(pid,
 * 0)` no manda ninguna senal (ni en Windows) -- solo prueba existencia/
 * permiso. ESRCH = no existe; EPERM = existe pero es de otro usuario (sigue
 * contando como vivo).
 */
function isPidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return (e as NodeJS.ErrnoException).code === 'EPERM';
  }
}

async function main(): Promise<void> {
  const cfg = parseArgs();

  // RONDA D (robustez, "el launcher detecta cerebro ya vivo, no duplica"):
  // Protocol() limpia chat_in/out, ops_req/res y brain.json en su
  // constructor asumiendo que es la UNICA sesion de proceso escribiendo ahi.
  // Si el usuario pulsa "Launch assistant" (o el hueco de _test_launch) justo
  // cuando un cerebro anterior sigue vivo -- p.ej. el heartbeat parpadeo a
  // "gone" sin que el proceso muriera de verdad -- un SEGUNDO proceso
  // corriendo esa limpieza a mitad de un turno real le arrancaria el
  // protocolo al primero por debajo. Se comprueba ANTES de tocar nada.
  const existing = readExistingBrain(cfg.dir);
  if (existing && existing.pid !== process.pid && isPidAlive(existing.pid)) {
    // eslint-disable-next-line no-console
    console.error(
      `render-llm assistant: ya hay un agente activo en este directorio (pid=${existing.pid}, estado=${existing.state}); ` +
        `este proceso (pid=${process.pid}) sale sin tocar el protocolo.`,
    );
    process.exit(0);
    return;
  }

  const p = new Protocol(cfg.dir);

  process.on('SIGINT', () => {
    p.log('SIGINT recibido, cerrando');
    p.close();
    process.exit(0);
  });
  process.on('SIGTERM', () => {
    p.log('SIGTERM recibido, cerrando');
    p.close();
    process.exit(0);
  });

  if (cfg.fake) {
    const { runFake } = await import('./fake.js');
    await runFake(p);
    return;
  }

  await runReal(p, cfg);
}

// RONDA E: solo arrancar main() cuando este modulo se ejecuta como programa
// (node dist/index.js ...), NO cuando otro modulo lo importa (p.ej.
// test/run-tests.mjs importando denyByDefault para el gate adversarial 5e).
// Sin esta guarda, un simple `import` ya lanzaba parseArgs() y hacia fallar
// cualquier test que necesitase esta funcion. No cambia nada de la postura
// de permisos ni del comportamiento en produccion: launch.cmd siempre invoca
// este fichero directamente, que es exactamente el caso que sigue arrancando.
const isMain = process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  main().catch((e) => {
    // eslint-disable-next-line no-console
    console.error('render-llm assistant fatal:', e);
    process.exit(1);
  });
}
