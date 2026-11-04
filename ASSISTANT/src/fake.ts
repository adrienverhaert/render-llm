// ASSISTANT/src/fake.ts
//
// Deterministic scripted "brain" used by --fake. No network, no model, no
// quota: chat_in is matched against a fixed keyword table (spec 5.5) and the
// script drives REAL ops through Protocol.runOps() against whatever editor
// (real or test-fake) is listening on the other end of the protocol. This is
// what gate G1 exercises end-to-end.
//
// Token pacing is 80ms so a harness can observe incremental streaming.

import { OpsAbortedError, Protocol, sleep, type Op, type OpResultEntry, type OpsRes } from './protocol.js';
import type { InRecord } from './protocol.js';
import { runRenderJob, type JobPhase } from './render.js';
import { MemoryStore } from './memory.js';

const TOKEN_DELAY_MS = 80;

// ---------------------------------------------------------------------------
// Tiny single-consumer async queue (mirrors the one in index.ts; kept local
// and duplicated on purpose so fake.ts has zero dependency on the real loop).
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function includesAny(haystack: string, needles: string[]): boolean {
  return needles.some((n) => haystack.includes(n));
}

/** Split `text` into exactly `n` pieces so callers can guarantee an exact token count. */
function splitIntoChunks(text: string, n: number): string[] {
  const words = text.split(' ').filter((w) => w.length > 0);
  if (words.length >= n) {
    const out: string[] = [];
    let idx = 0;
    for (let i = 0; i < n; i++) {
      const remainingChunks = n - i;
      const remainingWords = words.length - idx;
      const take = i === n - 1 ? remainingWords : Math.max(1, Math.ceil(remainingWords / remainingChunks));
      out.push((i === 0 ? '' : ' ') + words.slice(idx, idx + take).join(' '));
      idx += take;
    }
    return out;
  }
  // Fewer words than requested chunks: fall back to a character split so we
  // still emit exactly n token events (gates count events, not words).
  const per = Math.max(1, Math.ceil(text.length / n));
  const out: string[] = [];
  for (let i = 0; i < n; i++) out.push(text.slice(i * per, (i + 1) * per));
  return out;
}

/** Emit `text` as exactly `n` token events, 80ms apart. Returns false if aborted partway. */
async function narrate(p: Protocol, turnId: string, signal: AbortSignal, text: string, n: number): Promise<boolean> {
  for (const chunk of splitIntoChunks(text, n)) {
    if (signal.aborted) return false;
    p.emit({ type: 'token', turn_id: turnId, text: chunk });
    await sleep(TOKEN_DELAY_MS, signal);
    if (signal.aborted) return false;
  }
  return true;
}

/** Run one or more real ops as a single "tool" event pair, like an MCP tool call would. */
async function callOp(
  p: Protocol,
  turnId: string,
  signal: AbortSignal,
  name: string,
  startSummary: string,
  ops: Op[],
  timeoutMs = 15000,
): Promise<OpsRes> {
  const t0 = Date.now();
  p.emit({ type: 'tool', turn_id: turnId, name, phase: 'start', summary: startSummary });
  try {
    const res = await p.runOps(ops, turnId, timeoutMs, signal);
    const ms = Date.now() - t0;
    const ok = !res.error && res.ops_total === res.ops_ok;
    // RONDA D: mismo cambio que runTool() en tools.ts -- summary tambien en "end".
    p.emit({ type: 'tool', turn_id: turnId, name, phase: 'end', ok, ms, summary: startSummary });
    return res;
  } catch (e) {
    const ms = Date.now() - t0;
    p.emit({ type: 'tool', turn_id: turnId, name, phase: 'end', ok: false, ms, summary: startSummary });
    throw e;
  }
}

interface SceneObjectRow {
  saveId: string;
  name: string;
  type: string;
  parent: string;
  pos: [number, number, number];
  rot: [number, number, number];
  kf: string;
}

/** Parse assistant.scene_summary's `objects` field: "id|name|type|parent|px,py,pz|rx,ry,rz|kf;..." */
function parseObjectsField(objectsText: unknown): SceneObjectRow[] {
  if (typeof objectsText !== 'string' || objectsText.length === 0) return [];
  const rows: SceneObjectRow[] = [];
  for (const entry of objectsText.split(';')) {
    const t = entry.trim();
    if (!t) continue;
    const fields = t.split('|');
    if (fields.length < 7) continue;
    const [saveId, name, type, parent, posStr, rotStr, kf] = fields;
    const pos = posStr.split(',').map(Number);
    const rot = rotStr.split(',').map(Number);
    if (pos.length !== 3 || rot.length !== 3) continue;
    rows.push({
      saveId,
      name,
      type,
      parent,
      pos: pos as [number, number, number],
      rot: rot as [number, number, number],
      kf,
    });
  }
  return rows;
}

function isCameraType(type: string): boolean {
  return /cam/i.test(type);
}

// ---------------------------------------------------------------------------
// Scripts (spec 5.5)
// ---------------------------------------------------------------------------

async function scriptEscena(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  const res = await callOp(p, turnId, signal, 'mi3_scene_summary', 'leyendo la escena', [
    { id: 's1', op: 'assistant.scene_summary', args: { max_objects: 200 } },
  ]);
  if (signal.aborted) return;
  const r0 = res.results?.[0] as OpResultEntry | undefined;
  const proj = String(r0?.project ?? '(sin guardar)');
  const rows = parseObjectsField(r0?.objects);
  const text =
    `He mirado la escena "${proj}". Cuenta ${rows.length} objetos en el primer nivel. ` +
    'Puedo mover cosas, poner keyframes, encuadrar la camara o hacerte un preview cuando quieras.';
  await narrate(p, turnId, signal, text, 6);
}

/**
 * RONDA F: guion de creacion (para el e2e de frescura sin gastar cuota, spec
 * SPEC_ASSISTANT_RONDA_F.md 3.6 / gate F6.2.i): crea DOS personajes por ops
 * reales (assistant.object_add) y despues pide un scene_summary -- en OTRO
 * ops_req, pero el mismo turno -- para demostrar que no hay cache obsoleta
 * (mi3_as_op_scene_summary no la tiene; ver spec 1.4).
 */
async function scriptCrear(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  await callOp(p, turnId, signal, 'mi3_object_add', 'creando 1 x character', [
    { id: 'a1', op: 'assistant.object_add', args: { kind: 'character', name: 'Jugador 1' } },
  ]);
  if (signal.aborted) return;
  await callOp(p, turnId, signal, 'mi3_object_add', 'creando 1 x character', [
    { id: 'a1', op: 'assistant.object_add', args: { kind: 'character', name: 'Jugador 2' } },
  ]);
  if (signal.aborted) return;

  const res = await callOp(p, turnId, signal, 'mi3_scene_summary', 'leyendo la escena', [
    { id: 's1', op: 'assistant.scene_summary', args: { max_objects: 200 } },
  ]);
  if (signal.aborted) return;
  const r0 = res.results?.[0] as OpResultEntry | undefined;
  const rows = parseObjectsField(r0?.objects);
  const created = rows.filter((r) => r.name === 'Jugador 1' || r.name === 'Jugador 2').length;
  const text = `Cree 2 jugadores. La escena ya los ve: ${created} de 2 aparecen en el resumen que acabo de pedir.`;
  await narrate(p, turnId, signal, text, 5);
}

async function scriptMoveCube(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  const sceneRes = await callOp(p, turnId, signal, 'mi3_scene_summary', 'leyendo la escena', [
    { id: 's1', op: 'assistant.scene_summary', args: { max_objects: 200 } },
  ]);
  if (signal.aborted) return;
  const r0 = sceneRes.results?.[0] as OpResultEntry | undefined;
  const rows = parseObjectsField(r0?.objects);
  const target = rows.find((r) => !isCameraType(r.type));

  if (!target) {
    await narrate(p, turnId, signal, 'No encontre ningun objeto que no sea camara para mover.', 4);
    return;
  }

  const oldX = target.pos[0];
  const newX = oldX + 10;
  await callOp(p, turnId, signal, 'mi3_set_values', `aplicando valores a ${target.name}`, [
    { id: 'sv1', op: 'assistant.select', args: { ref: target.name } },
    { id: 'sv2', op: 'assistant.values_set', args: { ref: target.name, values: { POS_X: newX }, at_marker: true } },
  ]);
  if (signal.aborted) return;

  const text = `Moví "${target.name}" 10 unidades en X (de ${oldX} a ${newX}). Quedo un keyframe en el marker actual.`;
  await narrate(p, turnId, signal, text, 4);
}

async function scriptPreview(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  const width = 480;
  const height = 270;
  const out = `preview/${turnId.replace(/[^A-Za-z0-9_-]/g, '_')}_1.png`;
  const res = await callOp(p, turnId, signal, 'mi3_camera_preview', 'generando preview de camara', [
    { id: 'cp1', op: 'assistant.camera_preview', args: { width, height, samples: 1, out } },
  ], 30000);
  if (signal.aborted) return;

  const r0 = res.results?.[0] as OpResultEntry | undefined;
  const ok = !!r0?.ok || (res.ops_ok === res.ops_total && !res.error);
  if (ok) {
    const relOut = typeof r0?.out === 'string' ? r0.out : out;
    const w = typeof r0?.w === 'number' ? r0.w : width;
    const h = typeof r0?.h === 'number' ? r0.h : height;
    p.emit({ type: 'preview', turn_id: turnId, path: relOut, w, h });
  }
  await narrate(p, turnId, signal, 'Aqui tienes un preview de la camara actual.', 3);
}

/**
 * RONDA B: mismo camino que la tool mi3_render_quality pero sin modelo. Hace el
 * snapshot real, la copia real y el render real en la instancia dedicada.
 */
async function scriptRenderQuality(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  if (!(await narrate(p, turnId, signal, 'Voy a renderizar la escena en calidad y a mirar el resultado.', 4))) return;

  const jobId = `${turnId.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 40)}_q1`;
  const onPhase = (phase: JobPhase, detail?: string, ms?: number) =>
    p.emit({ type: 'job', turn_id: turnId, job_id: jobId, phase, detail, ms });

  p.emit({ type: 'status', state: 'acting', turn_id: turnId });
  onPhase('snapshot');
  const res = await callOp(p, turnId, signal, 'mi3_render_quality', 'renderizando en calidad',
    [{ id: 'rs1', op: 'assistant.render_snapshot', args: { job_id: jobId } }], 30000);
  const r0 = res.results?.[0] as OpResultEntry | undefined;
  if (!r0 || !r0.ok) {
    onPhase('failed', 'snapshot');
    p.emit({ type: 'text', turn_id: turnId, text: `No pude preparar el render: ${String(r0?.error ?? 'sin resultado')}` });
    return;
  }

  const man = {
    job_id: String(r0.job_id), job_dir: String(r0.job_dir), scene: String(r0.scene),
    project_folder: String(r0.project_folder), project_file: String(r0.project_file),
    project_name: String(r0.project_name), install_dir: String(r0.install_dir),
    marker: Number(r0.marker), engine: Number(r0.engine), camera: String(r0.camera ?? ''),
  };

  try {
    const out = await runRenderJob(p, man, { width: 960, height: 540, spp: 64, denoise: true }, onPhase, signal);
    p.emit({ type: 'preview', turn_id: turnId, path: out.outRel, w: out.w, h: out.h, kind: 'quality' });
    p.emit({ type: 'text', turn_id: turnId, text: `Render listo: ${out.outAbs} (${out.w}x${out.h}, ${out.spp} spp, ${Math.round(out.ms / 1000)} s).` });
  } catch (e) {
    onPhase('failed', String(e instanceof Error ? e.message : e));
    p.emit({ type: 'text', turn_id: turnId, text: `El render fallo: ${String(e instanceof Error ? e.message : e)}` });
  }
}

// ---------------------------------------------------------------------------
// RONDA E: guion de memoria (para los e2e sin cuota). El fake construye su
// PROPIO MemoryStore con p.dir -- asi el e2e ejercita el modulo real, no una
// imitacion, y emite exactamente los mismos eventos tool{start/end} y
// memory{...} que la tool real (mi3_memory_save / mi3_memory_forget).
// ---------------------------------------------------------------------------

/** Estado compartido entre turnos de la sesion --fake: el ultimo id guardado, para "olvida" sin id explicito. */
export interface FakeMemCtx {
  memory: MemoryStore;
  lastId?: string;
}

function extractFactText(raw: string): string {
  const m = /(?:recuerda\s+que\s+|recuerda\s+|remember\s+that\s+|remember\s+|memoriza\s+que\s+|memoriza\s+)/i.exec(raw);
  const rest = m ? raw.slice(m.index + m[0].length) : raw;
  return rest.trim();
}

function extractForgetId(raw: string): string | undefined {
  // Quitamos las palabras disparadoras ANTES de buscar: "olvida" y "forget"
  // son ellas mismas cadenas de 6 letras [a-z] y matchearian el patron de id
  // si no se excluyen primero.
  const stripped = raw.replace(/\bolvida(lo|te)?\b/gi, ' ').replace(/\bforget\b/gi, ' ');
  const m = /\b([0-9a-z]{6})\b/i.exec(stripped);
  return m ? m[1].toLowerCase() : undefined;
}

async function scriptMemoriaRecuerda(p: Protocol, turnId: string, signal: AbortSignal, mem: FakeMemCtx, rawText: string): Promise<void> {
  if (!(await narrate(p, turnId, signal, 'Voy a apuntar eso para futuras sesiones.', 4))) return;
  if (signal.aborted) return;

  const factText = extractFactText(rawText);
  const summary = 'recordando (global)';
  const t0 = Date.now();
  p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_save', phase: 'start', summary });
  const r = mem.memory.save('global', factText);
  const ms = Date.now() - t0;

  if (r.error) {
    p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_save', phase: 'end', ok: false, ms, summary });
    p.emit({ type: 'memory', turn_id: turnId, action: 'error', scope: 'global', message: r.error });
    await narrate(p, turnId, signal, `No pude guardarlo: ${r.error}`, 4);
    return;
  }

  p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_save', phase: 'end', ok: true, ms, summary });
  p.emit({ type: 'memory', turn_id: turnId, action: 'saved', scope: 'global', id: r.id, text: factText });
  mem.lastId = r.id;
  if (r.pruned.length > 0) {
    p.emit({ type: 'memory', turn_id: turnId, action: 'pruned', scope: 'global', ids: r.pruned.join(';'), n: r.pruned.length });
  }
  await narrate(p, turnId, signal, `Guardado: "${factText}".`, 3);
}

async function scriptMemoriaOlvida(p: Protocol, turnId: string, signal: AbortSignal, mem: FakeMemCtx, rawText: string): Promise<void> {
  const id = extractForgetId(rawText) ?? mem.lastId;
  if (!id) {
    await narrate(p, turnId, signal, 'No se que memoria olvidar: dime el id o guarda algo primero.', 6);
    return;
  }

  const summary = `olvidando ${id}`;
  const t0 = Date.now();
  p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_forget', phase: 'start', summary });
  const r = mem.memory.forget(id);
  const ms = Date.now() - t0;

  if (!r) {
    p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_forget', phase: 'end', ok: false, ms, summary });
    p.emit({ type: 'memory', turn_id: turnId, action: 'error', id, message: `no existe ninguna memoria con id ${id}` });
    await narrate(p, turnId, signal, `No existe ninguna memoria con id ${id}.`, 4);
    return;
  }

  p.emit({ type: 'tool', turn_id: turnId, name: 'mi3_memory_forget', phase: 'end', ok: true, ms, summary });
  p.emit({ type: 'memory', turn_id: turnId, action: 'forgotten', scope: r.scope, id, text: r.text });
  if (mem.lastId === id) mem.lastId = undefined;
  await narrate(p, turnId, signal, `Olvidado: "${r.text}".`, 3);
}

/**
 * RONDA J: guion de "fondo de estudio" (para el e2e de frescura sin gastar
 * cuota, gate J-T6 / J5.2): scene_summary -> assistant.background_set (receta
 * MEDIDA del cheat sheet 14: cielo #FFFFFF, suelo block/white_concrete, nubes
 * apagadas) -> preview de camara -> texto final que confirma el fondo de
 * estudio blanco y el Ctrl+Z unico.
 */
async function scriptFondo(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  await callOp(p, turnId, signal, 'mi3_scene_summary', 'leyendo la escena', [
    { id: 's1', op: 'assistant.scene_summary', args: { max_objects: 200 } },
  ]);
  if (signal.aborted) return;

  await callOp(p, turnId, signal, 'mi3_background_set', 'cambiando el fondo del proyecto', [
    {
      id: 'bg1',
      op: 'assistant.background_set',
      args: { sky_color: 'FFFFFF', ground_block: 'block/white_concrete', clouds_show: 0 },
    },
  ]);
  if (signal.aborted) return;

  const width = 480;
  const height = 270;
  const out = `preview/${turnId.replace(/[^A-Za-z0-9_-]/g, '_')}_1.png`;
  const res = await callOp(
    p,
    turnId,
    signal,
    'mi3_camera_preview',
    'generando preview de camara',
    [{ id: 'cp1', op: 'assistant.camera_preview', args: { width, height, samples: 1, out } }],
    30000,
  );
  if (signal.aborted) return;
  const r0 = res.results?.[0] as OpResultEntry | undefined;
  const ok = !!r0?.ok || (res.ops_ok === res.ops_total && !res.error);
  if (ok) {
    const relOut = typeof r0?.out === 'string' ? r0.out : out;
    const w = typeof r0?.w === 'number' ? r0.w : width;
    const h = typeof r0?.h === 'number' ? r0.h : height;
    p.emit({ type: 'preview', turn_id: turnId, path: relOut, w, h });
  }

  const text =
    'Listo: puse un fondo de estudio blanco (cielo blanco, suelo gris muy claro, nubes apagadas). ' +
    'Todo el cambio es un solo Ctrl+Z para ti; no toque luces, camara ni objetos.';
  await narrate(p, turnId, signal, text, 6);
}

async function scriptUndo(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  await callOp(p, turnId, signal, 'mi3_undo', 'deshaciendo 1 paso(s)', [{ id: 'u1', op: 'edit.undo' }]);
  if (signal.aborted) return;
  await narrate(p, turnId, signal, 'Deshecho el ultimo cambio.', 2);
}

async function scriptFallback(p: Protocol, turnId: string, signal: AbortSignal): Promise<void> {
  const lorem =
    'No tengo un guion especifico para ese mensaje, asi que esto es una respuesta de relleno ' +
    'fija del cerebro falso: sin red, sin modelo, sin cuota, solo para probar el protocolo.';
  await narrate(p, turnId, signal, lorem, 8);
}

function pickScript(
  text: string,
  mem: FakeMemCtx,
): (p: Protocol, turnId: string, signal: AbortSignal) => Promise<void> {
  const t = text.toLowerCase();
  // RONDA E: comprobar "olvida" antes que "memoria" para que una frase como
  // "olvida esa memoria" no caiga en el guion de guardado.
  if (includesAny(t, ['olvida', 'forget'])) return (p, turnId, signal) => scriptMemoriaOlvida(p, turnId, signal, mem, text);
  if (includesAny(t, ['recuerda', 'memoria', 'remember', 'memoriza']))
    return (p, turnId, signal) => scriptMemoriaRecuerda(p, turnId, signal, mem, text);
  // RONDA F: antes de 'escena' -- "agrega 4 jugadores a la escena" debe crear,
  // no solo leer.
  if (includesAny(t, ['crea', 'agrega', 'jugadores'])) return scriptCrear;
  // RONDA J: antes de 'escena' (mismo precedente que 'crea' en la ronda F) --
  // "pon un fondo de estudio" no debe caer en el guion generico de lectura.
  if (includesAny(t, ['fondo', 'estudio', 'backdrop', 'cielo'])) return scriptFondo;
  if (includesAny(t, ['escena', 'scene'])) return scriptEscena;
  if (includesAny(t, ['cubo', 'mueve'])) return scriptMoveCube;
  if (includesAny(t, ['preview', 'foto'])) return scriptPreview;
  if (includesAny(t, ['calidad', 'quality'])) return scriptRenderQuality;
  if (includesAny(t, ['undo'])) return scriptUndo;
  return scriptFallback;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

export async function runFake(p: Protocol): Promise<void> {
  p.beat('starting', { model: 'fake' });
  p.log('cerebro FALSO iniciado (--fake): sin red, sin modelo, sin cuota');

  let modelLabel = 'fake';
  let currentTurnId: string | undefined;
  let currentAbort: AbortController | undefined;

  // RONDA E: el fake construye su PROPIO MemoryStore con p.dir -- el e2e
  // ejercita el modulo real, no una imitacion. mem.lastId sobrevive entre
  // turnos de esta misma sesion (para que "olvida" sin id explicito funcione).
  const mem: FakeMemCtx = { memory: new MemoryStore(p.dir, (m) => p.log(m)) };

  const queue = new AsyncQueue<InRecord>();

  p.onIn((rec) => {
    if (rec.type === 'stop') {
      if (rec.turn_id === currentTurnId && currentAbort) {
        p.log(`fake: stop recibido para turno activo ${rec.turn_id}`);
        currentAbort.abort();
      } else {
        p.log(`fake: stop para turno inactivo/desconocido ${rec.turn_id} (activo=${currentTurnId ?? '(ninguno)'})`);
      }
      return; // stop takes effect immediately; it is never queued
    }
    queue.push(rec);
  });

  for (;;) {
    const rec = await queue.take();

    if (rec.type === 'model') {
      modelLabel = rec.model;
      p.log(`fake: modelo (cosmetico) cambiado a ${modelLabel}`);
      p.beat('idle', { model: modelLabel });
      continue;
    }
    if (rec.type === 'hello') {
      mem.memory.setProject(rec.project); // RONDA E
      p.emit({ type: 'status', state: 'ready', model: modelLabel });
      p.beat('idle', { model: modelLabel });
      continue;
    }
    if (rec.type !== 'user') continue;
    if (rec.project_file !== undefined) mem.memory.setProject(rec.project_file); // RONDA E

    currentTurnId = rec.turn_id;
    currentAbort = new AbortController();
    const signal = currentAbort.signal;

    p.emit({ type: 'status', state: 'thinking', turn_id: rec.turn_id, model: modelLabel });
    p.beat('thinking', { model: modelLabel });

    try {
      const script = pickScript(rec.text, mem);
      await script(p, rec.turn_id, signal);

      if (signal.aborted) {
        p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'stopped' });
      } else {
        p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'end_turn', cost_usd: 0 });
      }
    } catch (e) {
      if (e instanceof OpsAbortedError || signal.aborted) {
        p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'stopped' });
      } else {
        const message = e instanceof Error ? e.message : String(e);
        p.emit({ type: 'error', turn_id: rec.turn_id, message });
        p.emit({ type: 'done', turn_id: rec.turn_id, reason: 'error' });
      }
    }

    currentTurnId = undefined;
    currentAbort = undefined;
    p.beat('idle', { model: modelLabel });
  }
}
