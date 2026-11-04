// ASSISTANT/src/tools.ts
//
// The 8 in-process MCP tools the model uses to look at and edit the live
// render-llm scene. See SPEC_ASSISTANT_RONDA_A.md section 5.3 for the
// normative table of tool names / zod shapes / ops sent.
//
// Every handler follows the same shape (spec 5.3):
//   1. emit chat_out {type:'tool', phase:'start', name, summary}
//   2. await p.runOps(...)
//   3. emit chat_out {type:'tool', phase:'end', ok, ms}
//   4. (camera_preview only) emit {type:'preview', path, w, h}
//   5. return the result as text -- NEVER throw. A failure becomes
//      { isError: true, content: [...] } so the model can react to it.

import fs from 'node:fs';
import path from 'node:path';
import { createSdkMcpServer, tool } from '@anthropic-ai/claude-agent-sdk';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import { z } from 'zod';
import { Protocol, sleep, type Op, type OpResultEntry, type OpsRes } from './protocol.js';
import { TRIM_EFFECTS_WITH_NONE } from './trimEffects.js';

// ShinyTrims O5b (decision 3 del PO): el nombre del efecto se valida AQUI, en
// el schema, no dentro del engine. Motivo medido: mi3_trim_fx_id() devuelve 0
// tanto para 'none' como para cualquier nombre desconocido, y ese 0 significa
// "sin efecto" -- asi que un effect:'noexiste_xyz' del modelo BORRABA el efecto
// de las cuatro piezas y respondia ok. La lista sale del catalogo generado
// (trimEffects.ts), no de una lista a mano: anadir un efecto al pack lo pone
// aqui sin tocar este fichero, y gen_trim_effects.py --check caza la deriva.
// El op headless tiene la MISMA validacion (mi3_trim_fx_valid): esto es la
// primera puerta, no la unica.
const zTrimEffect = z.enum(TRIM_EFFECTS_WITH_NONE);
import {
  EXPORT_LIMITS,
  LIMITS,
  RenderAborted,
  RenderError,
  runExportJob,
  runRenderJob,
  type ExportPlan,
  type RenderManifest,
} from './render.js';
import type { MemoryStore } from './memory.js';
import { GAP_LIMITS, type GapLog } from './gap.js';
import {
  checkPathCage,
  dataRootFromAssistantDir,
  particlesRootFromAssistantDir,
  resolveAttachmentPath,
  type DesignatedRoots,
} from './paths.js';
import { ProjectFileGuard, type GuardConfirm, type GuardOp } from './projectFile.js';
import { RENDER_LLM_SHORT_NAMES, type ActionTurnCtx } from './renderllm-tools.js';
export { RENDER_LLM_TOOL_NAMES, type ActionTurnCtx } from './renderllm-tools.js';
import {
  cancelColorBatchAction,
  cancelStillAction,
  cancelVideoAction,
  renderLlmActionJson,
  runColorBatchAction,
  runRenderLlmAction,
  runStillAction,
  runVideoAction,
  startColorBatchAction,
  startStillAction,
  startVideoAction,
  statusColorBatchAction,
  statusStillAction,
  statusVideoAction,
} from './renderllm-actions.js';

/** Per-turn state shared by every tool call made during that turn. */
export interface TurnCtx {
  turnId: string;
  /** Bumped by mi3_camera_preview so filenames don't collide within a turn. */
  previewN: number;
  /** RONDA B: renders de CALIDAD ya hechos en este turno (tope 3 blando / 6 duro). */
  qualityN: number;
  /** The turn's AbortController signal -- lets a STOP unblock a stuck runOps() promptly. */
  signal?: AbortSignal;
  /** RONDA E: almacen de memoria del turno (lo crea index.ts una sola vez). */
  memory: MemoryStore;
  /** RONDA F: objetos creados o duplicados en este turno. */
  createdN: number;
  /** RONDA K0: buzon de huecos (lo crea index.ts una sola vez, como memory). */
  gapLog: GapLog;
  /** RONDA K0: modelo activo de este turno, para gap_log.jsonl. */
  model: string;
  /** RONDA K0: nombre de fichero del proyecto abierto, "" si nunca se ha guardado. */
  projectName: string;
  /** RONDA K2: keyframes creados/borrados/movidos/retransicionados/pegados en este turno. */
  keyframeWritesN: number;
  /** RONDA K4: carpeta Data/assistant (=cfg.dir) -- para resolver adjuntos y la raiz "data". */
  dataDir: string;
  /** RONDA K4 (jaula de rutas, 2.3): el mensaje LITERAL del usuario de ESTE turno, y
   *  NADA MAS (nunca memoria, nunca un resultado de tool). Ver paths.ts. */
  userText: string;
  /** RONDA K4: imports (mi3_import op != "library") hechos en este turno. */
  importsN: number;
  /** RONDA K4: listados (mi3_import op:"library") hechos en este turno. */
  libraryListsN: number;
  /** RONDA K5: ruta COMPLETA del .miproject abierto ("" si nunca se ha
   *  guardado) -- a diferencia de projectName (solo el basename, para
   *  gap_log.jsonl), esto es lo que el guardarrail de mi3_project_file usa
   *  para detectar "el proyecto abierto cambio" entre dos turnos. */
  projectFile: string;
  /** RONDA K5: contador MONOTONO de turnos de usuario de esta sesion de
   *  proceso (lo sube index.ts una vez por turno, ANTES de construir este
   *  ctx) -- es lo que hace demostrable "la confirmacion llego en un turno
   *  POSTERIOR" del guardarrail de mi3_project_file (los turn_id del
   *  protocolo son opacos, no sirven para eso). Ver projectFile.ts. */
  turnOrdinal: number;
  /** RONDA K5: almacen del guardarrail de dos turnos (una unica instancia por
   *  proceso, igual que memory/gapLog -- la crea index.ts una sola vez). */
  projectGuard: ProjectFileGuard;
  /** RONDA K5: exportaciones (mi3_export) hechas en este turno. */
  exportsN: number;
  /** RONDA K5: operaciones de abrir/nuevo/restaurar/guardar-como
   *  (mi3_project_file) hechas en este turno -- "save" NO cuenta (spec 2.4). */
  projectFileOpsN: number;
  /** RONDA K6: exportaciones de VIDEO (mi3_export op:"movie") hechas en este
   *  turno -- tope APARTE de exportsN, mas restrictivo (spec 2.4,
   *  CAPS.moviePerTurn). Cuenta con independencia de "format" (incluso si
   *  degrada a PNG, el render ya se hizo). */
  movieN: number;
}

/** RONDA F: tope de objetos creados/duplicados por turno (mismo espiritu que
 *  LIMITS.softPerTurn/hardPerTurn de render_quality en la ronda B: el tope vive
 *  en el CODIGO, no solo en el rol). */
export const CREATE_CAP_PER_TURN = 12;

/** RONDA K2 (spec seccion 2.4): tope de escrituras de keyframe por turno --
 *  cada frame borrado/movido/retransicionado/pegado cuenta 1. El tope vive en
 *  el CODIGO (esta constante), no en el rol. */
export const KEYFRAME_WRITES_CAP_PER_TURN = 80;

/** RONDA K4 (spec seccion 2.4): mi3_import puede ser lento (una caja de mundo
 *  entera) -- tope bajo, en el CODIGO. */
export const IMPORTS_CAP_PER_TURN = 4;

/** RONDA K4 (spec seccion 2.4): listados de biblioteca (mi3_import op:"library")
 *  -- barato, pero sigue siendo E/S de disco del usuario; tope aparte de imports. */
export const LIBRARY_LISTS_CAP_PER_TURN = 3;

/** RONDA K5 (spec seccion 2.4): "imagen o secuencia" (y, en esta oleada,
 *  white_details/layers, que tambien renderizan) por turno. */
export const EXPORTS_CAP_PER_TURN = 2;

/** RONDA K5 (spec seccion 2.4): abrir/nuevo/restaurar-checkpoint/guardar-como
 *  por turno -- "guardar" (op:"save") NO cuenta contra este tope. */
export const PROJECT_FILE_OPS_CAP_PER_TURN = 1;

/** RONDA K6 (spec seccion 2.4): "movie" (video) por turno -- APARTE del tope
 *  compartido EXPORTS_CAP_PER_TURN, y mas bajo: un video es un render por
 *  cada frame de salida, lo mas caro que hace mi3_export. */
export const MOVIE_CAP_PER_TURN = 1;

const TOOL_SHORT_NAMES = [
  'mi3_scene_summary',
  'mi3_object_get',
  'mi3_set_values',
  'mi3_keyframe_add',
  'mi3_camera_preview',
  'mi3_render_quality',
  'mi3_checkpoint',
  'mi3_undo',
  'mi3_wait_idle',
  'mi3_screen_layout',
  'mi3_memory_save',
  'mi3_memory_list',
  'mi3_memory_forget',
  'mi3_object_add',
  'mi3_object_delete',
  'mi3_object_duplicate',
  'mi3_background_set',
  'mi3_report_gap',
  'mi3_project_set',
  'mi3_timeline',
  'mi3_keyframes',
  'mi3_object_edit',
  'mi3_redo',
  'mi3_import',
  'mi3_export',
  'mi3_project_file',
  ...RENDER_LLM_SHORT_NAMES,
] as const;

/** Fully-qualified names as the model sees them: mcp__<server>__<tool>. */
export const MI3_TOOL_NAMES: string[] = TOOL_SHORT_NAMES.map((n) => `mcp__mi3__${n}`);

// ---------------------------------------------------------------------------
// Shared result formatting
// ---------------------------------------------------------------------------

/** Render one op result entry as readable "key: value" lines, generic to whatever fields the editor's op included. */
function formatOpResult(result: Record<string, unknown> | undefined): string {
  if (!result) return '(no result)';
  const skip = new Set(['id', 'op', 'ok']);
  const lines: string[] = [];
  for (const [k, v] of Object.entries(result)) {
    if (skip.has(k)) continue;
    if (v === undefined) continue;
    if (typeof v === 'string' && v.includes('\n')) {
      lines.push(`${k}:\n${v}`);
    } else if (typeof v === 'object' && v !== null) {
      lines.push(`${k}: ${JSON.stringify(v)}`);
    } else {
      lines.push(`${k}: ${String(v)}`);
    }
  }
  return lines.join('\n');
}

/** Turn a whole OpsRes (possibly several ops) into one text block for the model. */
function summarize(res: OpsRes): { ok: boolean; text: string } {
  const parts: string[] = [];

  if (res.error) {
    parts.push(`Editor error: ${res.error}`);
  }

  if (res.checkpoint) {
    if (res.checkpoint.created) {
      parts.push(`Checkpoint created: ${res.checkpoint.cp_id ?? '?'} (${res.checkpoint.name ?? 'assistant'})`);
    } else if (res.checkpoint.error) {
      const hint =
        res.checkpoint.error === 'nofolder'
          ? 'the project has never been saved, so there is nothing to roll back to yet. Tell the user to press Ctrl+S before you make changes.'
          : 'the undo safety net is degraded for this turn -- proceed carefully and say so.';
      parts.push(`Checkpoint NOT created (${res.checkpoint.error}): ${hint}`);
    }
  }

  const results = res.results ?? [];
  for (const r of results) {
    const rr = r as OpResultEntry;
    const label = String(rr.op ?? rr.id ?? '?');
    if (!rr.ok) {
      parts.push(`${label}: FAILED -- ${String(rr.error ?? 'unknown error')}`);
    } else {
      const body = formatOpResult(rr);
      parts.push(body ? `${label}:\n${body}` : `${label}: ok`);
    }
  }

  const allOk = !res.error && res.ops_total === res.ops_ok && results.every((r) => !!(r as OpResultEntry).ok);
  return { ok: allOk, text: parts.join('\n\n') || '(empty result)' };
}

function summarizeAction(res: OpsRes): {
  ok: boolean;
  text: string;
  structuredContent: Record<string, unknown>;
} {
  const summary = summarize(res);
  return { ...summary, structuredContent: renderLlmActionJson(res) };
}

function truthy(v: unknown): boolean {
  if (typeof v === 'boolean') return v;
  if (typeof v === 'number') return v !== 0;
  if (typeof v === 'string') return v !== '' && v !== '0';
  return !!v;
}

/** Tope de tamano del PNG que se adjunta al resultado de una tool (4 MB). */
const MAX_INLINE_PNG_BYTES = 4 * 1024 * 1024;

/**
 * RONDA B (verificacion): devuelve el PNG como BLOQUE DE IMAGEN del resultado
 * de la tool, para que el modelo vea los pixeles SIEMPRE.
 *
 * Por que: medido con claude-haiku-4-5 en el gate B5, el modelo NO llamaba a
 * Read tras un render de calidad (cero lineas "permiso: Read ..." en
 * assistant.log en dos turnos seguidos) y aun asi describia la luz -- una
 * descripcion inventada a partir de los metadatos del texto. Ni el texto de la
 * tool ("Use the Read tool on this exact path") ni el cierre imperativo
 * ("Your very next tool call MUST be Read") lo cambiaron. Pedirle que mire es
 * una sugerencia; adjuntar la imagen es un hecho.
 *
 * Nunca es fatal: si el fichero no se puede leer o es enorme, se cae al
 * comportamiento anterior (solo texto + la ruta, que el modelo puede Read).
 */
function readPngForModel(p: Protocol, absPath: string): { type: 'image'; data: string; mimeType: string } | undefined {
  try {
    const st = fs.statSync(absPath);
    if (!st.isFile() || st.size > MAX_INLINE_PNG_BYTES) {
      p.log(`imagen NO adjuntada al resultado (${st.size} bytes > ${MAX_INLINE_PNG_BYTES}): ${absPath}`);
      return undefined;
    }
    const data = fs.readFileSync(absPath).toString('base64');
    p.log(`imagen adjuntada al resultado de la tool (${st.size} bytes): ${absPath}`);
    return { type: 'image', data, mimeType: 'image/png' };
  } catch (e) {
    p.log(`WARN no se pudo adjuntar la imagen ${absPath}: ${String(e)}`);
    return undefined;
  }
}

/**
 * Wrap a tool body with the start/end chat_out events and the "never throw"
 * guarantee (spec 5.3, point 5). `body` returns {ok, text} (y, opcionalmente,
 * `imagePath` para adjuntar un PNG al resultado); runTool turns that into a
 * CallToolResult and always emits the matching tool/end event, even when
 * `body` throws (e.g. OpsTimeoutError / OpsAbortedError).
 */
async function runTool(
  p: Protocol,
  ctx: ActionTurnCtx,
  name: string,
  startSummary: string,
  body: () => Promise<{
    ok: boolean;
    text: string;
    imagePath?: string;
    imagePaths?: string[];
    structuredContent?: Record<string, unknown>;
  }>,
): Promise<CallToolResult> {
  const t0 = Date.now();
  p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'start', summary: startSummary });
  try {
    const { ok, text, imagePath, imagePaths, structuredContent } = await body();
    const ms = Date.now() - t0;
    // RONDA D (streaming fino): "summary" viaja tambien en "end" -- antes solo
    // iba en "start" y la pestana (que solo pinta "end", con el timing) no
    // tenia forma de mostrar QUE hizo la tool sin correlacionar dos eventos.
    p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'end', ok, ms, summary: startSummary });
    // RONDA F (diagnostico, gate F2.b): el chat_out tool{end} solo lleva el
    // resumen de INICIO + timing -- el TEXTO completo que ve el modelo (p.ej.
    // el aviso de tope) no viajaba a ningun sitio observable desde fuera del
    // proceso del SDK. assistant.log es "diagnostic convenience" por diseno
    // (protocol.ts); una linea por tool aqui lo hace verificable sin tocar
    // el protocolo chat_out ni el contrato de la tool.
    p.log(`tool result ${name} ok=${ok}: ${text.slice(0, 500)}${text.length > 500 ? '...' : ''}`);
    const content: CallToolResult['content'] = [{ type: 'text', text }];
    const img = imagePath ? readPngForModel(p, imagePath) : undefined;
    if (img) content.push(img);
    for (const candidate of imagePaths ?? []) {
      const batchImage = readPngForModel(p, candidate);
      if (batchImage) content.push(batchImage);
    }
    return { content, isError: !ok, ...(structuredContent ? { structuredContent } : {}) };
  } catch (e) {
    const ms = Date.now() - t0;
    const message = e instanceof Error ? e.message : String(e);
    p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'end', ok: false, ms, summary: startSummary });
    return { content: [{ type: 'text', text: `ERROR: ${message}` }], isError: true };
  }
}

// ---------------------------------------------------------------------------
// RONDA K4: jaula de rutas (spec seccion 2.3) -- helpers compartidos por
// mi3_import y por mi3_object_add (kind:"model"/"particles", que tambien
// traen algo de fuera aunque el usuario los pida como si fueran un objeto
// mas). Nunca se cachea entre llamadas: el proyecto abierto puede cambiar de
// un turno a otro (y en K5, dentro del mismo turno via mi3_project_file).
// ---------------------------------------------------------------------------

/**
 * Lee las raices "projects"/"project" del EDITOR (assistant.paths, estado
 * vivo) y completa attachments/data/exports relativas a --dir. Nunca se
 * cachea entre llamadas: el proyecto abierto puede cambiar de un turno a otro
 * (y en K5, dentro del mismo turno via mi3_project_file).
 *
 * RESCATE K5 -- CORRECCION DEL DIAGNOSTICO. La version anterior de este
 * comentario decia que 10000ms se quedaba corto por "contienda de fichero al
 * publicar ops_res.json". Era FALSO y se comprobo midiendo el fichero que el
 * editor dejo en disco: la respuesta llegaba en 9 ms (`"ms": 9` en el propio
 * ops_res.json) y estaba ahi todo el rato -- pero traia
 * `project_file: "C:\Users\example\..."` SIN ESCAPAR, o sea JSON invalido, asi que
 * `JSON.parse` fallaba en cada sondeo y runOpsInner agotaba el timeout. El
 * arreglo real vive en el editor (mi3_hl_json_save_var) mas la normalizacion
 * a "/" de toEditorPath. Se deja 30000ms (igual que assistant.render_snapshot)
 * porque este op tambien puede caer detras de una carga de proyecto larga,
 * pero YA NO es lo que sostiene el gate.
 */
/**
 * RESCATE K5: toda ruta que CRUZA al editor viaja con "/" -- nunca con "\".
 * Dos razones, las dos medidas:
 *   1. El editor GUARDA lo que le mandamos (project.open/project.save fijan
 *      `project_file` con la cadena tal cual) y luego lo DEVUELVE en
 *      assistant.paths. Con "\" el resultado salia como JSON invalido y el
 *      turno moria por timeout (ver mi3_hl_json_save_var; el editor ya no
 *      puede producir JSON invalido, pero no hay razon para volver a
 *      alimentarlo con separadores que el no usa: `project_folder` y
 *      `setting_project_folder` son de "/" nativamente).
 *   2. Es exactamente lo que render.ts ya hacia para los op-scripts de render
 *      (`.replace(/\\/g, '/')` en cada ruta del plan); esto solo lo extiende
 *      al canal vivo.
 * Windows acepta "/" en cualquier API de fichero, asi que no cambia nada mas.
 */
function toEditorPath(pth: string): string {
  return pth.replace(/\\/g, '/');
}

async function getDesignatedRoots(p: Protocol, ctx: TurnCtx): Promise<DesignatedRoots> {
  const res = await p.runOps([{ id: 'paths1', op: 'assistant.paths' }], ctx.turnId, 30000, ctx.signal);
  const r = (res.results ?? [])[0] as OpResultEntry | undefined;
  const projects = r && typeof r.projects_folder === 'string' ? (r.projects_folder as string) : '';
  const project = r && typeof r.project_folder === 'string' ? (r.project_folder as string) : '';
  return {
    projects,
    project,
    attachments: path.resolve(ctx.dataDir, 'attachments', ctx.turnId.replace(/[^A-Za-z0-9_-]/g, '_')),
    data: dataRootFromAssistantDir(ctx.dataDir),
    exports: project ? path.join(project, 'exports') : '',
    // RONDA L (H3a): sexta raiz designada -- <instalacion>/Particles, hermana
    // de Data/, NO dentro de ella (Data/Particles no existe en ninguna
    // instalacion, medido).
    particles: particlesRootFromAssistantDir(ctx.dataDir),
  };
}

/**
 * Resuelve el "asset"/"preset" de mi3_object_add (kind:"model"/"particles")
 * a una ruta VALIDADA por la jaula. Camino preferente: adjunto de ESTE turno
 * (no necesita ruta ninguna, es el mismo mecanismo de skin/G2). Si no,
 * "asset"/"preset" se trata como una ruta que tiene que cumplir (a) o (b)
 * de la jaula -- lo mas comun es un nombre pelado devuelto por
 * mi3_import op:"library" kind:"project_assets"/"particles", que cae solo
 * dentro de la raiz correspondiente.
 */
async function resolveAssetPath(
  p: Protocol,
  ctx: TurnCtx,
  kind: 'model' | 'particles',
  nameOrAttachment: string,
): Promise<{ ok: true; path: string } | { ok: false; text: string }> {
  const attached = resolveAttachmentPath(ctx.dataDir, ctx.turnId, nameOrAttachment);
  if (attached) return { ok: true, path: attached };

  const roots = await getDesignatedRoots(p, ctx);
  // "particles" ademas admite el nombre PELADO relativo a <instalacion>/Particles/
  // directamente (no hace falta que el usuario lo haya escrito ni que sea
  // un adjunto: es un preset builtin de la instalacion, raiz "particles" --
  // RONDA L (H3a): antes apuntaba a Data/Particles/, que no existe en
  // ninguna instalacion).
  let bareName = nameOrAttachment;
  if (kind === 'particles' && !bareName.toLowerCase().endsWith('.miparticles')) bareName += '.miparticles';
  const candidate =
    kind === 'particles' && !path.isAbsolute(nameOrAttachment) && !nameOrAttachment.includes('/') && !nameOrAttachment.includes('\\')
      ? path.join(roots.particles, bareName)
      : nameOrAttachment;
  const cage = checkPathCage(candidate, ctx.userText, roots);
  if (!cage.ok || !cage.resolved) {
    return {
      ok: false,
      text:
        `No puedo usar "${nameOrAttachment}" para ${kind}: ${cage.reason}. Usa un adjunto de este turno, ` +
        'o pide primero mi3_import op:"library" para ver lo que hay disponible.',
    };
  }
  if (!fs.existsSync(cage.resolved)) {
    return { ok: false, text: `No existe el fichero: ${cage.resolved}` };
  }
  return { ok: true, path: cage.resolved };
}

// ---------------------------------------------------------------------------
// RONDA K5: helpers compartidos por mi3_export (nombre de fichero) y
// mi3_project_file (resolucion de proyecto por nombre + guardarrail de dos
// turnos). Ver SPEC_ASSISTANT_RONDA_K.md seccion 3 y 7.7/7.8.
// ---------------------------------------------------------------------------

/**
 * Nombre de fichero SANEADO a partir de lo que pida el modelo -- nunca una
 * ruta. Toma solo el ULTIMO segmento tras normalizar separadores (asi
 * "..\..\evil" se convierte en "evil": nunca puede escapar la carpeta que ya
 * eligio el anfitrion, gate G5.7), quita ".." sueltos, colapsa a un alfabeto
 * seguro y cae a `fallback` si queda vacio.
 */
export function sanitizeFileStem(raw: string | undefined, fallback = 'export'): string {
  let s = (raw ?? '').trim();
  if (!s) return fallback;
  s = s.replace(/\\/g, '/');
  const seg = s.split('/').filter((part) => part !== '' && part !== '.');
  s = seg.length > 0 ? seg[seg.length - 1] : s;
  s = s.replace(/\.\./g, '_');
  s = s.replace(/[^A-Za-z0-9 _.-]/g, '_');
  s = s.replace(/^[.\s]+/, '').replace(/[.\s]+$/, '');
  if (!s) return fallback;
  return s.slice(0, 60);
}

/** assistant.ping -> ?estado sucio? (spec 3.1: "si el proyecto NO esta
 *  sucio, no hay pregunta"). */
async function checkProjectDirty(p: Protocol, ctx: TurnCtx): Promise<{ dirty: boolean; objects?: number }> {
  const res = await p.runOps([{ id: 'pfping', op: 'assistant.ping' }], ctx.turnId, 15000, ctx.signal);
  const r0 = res.results?.[0] as OpResultEntry | undefined;
  return {
    dirty: truthy(r0?.changed),
    objects: typeof r0?.objects === 'number' ? (r0.objects as number) : undefined,
  };
}

/**
 * Resuelve "name" (o "path") de mi3_project_file op:"open" a una ruta .miproject
 * VALIDADA. Camino (a): ruta literal del mensaje de este turno o dentro de una
 * raiz designada (checkPathCage, igual que mi3_import). Camino (b): nombre
 * PELADO relativo a la raiz "projects" -- <projects>/<name>/<name>.miproject
 * (la convencion de project.new) o <projects>/<name>.miproject -- SOLO si el
 * fichero existe de verdad (el modelo no puede listar disco por su cuenta;
 * si no acierta, se le remite a op:"projects").
 */
async function resolveProjectRef(
  p: Protocol,
  ctx: TurnCtx,
  nameOrPath: string,
): Promise<{ ok: true; path: string } | { ok: false; text: string }> {
  const roots = await getDesignatedRoots(p, ctx);
  const cage = checkPathCage(nameOrPath, ctx.userText, roots);
  if (cage.ok && cage.resolved && cage.resolved.toLowerCase().endsWith('.miproject')) {
    return { ok: true, path: cage.resolved };
  }
  if (!roots.projects) {
    return { ok: false, text: 'No hay una carpeta de proyectos configurada.' };
  }
  const projectsRoot = path.resolve(roots.projects);
  const bare = nameOrPath.replace(/[\\/]+$/, '');
  const candidates = [
    path.join(projectsRoot, bare, `${bare}.miproject`),
    path.join(projectsRoot, bare.toLowerCase().endsWith('.miproject') ? bare : `${bare}.miproject`),
  ];
  for (const c of candidates) {
    const resolved = path.resolve(c);
    const rl = resolved.toLowerCase();
    const rootL = projectsRoot.toLowerCase();
    if (rl !== rootL && !rl.startsWith(rootL + path.sep)) continue;
    if (fs.existsSync(resolved)) return { ok: true, path: resolved };
  }
  return {
    ok: false,
    text: `No encuentro un proyecto llamado "${nameOrPath}" en la carpeta de proyectos. Usa mi3_project_file op:"projects" para ver los disponibles.`,
  };
}

/** Resuelve el DESTINO de op:"save_as" -- no tiene por que existir todavia
 *  (es donde se va a CREAR); solo tiene que caer dentro de una raiz valida. */
async function resolveSaveAsDest(
  p: Protocol,
  ctx: TurnCtx,
  nameOrPath: string,
): Promise<{ ok: true; path: string } | { ok: false; text: string }> {
  const roots = await getDesignatedRoots(p, ctx);
  const cage = checkPathCage(nameOrPath, ctx.userText, roots);
  if (cage.ok && cage.resolved) {
    if (cage.resolved.toLowerCase().endsWith('.miproject')) return { ok: true, path: cage.resolved };
    const stem = sanitizeFileStem(path.basename(nameOrPath), 'proyecto');
    return { ok: true, path: path.join(cage.resolved, `${stem}.miproject`) };
  }
  if (!roots.projects) return { ok: false, text: 'No hay una carpeta de proyectos configurada.' };
  const stem = sanitizeFileStem(nameOrPath.replace(/\.miproject$/i, ''), 'proyecto');
  return { ok: true, path: path.join(path.resolve(roots.projects), stem, `${stem}.miproject`) };
}

/**
 * RONDA L (H3b): listado de proyectos hecho en el ANFITRION. El op del motor
 * es plano y no puede dejar de serlo (file_find_first solo devuelve ficheros:
 * QDirIterator con QDir::Files en CppProject/Gml/FileFunc.cpp, y el iterador
 * es global, no se pueden anidar dos file_find_*), y los proyectos reales
 * viven en <projects>/<nombre>/<nombre>.miproject -- la convencion que
 * project.new y resolveProjectRef ya usan. Mira la raiz y UN nivel de
 * subcarpetas, con el mismo tope 200 y el mismo formato "nombre|bytes;" que
 * devolvia el op, para que nada de lo que ya lee ese texto cambie. Sin
 * argumento de ruta: la raiz la da el editor (roots.projects, via
 * assistant.paths -- nunca del modelo).
 */
export function listProjectsHost(
  projectsRoot: string,
  cap = 200,
): { count: number; entries: string; truncated: boolean } {
  const out: string[] = [];
  let truncated = false;
  const push = (full: string, name: string): void => {
    if (out.length >= cap) {
      truncated = true;
      return;
    }
    let size = 0;
    try {
      size = fs.statSync(full).size;
    } catch {
      size = 0;
    }
    out.push(`${name}|${size}`);
  };
  let names: string[] = [];
  try {
    names = fs.readdirSync(projectsRoot);
  } catch {
    return { count: 0, entries: '', truncated: false };
  }
  names.sort((a, b) => a.localeCompare(b));
  for (const n of names) {
    if (out.length >= cap) {
      truncated = true;
      break;
    }
    const full = path.join(projectsRoot, n);
    let st: fs.Stats;
    try {
      st = fs.statSync(full);
    } catch {
      continue;
    }
    if (st.isFile() && n.toLowerCase().endsWith('.miproject')) {
      push(full, n);
      continue;
    }
    if (st.isDirectory()) {
      const inner = path.join(full, `${n}.miproject`);
      try {
        if (fs.statSync(inner).isFile()) push(inner, `${n}.miproject`);
      } catch {
        /* carpeta sin proyecto homonimo */
      }
    }
  }
  return { count: out.length, entries: out.length ? out.join(';') + ';' : '', truncated };
}

/** Formatea el resultado de listProjectsHost EXACTAMENTE como summarize()
 *  formatearia el op "assistant.library"/kind:"projects" real, para que el
 *  cambio de sitio (motor -> anfitrion) sea invisible para el modelo. */
function formatProjectsListing(opLabel: string, listing: { count: number; entries: string; truncated: boolean }): { ok: true; text: string } {
  const lines = [`count: ${listing.count}`, `entries: ${listing.entries}`];
  if (listing.truncated) lines.push('truncated: 1');
  return { ok: true, text: `${opLabel}:\n${lines.join('\n')}` };
}

/**
 * El mecanismo de dos turnos en si (spec seccion 3.1): si llega `confirm`,
 * SOLO se acepta cuando ctx.projectGuard tiene un pending para EXACTAMENTE
 * (op,target) registrado en un turno ANTERIOR -- checkConfirm() lo exige.
 * Si no llega `confirm`, se pregunta `needsGuard()`; si dice que hace falta,
 * se registra el pending y se rechaza sin tocar nada. Si no hace falta (op no
 * destructivo en este momento), se ejecuta directo.
 */
async function guardedProjectOp(
  p: Protocol,
  ctx: TurnCtx,
  opKind: GuardOp,
  target: string,
  verb: string,
  confirm: GuardConfirm | undefined,
  needsGuard: () => Promise<{ need: boolean; detail: string }>,
  execute: () => Promise<{ ok: boolean; text: string }>,
): Promise<{ ok: boolean; text: string }> {
  if (confirm) {
    const check = ctx.projectGuard.checkConfirm(opKind, target, ctx.turnOrdinal, ctx.projectFile);
    if (!check.ok) {
      return {
        ok: false,
        text:
          `No puedo aplicar esa confirmacion: ${check.reason}. Si el usuario de verdad quiere continuar, pidele que ` +
          'lo confirme otra vez en el chat y vuelve a intentarlo en tu SIGUIENTE turno.',
      };
    }
    ctx.projectGuard.clear();
    if (confirm === 'save_first') {
      const saveRes = await p.runOps([{ id: 'pfsave', op: 'project.save', args: {} }], ctx.turnId, 30000, ctx.signal);
      const saveSummary = summarize(saveRes);
      if (!saveSummary.ok) {
        return { ok: false, text: `No se pudo guardar antes de continuar:\n${saveSummary.text}` };
      }
    }
    ctx.projectFileOpsN++;
    return execute();
  }

  const g = await needsGuard();
  if (g.need) {
    ctx.projectGuard.register(opKind, target, ctx.turnId, ctx.turnOrdinal, ctx.projectFile);
    return {
      ok: false,
      text:
        `${g.detail} ${verb} "${target}" lo perderia.\n` +
        'PREGUNTALE al usuario, en el chat, que quiere hacer: (1) guardar antes, (2) continuar de todos modos ' +
        '(se pierde lo no guardado), (3) cancelar. Termina tu turno AHORA y espera su respuesta -- no vuelvas a ' +
        'llamar a esta tool en este mismo turno, la confirmacion tiene que venir de el. Cuando conteste, en un ' +
        'turno NUEVO, llama otra vez a esta misma operacion con confirm:"save_first" o confirm:"discard".',
    };
  }
  ctx.projectFileOpsN++;
  return execute();
}

// ---------------------------------------------------------------------------
// MCP server
// ---------------------------------------------------------------------------

function buildRenderLlmTools(p: Protocol, ctx: ActionTurnCtx) {
  return [
    tool(
      'scene_get',
      'Return the complete canonical render-llm document and its current revision. ' +
        'This is read-only. Read it before proposing a scene_apply so the write can use ' +
        'expected_revision and fail safely if another actor changed the document.',
      {},
      async () =>
        runTool(p, ctx, 'scene_get', 'reading the render-llm document', async () => {
          const res = await runRenderLlmAction(p, ctx, 'scene_get', {});
          return summarizeAction(res);
        }),
    ),
    tool(
      'scene_validate',
      'Validate and canonicalize a complete render-llm document without changing the open ' +
        'scene. Returns the canonical document on success and a schema or credential error ' +
        'on failure.',
      { document: z.string().min(1) },
      async (args) =>
        runTool(p, ctx, 'scene_validate', 'validating the render-llm document', async () => {
          const res = await runRenderLlmAction(p, ctx, 'scene_validate', { document: args.document });
          return summarizeAction(res);
        }),
    ),
    tool(
      'scene_apply',
      'Transactionally replace the supported live scene with a complete validated render-llm ' +
        'document. The write is undoable and must include the revision returned by scene_get; ' +
        'a stale revision is rejected before mutation.',
      {
        document: z.string().min(1),
        expected_revision: z.number().int().nonnegative(),
      },
      async (args) =>
        runTool(p, ctx, 'scene_apply', 'applying the render-llm document', async () => {
          const res = await runRenderLlmAction(p, ctx, 'scene_apply', {
            document: args.document,
            expected_revision: args.expected_revision,
          });
          return summarizeAction(res);
        }),
    ),
    tool(
      'undo',
      'Undo the most recent render-llm document transaction. Returns the restored canonical ' +
        'document and the newly advanced revision; fails without mutation if history is empty.',
      {},
      async () =>
        runTool(p, ctx, 'undo', 'undoing the render-llm transaction', async () => {
          const res = await runRenderLlmAction(p, ctx, 'undo', {});
          return summarizeAction(res);
        }),
    ),
    tool(
      'redo',
      'Redo the most recently undone render-llm document transaction. Returns the restored ' +
        'canonical document and the newly advanced revision; fails without mutation if redo ' +
        'history is empty.',
      {},
      async () =>
        runTool(p, ctx, 'redo', 'redoing the render-llm transaction', async () => {
          const res = await runRenderLlmAction(p, ctx, 'redo', {});
          return summarizeAction(res);
        }),
    ),
    tool(
      'batch_start',
      'Start four isolated color variants from the exact requested scene revision and return a job ID.',
      {
        object_id: z.string().min(1),
        colors: z.array(z.string().regex(/^#[0-9A-Fa-f]{6}$/)).length(4),
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920),
        height: z.number().int().min(180).max(1080),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean(),
      },
      async (args) =>
        runTool(p, ctx, 'batch_start', 'starting isolated color batch', async () => ({
          ok: true,
          text: 'Started revision-frozen color batch.',
          structuredContent: await startColorBatchAction(p, ctx, args),
        })),
    ),
    tool(
      'batch_status',
      'Read one color-batch job. A completed job returns all four actual PNGs.',
      { job_id: z.string().regex(/^batch-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'batch_status', 'reading color batch status', async () => {
          const result = statusColorBatchAction(p, args);
          const outputs = result.job.state === 'succeeded' && Array.isArray(result.job.result?.outputs)
            ? result.job.result.outputs as Array<Record<string, unknown>> : [];
          const imagePaths = outputs.map((entry) => String(entry.image ?? '')).filter(Boolean).map((image) => path.join(p.dir, image));
          return {
            ok: true,
            text: `Batch job is ${result.job.state}.`,
            ...(imagePaths.length ? { imagePaths } : {}),
            structuredContent: result,
          };
        }),
    ),
    tool(
      'batch_cancel',
      'Cancel one queued or running color-batch job and its current isolated renderer.',
      { job_id: z.string().regex(/^batch-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'batch_cancel', 'cancelling color batch', async () => {
          const result = cancelColorBatchAction(p, args);
          return { ok: true, text: `Batch job is ${result.job.state}.`, structuredContent: result };
        }),
    ),
    tool(
      'color_batch',
      'Render isolated color variants of one retained shape from the current scene. The base ' +
        'scene is frozen at expected_revision, only the selected unique material color changes, ' +
        'and each output is rendered in a fresh native Flame instance.',
      {
        object_id: z.string().min(1),
        colors: z.array(z.string().regex(/^#[0-9A-Fa-f]{6}$/)).length(4),
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920),
        height: z.number().int().min(180).max(1080),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean(),
      },
      async (args) =>
        runTool(p, ctx, 'color_batch', 'rendering isolated color variants', async () => {
          const result = await runColorBatchAction(p, ctx, args);
          const imagePaths = result.outputs.map((entry) => path.resolve(p.dir, entry.image));
          return {
            ok: true,
            text: `Rendered ${result.outputs.length} isolated color variants from revision ${result.base_revision}.`,
            imagePaths,
            structuredContent: result,
          };
        }),
    ),
    tool(
      'render_start',
      'Start a bounded background render from the exact requested scene revision. ' +
        'Returns immediately with a job ID for render_status or render_cancel.',
      {
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920),
        height: z.number().int().min(180).max(1080),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean(),
      },
      async (args) =>
        runTool(p, ctx, 'render_start', 'starting frozen image render', async () => ({
          ok: true,
          text: 'Started revision-frozen image render.',
          structuredContent: await startStillAction(p, ctx, args),
        })),
    ),
    tool(
      'render_status',
      'Read one render job without changing the scene. A completed job returns its actual PNG.',
      { job_id: z.string().regex(/^render-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'render_status', 'reading image render status', async () => {
          const result = statusStillAction(p, args);
          const image = result.job.state === 'succeeded' ? String(result.job.result?.image ?? '') : '';
          return {
            ok: true,
            text: `Render job is ${result.job.state}.`,
            ...(image ? { imagePaths: [path.join(p.dir, image)] } : {}),
            structuredContent: result,
          };
        }),
    ),
    tool(
      'render_cancel',
      'Cancel one queued or running render job. Cancellation stops only its isolated native process.',
      { job_id: z.string().regex(/^render-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'render_cancel', 'cancelling image render', async () => {
          const result = cancelStillAction(p, args);
          return { ok: true, text: `Render job is ${result.job.state}.`, structuredContent: result };
        }),
    ),
    tool(
      'render_image',
      'Render the current revision-frozen scene through native Flame. Returns the actual PNG, ' +
        'verified metadata, and the backend and denoiser that executed.',
      {
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920),
        height: z.number().int().min(180).max(1080),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean(),
      },
      async (args) =>
        runTool(p, ctx, 'render_image', 'rendering frozen image', async () => {
          const result = await runStillAction(p, ctx, args);
          return {
            ok: true,
            text: `Rendered ${result.width}x${result.height} at ${result.samples} spp using ${result.backend}/${result.denoiser}.`,
            imagePaths: [path.join(p.dir, result.image)],
            structuredContent: result,
          };
        }),
    ),
    tool(
      'video_start',
      'Start a bounded background H.264 export from the exact requested scene revision and return a job ID.',
      {
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920).refine((value) => value % 2 === 0),
        height: z.number().int().min(180).max(1080).refine((value) => value % 2 === 0),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean().default(true),
        fps: z.number().int().min(1).max(60),
        frames: z.number().int().min(1).max(240),
      },
      async (args) =>
        runTool(p, ctx, 'video_start', 'starting frozen video', async () => ({
          ok: true,
          text: 'Started revision-frozen video export.',
          structuredContent: await startVideoAction(p, ctx, args),
        })),
    ),
    tool(
      'video_status',
      'Read one video-export job without changing the scene.',
      { job_id: z.string().regex(/^video-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'video_status', 'reading video status', async () => {
          const result = statusVideoAction(p, args);
          return { ok: true, text: `Video job is ${result.job.state}.`, structuredContent: result };
        }),
    ),
    tool(
      'video_cancel',
      'Cancel one queued or running video export and only its isolated native process.',
      { job_id: z.string().regex(/^video-[0-9a-f-]{36}$/i) },
      async (args) =>
        runTool(p, ctx, 'video_cancel', 'cancelling video', async () => {
          const result = cancelVideoAction(p, args);
          return { ok: true, text: `Video job is ${result.job.state}.`, structuredContent: result };
        }),
    ),
    tool(
      'video_export',
      'Export a revision-frozen animated render-llm document through the retained native ' +
        'Flame movie path. Returns verified MP4 metadata without mutating the open scene.',
      {
        expected_revision: z.number().int().nonnegative(),
        width: z.number().int().min(320).max(1920).refine((value) => value % 2 === 0),
        height: z.number().int().min(180).max(1080).refine((value) => value % 2 === 0),
        samples: z.number().int().min(1).max(256),
        denoise: z.boolean().default(true),
        fps: z.number().int().min(1).max(60),
        frames: z.number().int().min(1).max(240),
      },
      async (args) =>
        runTool(p, ctx, 'video_export', 'exporting frozen video', async () => {
          const result = await runVideoAction(p, ctx, args);
          return {
            ok: true,
            text: `Exported ${result.frames} frames at ${result.fps} fps (${result.duration_s} s) using ${result.backend}/${result.denoiser}.`,
            structuredContent: result,
          };
        }),
    ),
  ];
}

export function buildRenderLlmMcp(p: Protocol, ctx: ActionTurnCtx) {
  return createSdkMcpServer({
    name: 'render-llm',
    version: '0.1.0',
    instructions: 'Validated, revision-aware control of the open render-llm scene.',
    tools: buildRenderLlmTools(p, ctx),
  });
}

export function buildMcp(p: Protocol, ctx: TurnCtx) {
  return createSdkMcpServer({
    name: 'mi3',
    version: '1.0.0',
    instructions: 'Live control of the render-llm editor the user has open.',
    tools: [
      ...buildRenderLlmTools(p, ctx),
      tool(
        'mi3_scene_summary',
        'Get a compact text digest of the whole live scene right now: project name, timeline ' +
          'marker/length/tempo, render settings, every root object (with its first-level ' +
          'children), every camera, every light, and background settings. Call this first, at ' +
          'the start of every task, before touching anything -- never guess what is in the scene.',
        {
          max_objects: z.number().int().min(10).max(500).default(200).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_scene_summary', 'leyendo la escena', async () => {
            const res = await p.runOps(
              [{ id: 's1', op: 'assistant.scene_summary', args: { max_objects: args.max_objects ?? 200 } }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_object_get',
        'Get full detail on a single object: identity, type, parent, children, every POS/ROT/SCA ' +
          'channel (and which of those this object type actually has), world position, ' +
          'visibility, its keyframe list, and type-specific properties for cameras/lights. Call ' +
          'this before changing anything about an object -- never guess its current transform.',
        { ref: z.string() },
        async (args) =>
          runTool(p, ctx, 'mi3_object_get', `leyendo ${args.ref}`, async () => {
            const res = await p.runOps(
              [{ id: 'o1', op: 'assistant.object_get', args: { ref: args.ref } }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_set_values',
        'Change one or more value channels on an object (e.g. POS_X, ROT_Z, CAM_FOV, ' +
          'LIGHT_STRENGTH). Goes through the same code path as a user dragging a slider: if the ' +
          'timeline marker sits on a keyframe it is edited in place, otherwise a new keyframe is ' +
          'created there. This is a real, undoable editor action -- the user can always Ctrl+Z ' +
          'it. The object is selected first so the user can see what is being touched.',
        {
          ref: z.string(),
          values: z.record(z.string(), z.union([z.number(), z.string()])),
          at_marker: z.boolean().default(true).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_set_values', `aplicando valores a ${args.ref}`, async () => {
            const atMarker = args.at_marker ?? true;
            const ops: Op[] = [
              { id: 'sv1', op: 'assistant.select', args: { ref: args.ref } },
              {
                id: 'sv2',
                op: 'assistant.values_set',
                args: { ref: args.ref, values: args.values, at_marker: atMarker },
              },
            ];
            const res = await p.runOps(ops, ctx.turnId, 15000, ctx.signal);
            return summarize(res);
          }),
      ),

      tool(
        'mi3_keyframe_add',
        'Set values on an object at an explicit frame number, moving the timeline marker there ' +
          'first. This is mi3_set_values spelled out for when the keyframe you want is not at ' +
          'the current marker position.',
        {
          ref: z.string(),
          frame: z.number(),
          values: z.record(z.string(), z.number()),
        },
        async (args) =>
          runTool(
            p,
            ctx,
            'mi3_keyframe_add',
            `creando keyframe en ${args.ref} (frame ${args.frame})`,
            async () => {
              const ops: Op[] = [
                { id: 'k1', op: 'assistant.marker', args: { frame: args.frame } },
                { id: 'k2', op: 'assistant.select', args: { ref: args.ref } },
                {
                  id: 'k3',
                  op: 'assistant.values_set',
                  args: { ref: args.ref, values: args.values, at_marker: true },
                },
              ];
              const res = await p.runOps(ops, ctx.turnId, 15000, ctx.signal);
              return summarize(res);
            },
          ),
      ),

      tool(
        'mi3_camera_preview',
        'Render a cheap live preview PNG from a camera. The image itself comes back attached to ' +
          'the result, so you see the shot immediately; its absolute path under ' +
          'Data/assistant/preview/ is also returned in case you want to Read it again. Describe ' +
          'only what is actually in that image. This never touches the final render pipeline and ' +
          'does not change the scene (camera visibility and light management are saved and ' +
          'restored around the render).',
        {
          ref: z.string().optional(),
          width: z.number().int().default(480).optional(),
          height: z.number().int().default(270).optional(),
          samples: z.number().int().default(1).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_camera_preview', 'generando preview de camara', async () => {
            const n = ++ctx.previewN;
            const safeTurn = ctx.turnId.replace(/[^A-Za-z0-9_-]/g, '_');
            const requestedOut = `preview/${safeTurn}_${n}.png`;
            const width = args.width ?? 480;
            const height = args.height ?? 270;
            const res = await p.runOps(
              [
                {
                  id: 'cp1',
                  op: 'assistant.camera_preview',
                  args: {
                    ref: args.ref,
                    width,
                    height,
                    samples: args.samples ?? 1,
                    out: requestedOut,
                  },
                },
              ],
              ctx.turnId,
              30000,
              ctx.signal,
            );
            const { ok, text } = summarize(res);
            if (!ok) return { ok, text };

            const r0 = res.results?.[0] as OpResultEntry | undefined;
            const relOut = typeof r0?.out === 'string' ? r0.out : requestedOut;
            const w = typeof r0?.w === 'number' ? r0.w : width;
            const h = typeof r0?.h === 'number' ? r0.h : height;
            const absPath = path.join(p.dir, relOut);

            p.emit({ type: 'preview', turn_id: ctx.turnId, path: relOut, w, h });

            // VERIFICACION C: el preview viaja ADJUNTO igual que el render de
            // calidad. Medido en el examen de poses con claude-haiku-4-5 (el
            // modelo por defecto de launch.cmd): en DOS turnos seguidos el
            // modelo pidio un camera_preview, escribio "ahora miro el preview"
            // y describio la imagen (encuadre, pose, legibilidad del brazo)
            // SIN una sola linea "permiso: Read ..." en assistant.log, o sea
            // sin abrirla nunca. La ronda B ya habia medido exactamente esto
            // para render_quality y lo resolvio adjuntando el PNG
            // (readPngForModel); el preview -- que es el ojo que el rol le
            // manda usar todo el rato -- se quedo en el camino de "por favor
            // llama a Read", que esta medido que no funciona. Pedir es una
            // sugerencia; adjuntar es un hecho.
            return {
              ok,
              imagePath: absPath,
              text:
                `Preview saved to: ${absPath}\n` +
                'The preview image itself is attached below this text: LOOK at it. If it did not ' +
                'come through, use the Read tool on the exact path above. Describe ONLY what is ' +
                'visible in it; if you cannot see an image attached, say so plainly instead of ' +
                'guessing.\n\n' +
                text,
            };
          }),
      ),

      tool(
        'mi3_render_quality',
        'Render the scene properly (real samples, denoising, the same engine the project uses) ' +
          'and return the absolute path of the PNG. This is slow -- tens of seconds to minutes -- ' +
          'and runs in a separate copy of the editor, so it never touches or freezes what the user ' +
          'is working on. Use mi3_camera_preview while you explore, and this ONLY to confirm a shot ' +
          'you already believe is right, or when the user asks about lighting, noise, materials or ' +
          'final look -- things a cheap preview cannot answer honestly. After it returns you MUST ' +
          'Read the returned path and actually look at the image before saying anything about it. ' +
          'Limit: 3 quality renders per turn.',
        {
          width: z.number().int().default(960).optional(),
          height: z.number().int().default(540).optional(),
          spp: z.number().int().default(64).optional(),
          denoise: z.boolean().default(true).optional(),
          camera: z.string().optional(),
          user_asked_for_more: z.boolean().default(false).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_render_quality', 'renderizando en calidad', async () => {
            // --- politica de iteracion (tope en el CODIGO, no solo en el rol) ---
            if (ctx.qualityN >= LIMITS.hardPerTurn) {
              return {
                ok: false,
                text:
                  `Ya has hecho ${ctx.qualityN} renders de calidad en este turno; ese es el tope ` +
                  'absoluto. Resume lo que ves y deja que el usuario decida el siguiente paso.',
              };
            }
            if (ctx.qualityN >= LIMITS.softPerTurn && !args.user_asked_for_more) {
              return {
                ok: false,
                text:
                  `Ya has hecho ${ctx.qualityN} renders de calidad en este turno. Solo puedes hacer ` +
                  'otro si el usuario lo pidio explicitamente en este turno; en ese caso vuelve a ' +
                  'llamar con user_asked_for_more: true. Si no, usa mi3_camera_preview o termina el turno.',
              };
            }

            // --- saneo de parametros ---
            const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v)));
            const width = clamp(args.width ?? 960, LIMITS.minW, LIMITS.maxW);
            const height = clamp(args.height ?? 540, LIMITS.minH, LIMITS.maxH);
            const spp = clamp(args.spp ?? 64, LIMITS.minSpp, LIMITS.maxSpp);
            const denoise = args.denoise ?? true;
            const budget = width * height * spp;
            if (budget > LIMITS.maxBudget) {
              return {
                ok: false,
                text:
                  `Demasiado trabajo: ${width}x${height} a ${spp} spp. Baja resolucion o muestras ` +
                  '(por ejemplo 1280x720 a 128 spp, o 960x540 a 128 spp) y vuelve a llamar.',
              };
            }

            const n = ++ctx.qualityN;
            const safeTurn = ctx.turnId.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 40);
            const jobId = `${safeTurn}_q${n}`;

            p.emit({ type: 'status', state: 'acting', turn_id: ctx.turnId });
            const onPhase = (phase: 'snapshot' | 'instance' | 'copy' | 'render' | 'done' | 'failed', detail?: string, ms?: number) =>
              p.emit({ type: 'job', turn_id: ctx.turnId, job_id: jobId, phase, detail, ms });

            // --- 1. snapshot del estado VIVO (lo hace el editor) ---
            onPhase('snapshot');
            const res = await p.runOps(
              [{ id: 'rs1', op: 'assistant.render_snapshot', args: { job_id: jobId, camera: args.camera } }],
              ctx.turnId,
              30000,
              ctx.signal,
            );
            const { ok, text } = summarize(res);
            if (!ok) {
              onPhase('failed', 'snapshot');
              const r0 = res.results?.[0] as OpResultEntry | undefined;
              if (String(r0?.error ?? '').includes('nofolder')) {
                return {
                  ok: false,
                  text:
                    'No puedo renderizar en calidad porque el proyecto nunca se ha guardado. ' +
                    'Pidele al usuario que pulse Ctrl+S y vuelve a intentarlo.\n\n' + text,
                };
              }
              return { ok: false, text };
            }

            const r0 = res.results?.[0] as OpResultEntry as unknown as RenderManifest;
            const man: RenderManifest = {
              job_id: String(r0.job_id),
              job_dir: String(r0.job_dir),
              scene: String(r0.scene),
              project_folder: String(r0.project_folder),
              project_file: String(r0.project_file),
              project_name: String(r0.project_name),
              install_dir: String(r0.install_dir),
              marker: Number(r0.marker),
              engine: Number(r0.engine),
              camera: String(r0.camera ?? ''),
            };

            // --- 2..4. copia + render + validacion (fuera del editor) ---
            try {
              const out = await runRenderJob(p, man, { width, height, spp, denoise, camera: args.camera }, onPhase, ctx.signal);
              p.emit({ type: 'preview', turn_id: ctx.turnId, path: out.outRel, w: out.w, h: out.h, kind: 'quality' });
              p.emit({ type: 'status', state: 'thinking', turn_id: ctx.turnId });
              return {
                ok: true,
                imagePath: out.outAbs,
                text:
                  `Quality render saved to: ${out.outAbs}\n` +
                  `${out.w}x${out.h}, ${out.spp} spp, engine ${out.engine}, ${Math.round(out.ms / 1000)} s.\n` +
                  'The rendered image itself is attached below this text: LOOK at it and describe only what ' +
                  'you can actually see in it. If it did not come through, use the Read tool on the exact ' +
                  'path above before saying anything about the image.\n' +
                  `Quality renders used this turn: ${n}/${LIMITS.softPerTurn}.\n\n` +
                  // RONDA B (verificacion, gate B5): el modelo (haiku, medido en dos
                  // turnos seguidos) NO llamaba a Read tras un render OK -- cero lineas
                  // "permiso: Read ..." en assistant.log -- y aun asi describia la luz.
                  // Ni pedirlo ("Use the Read tool...") ni exigirlo en imperativo
                  // ("Your very next tool call MUST be Read") lo cambiaron: eran
                  // sugerencias. Por eso la imagen viaja ADJUNTA en el resultado (ver
                  // readPngForModel) y este cierre solo prohibe describir a ciegas.
                  'Describe ONLY what is visible in the attached image. If you cannot see an image ' +
                  'attached to this result, say so plainly instead of guessing.',
              };
            } catch (e) {
              p.emit({ type: 'status', state: 'thinking', turn_id: ctx.turnId });
              if (e instanceof RenderAborted) return { ok: false, text: 'Render cancelado por el usuario.' };
              if (e instanceof RenderError) return { ok: false, text: `El render fallo: ${e.message}` };
              throw e;
            }
          }),
      ),

      tool(
        'mi3_checkpoint',
        'Create a NAMED checkpoint the user can come back to, or list existing ones. Every turn ' +
          'already gets an automatic checkpoint (pool of 5, rotating); a named one is permanent ' +
          'until the user deletes it, so use it to mark milestones -- before a big change, and ' +
          'after a state worth keeping. Name it after what the scene IS at that moment ("before ' +
          'relighting", "group blocked out"), never after what you are about to do. action:"list" ' +
          'first when the user wants to go back, so you can refer to a checkpoint by its name.',
        {
          action: z.enum(['create', 'list']),
          name: z.string().optional(),
          note: z.string().optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_checkpoint', `checkpoint: ${args.action}`, async () => {
            const res = await p.runOps(
              [
                {
                  id: 'c1',
                  op: 'assistant.checkpoint',
                  args: { action: args.action, name: args.name, note: args.note },
                },
              ],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_undo',
        'Undo the last N editor actions (like pressing Ctrl+Z N times). Use this if a change did ' +
          'not turn out the way you intended, instead of trying to hand-compute the inverse.',
        {
          count: z.number().int().min(1).max(20).default(1).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_undo', `deshaciendo ${args.count ?? 1} paso(s)`, async () => {
            const n = args.count ?? 1;
            const ops: Op[] = Array.from({ length: n }, (_, i) => ({ id: `u${i + 1}`, op: 'edit.undo' }));
            const res = await p.runOps(ops, ctx.turnId, 15000, ctx.signal);
            return summarize(res);
          }),
      ),

      tool(
        'mi3_wait_idle',
        'Wait until the editor is not busy (no modal save/render in progress) and has no popup ' +
          'open. Useful right after a checkpoint, or before a preview if the editor might still ' +
          'be settling.',
        {
          timeout_ms: z.number().int().default(5000).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_wait_idle', 'esperando a que el editor este libre', async () => {
            const timeoutMs = args.timeout_ms ?? 5000;
            const deadline = Date.now() + timeoutMs;
            let last: OpResultEntry | undefined;
            for (;;) {
              const res = await p.runOps([{ id: 'p1', op: 'assistant.ping' }], ctx.turnId, 15000, ctx.signal);
              last = res.results?.[0] as OpResultEntry | undefined;
              const busy = truthy(last?.busy);
              const popup = truthy(last?.popup);
              if (!busy && !popup) {
                return { ok: true, text: `Editor idle.\n${formatOpResult(last)}` };
              }
              if (Date.now() > deadline) {
                return { ok: false, text: `Editor still busy after ${timeoutMs}ms.\n${formatOpResult(last)}` };
              }
              if (ctx.signal?.aborted) {
                return { ok: false, text: 'Wait cancelled (turn stopped).' };
              }
              await sleep(250, ctx.signal);
            }
          }),
      ),

      tool(
        'mi3_screen_layout',
        'RONDA C: compute where objects actually land ON SCREEN for a given camera -- reason ' +
          'about composition (rule of thirds, headroom, whether a limb points at the lens, whether ' +
          'two objects overlap) with real numbers instead of guessing from a flat preview image. ' +
          'Projects each object\'s true world bounding box (itself + all its child parts) through ' +
          'the camera\'s actual lens (FOV or ortho, aspect ratio) using the same math the renderer ' +
          'uses. Call this BEFORE mi3_camera_preview when you are choosing or checking a shot -- it ' +
          'is far cheaper than a render and gives you exact percentages instead of pixels you have ' +
          'to eyeball. "camera" defaults to the scene\'s active camera. "refs" is an optional ' +
          'comma-separated list of object refs (omit for every root object in the scene, same set ' +
          'mi3_scene_summary lists). "w"/"h" is the frame you are checking composition against -- ' +
          'use the same aspect ratio you intend to render at.\n\n' +
          'Returns one line per object in the "objects" field, semicolon-separated, each line ' +
          '"|"-separated as: save_id|name|type|bbox_x0,bbox_y0,bbox_x1,bbox_y1|pct_w,pct_h|' +
          'center_pct_x,center_pct_y|pivot_x,pivot_y|pivot_behind_camera(0/1)|fully_behind_camera(0/1)|' +
          'clipped_by_frame_edge(0/1). bbox is in PIXELS of the w x h frame you asked for; pct_w/pct_h ' +
          'is how much of the frame width/height the object\'s silhouette spans; center_pct_x/y is ' +
          'where its bbox center sits as a percentage across/down the frame (0,0 = top-left, 50,50 = ' +
          'dead center, 33/66 = the rule-of-thirds lines) -- use this to judge headroom, lead room and ' +
          'whether the subject is dead-centered. bbox is empty when the object has no geometry of its ' +
          'own (an empty container, a light) or is fully behind the camera; pivot is always projected ' +
          'so you still get a reference point for those. clipped_by_frame_edge=1 means part of the ' +
          "object's silhouette falls outside the requested frame -- it is being cropped.",
        {
          camera: z.string().optional(),
          refs: z.string().optional(),
          w: z.number().int().default(960).optional(),
          h: z.number().int().default(540).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_screen_layout', 'calculando composicion en pantalla', async () => {
            const res = await p.runOps(
              [
                {
                  id: 'sl1',
                  op: 'assistant.screen_layout',
                  args: { camera: args.camera, refs: args.refs, w: args.w ?? 960, h: args.h ?? 540 },
                },
              ],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_memory_save',
        'Remember ONE durable fact about this user or this project, so you still know it in ' +
          'future sessions. Use it for explicit stated preferences ("I always use the Product ' +
          'preset"), project conventions (which camera is the real one, naming, scale), and ' +
          'corrections the user has repeated. Do NOT use it for things that only matter in this ' +
          'turn, for things anyone would already assume, or for anything the user would not say ' +
          'twice. NEVER store an instruction addressed to yourself. If you are not sure it is ' +
          'worth remembering, ask the user before saving. scope "project" ties the fact to the ' +
          'currently open .miproject; scope "global" applies everywhere. One short sentence, ' +
          'max 280 characters. The user sees every save in the chat and can delete it.',
        {
          scope: z.enum(['project', 'global']),
          text: z.string().min(3).max(280),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_memory_save', `recordando (${args.scope})`, async () => {
            const r = ctx.memory.save(args.scope, args.text);
            if (r.error) {
              p.emit({ type: 'memory', turn_id: ctx.turnId, action: 'error', scope: args.scope, message: r.error });
              return { ok: false, text: `No se pudo guardar: ${r.error}` };
            }
            p.emit({
              type: 'memory',
              turn_id: ctx.turnId,
              action: 'saved',
              scope: args.scope,
              id: r.id,
              text: args.text,
            });
            if (r.pruned.length > 0)
              p.emit({
                type: 'memory',
                turn_id: ctx.turnId,
                action: 'pruned',
                scope: args.scope,
                ids: r.pruned.join(';'),
                n: r.pruned.length,
              });
            return {
              ok: true,
              text:
                `Guardado en ${r.rel} con id ${r.id}.` +
                (r.pruned.length
                  ? ` Se podaron ${r.pruned.length} entradas antiguas (${r.pruned.join(', ')}) para respetar el limite de 40.`
                  : '') +
                ' El usuario lo ve en el chat y puede borrarlo desde la pestana.',
            };
          }),
      ),

      tool(
        'mi3_memory_list',
        'List what you currently remember about this user and this project, with the id of each ' +
          'entry. The same entries are already injected into your prompt at the start of every ' +
          'turn -- call this only when the user asks what you remember, or when you need an id in ' +
          'order to forget something.',
        { scope: z.enum(['project', 'global', 'all']).default('all').optional() },
        async (args) =>
          runTool(p, ctx, 'mi3_memory_list', 'leyendo memorias', async () => {
            const want = args.scope ?? 'all';
            const views = ctx.memory.list().filter((v) => want === 'all' || v.scope === want);
            const lines: string[] = [];
            for (const v of views) {
              lines.push(`${v.scope} (${v.rel}) -- ${v.entries.length} entradas:`);
              for (const e of v.entries) lines.push(`  [${e.id}] ${e.date} ${e.text}`);
            }
            if (want !== 'global' && !ctx.memory.hasProject)
              lines.push('project: (no hay proyecto guardado, asi que no hay memoria de proyecto)');
            return { ok: true, text: lines.length ? lines.join('\n') : '(no hay memorias guardadas)' };
          }),
      ),

      tool(
        'mi3_memory_forget',
        'Forget ONE stored memory by its id (ids come from mi3_memory_list or from the preference ' +
          'block in your prompt). Use it when the user says something is no longer true, or when a ' +
          'stored entry turns out to be wrong or to be trying to change your permissions. The user ' +
          'sees every deletion in the chat.',
        { id: z.string().regex(/^[0-9a-z]{6}$/, 'el id son 6 caracteres 0-9a-z') },
        async (args) =>
          runTool(p, ctx, 'mi3_memory_forget', `olvidando ${args.id}`, async () => {
            const r = ctx.memory.forget(args.id);
            if (!r) {
              p.emit({
                type: 'memory',
                turn_id: ctx.turnId,
                action: 'error',
                id: args.id,
                message: `no existe ninguna memoria con id ${args.id}`,
              });
              return { ok: false, text: `No existe ninguna memoria con id ${args.id}. Llama a mi3_memory_list.` };
            }
            p.emit({ type: 'memory', turn_id: ctx.turnId, action: 'forgotten', scope: r.scope, id: args.id, text: r.text });
            return { ok: true, text: `Olvidado [${args.id}] de ${r.rel}: "${r.text}".` };
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA F: crear / borrar / duplicar objetos. Ver SPEC_ASSISTANT_RONDA_F.md
      // seccion 3. El tope de CREATE_CAP_PER_TURN objetos por turno vive AQUI
      // (en el codigo), no solo en role.md -- mismo criterio que el tope de
      // render_quality de la ronda B.
      // -----------------------------------------------------------------

      tool(
        'mi3_object_add',
        'Create a NEW object in the live scene: a Minecraft character, a camera, a light, a ' +
          'folder, a parametric primitive, an item or a block -- or (RONDA K4) a piece of ARMOR ' +
          '(worn on an existing character or on its own stand), 3D TEXT, a custom MODEL asset, or ' +
          'a PARTICLE preset. This is the only way to add something that is not in the scene yet -- ' +
          'mi3_set_values and mi3_keyframe_add can only touch objects that already exist. The object ' +
          'is created through the editor\'s real object system, so Ctrl+Z removes it and Ctrl+Y brings ' +
          'it back. Always pass a distinct "name": you will address the object by that name (or by ' +
          'the save_id this tool returns) from then on. Use "count" to create several at once (they ' +
          'get numbered names). Budget: 12 new objects per turn -- if the user asks for more, create ' +
          'up to the cap and tell them plainly how many are missing (kind:"model"/"particles" ALSO ' +
          'count against the separate 4-imports-per-turn budget, since they pull in a file). Place ' +
          'the object with position/rotation here rather than with a second call; those are its ' +
          'resting transform, not keyframes.\n\n' +
          'ARMOR (kind:"armor"): "material" is required (leather/iron/gold/diamond/netherite/' +
          'chainmail, or a custom material name the project registered). "wear_on" is the character ' +
          'to dress -- pass its ref and the armor is attached to that character\'s ROOT (never to ' +
          'one of its body parts) with the engine\'s "inherit pose" on, so from then on EVERY armor ' +
          'piece tracks the SAME-NAME bone of the wearer live, every frame, bend and IK included: ' +
          'animate the character afterwards and the armor walks with it. Nothing to re-sync, ever. ' +
          'Pass "stand":true instead of "wear_on" for ' +
          'a free-standing armor display (self-contained, ignores wear_on). ' +
          '"trim" adds Minecraft\'s trim overlay: {pattern (required, e.g. silence/spire/wild), material ' +
          '(the trim metal, default the pack\'s first), effect (one of the 20 animated ShinyTrims effects ' +
          'or none), intensity}. See the trim effects table in the rig cheat sheet.\n\n' +
          'TRIMMED TOOLS (kind:"item"): the same "trim" field works on an item, and gives you a trimmed ' +
          'sword / axe / pickaxe / shovel / hoe / mace / spear / bow / crossbow (ShinyTrims O7). Only ' +
          'those shapes have a mask -- ask for a trimmed apple and the call tells you it cannot. To ' +
          'change or remove a trim on an item already in the scene use mi3_object_edit op:"item_trim" ' +
          '(pattern:"none" removes it).\n\n' +
          '3D TEXT (kind:"text"): "text" is required. "align" (left/center/right, default center) and ' +
          '"outline" (bool) are optional. Uses the built-in Minecraft font.\n\n' +
          'MODEL (kind:"model") and PARTICLES (kind:"particles") bring in a file: pass "asset" (model) ' +
          'or "preset" (particles) as either the name of an image/model the user attached to THIS ' +
          'turn\'s message, or a bare filename from mi3_import op:"library" ' +
          '(kind:"project_assets"/"particles"). Never invent a filename -- list first if unsure.',
        {
          kind: z.enum([
            'character', 'camera', 'light', 'folder', 'primitive', 'item', 'block',
            'armor', 'text', 'model', 'particles',
          ]),
          name: z.string().min(1).max(60),
          count: z.number().int().min(1).max(12).default(1).optional(),
          // kind: 'character'
          body: z.enum(['slim', 'classic']).default('slim').optional(),
          variant: z
            .enum(['steve', 'alex', 'zuri', 'sunny', 'noor', 'makena', 'kai', 'efe', 'ari'])
            .default('steve')
            .optional(),
          // kind: 'light'
          light: z.enum(['point', 'spot']).default('point').optional(),
          // kind: 'primitive'
          shape: z.enum(['CUBE', 'CYLINDER', 'CONE', 'SPHERE', 'SURFACE']).default('CUBE').optional(),
          // kind: 'item'
          item: z.string().optional(),
          // kind: 'block'
          block: z.string().optional(),
          block_state: z.record(z.string(), z.string()).optional(),
          // kind: 'armor' (RONDA K4)
          material: z.string().optional(),
          wear_on: z.string().optional(),
          stand: z.boolean().default(false).optional(),
          // kind: 'armor' -- trim + efecto (ShinyTrims O6)
          trim: z
            .object({
              pattern: z.string(),
              material: z.string().optional(),
              effect: zTrimEffect.optional(),
              intensity: z.number().min(0).max(4).optional(),
            })
            .optional(),
          // kind: 'text' (RONDA K4)
          text: z.string().min(1).max(200).optional(),
          align: z.enum(['left', 'center', 'right']).default('center').optional(),
          outline: z.boolean().default(false).optional(),
          // kind: 'model' (RONDA K4) -- nombre de adjunto o de la libreria del proyecto
          asset: z.string().optional(),
          // kind: 'particles' (RONDA K4) -- nombre de adjunto o preset de <instalacion>/Particles (RONDA L, H3a)
          preset: z.string().optional(),
          // comunes
          position: z.array(z.number()).length(3).optional(),
          rotation: z.array(z.number()).length(3).optional(),
          parent: z.string().optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_object_add', `creando ${args.count ?? 1} x ${args.kind}`, async () => {
            const want = args.count ?? 1;
            const left = Math.max(0, CREATE_CAP_PER_TURN - ctx.createdN);
            if (left === 0) {
              return {
                ok: false,
                text:
                  `Ya has creado ${ctx.createdN} objetos en este turno; ese es el tope ` +
                  `(${CREATE_CAP_PER_TURN}). No crees mas: dile al usuario cuantos faltan y que te lo ` +
                  'pida en otro turno.',
              };
            }
            const n = Math.min(want, left);

            // --- RONDA K4: validaciones tempranas y resolucion de ruta ANTES de
            // tocar el editor -- un fallo aqui no cuenta contra ningun tope. ---
            if (args.kind === 'armor' && !args.material) {
              return { ok: false, text: 'Falta "material" (leather/iron/gold/diamond/netherite/chainmail, o un material custom registrado).' };
            }
            if (args.kind === 'text' && !args.text) {
              return { ok: false, text: 'Falta "text" (el contenido del texto 3D).' };
            }
            let resolvedAssetPath: string | undefined;
            if (args.kind === 'model' || args.kind === 'particles') {
              const nameArg = args.kind === 'model' ? args.asset : args.preset;
              if (!nameArg) {
                return { ok: false, text: `Falta "${args.kind === 'model' ? 'asset' : 'preset'}" (nombre de un adjunto de este turno o de la libreria).` };
              }
              const importsLeft = Math.max(0, IMPORTS_CAP_PER_TURN - ctx.importsN);
              if (importsLeft === 0) {
                return {
                  ok: false,
                  text:
                    `Ya has importado ${ctx.importsN} archivos en este turno; ese es el tope (${IMPORTS_CAP_PER_TURN}). ` +
                    'Pidele al usuario que te lo pida en otro turno.',
                };
              }
              const resolution = await resolveAssetPath(p, ctx, args.kind, nameArg);
              if (!resolution.ok) return { ok: false, text: resolution.text };
              resolvedAssetPath = resolution.path;
              ctx.importsN++;
            }

            const ops: Op[] = [];
            for (let i = 0; i < n; i++) {
              const opArgs: Record<string, unknown> = {
                kind: args.kind,
                name: n === 1 ? args.name : `${args.name} ${i + 1}`,
              };
              if (args.kind === 'character' && args.body !== undefined) opArgs.type = args.body;
              if (args.variant !== undefined) opArgs.variant = args.variant;
              if (args.light !== undefined) opArgs.light = args.light;
              if (args.shape !== undefined) opArgs.shape = args.shape;
              if (args.item !== undefined) opArgs.item = args.item;
              if (args.block !== undefined) opArgs.block = args.block;
              if (args.block_state !== undefined) opArgs.state = args.block_state;
              if (args.kind === 'armor') {
                if (args.material !== undefined) opArgs.material = args.material;
                if (args.stand) opArgs.stand = 1;
                else if (args.wear_on !== undefined) opArgs.wear_on = args.wear_on;
                if (args.trim !== undefined) opArgs.trim = args.trim;
              }
              // ShinyTrims O7 (tool trims): el MISMO sub-mapa "trim" vale para
              // kind:"item". No es un campo nuevo del schema a proposito -- el
              // vocabulario del trim es uno solo (pattern/material/effect/
              // intensity) y duplicarlo con otro nombre solo daria dos formas
              // de escribir lo mismo.
              if (args.kind === 'item' && args.trim !== undefined) opArgs.trim = args.trim;
              if (args.kind === 'text') {
                opArgs.text = args.text;
                if (args.align !== undefined) opArgs.align = args.align;
                if (args.outline !== undefined) opArgs.outline = args.outline ? 1 : 0;
              }
              if ((args.kind === 'model' || args.kind === 'particles') && resolvedAssetPath !== undefined) {
                opArgs.path = resolvedAssetPath;
              }
              if (args.parent !== undefined) opArgs.parent = args.parent;
              if (args.position !== undefined) {
                opArgs.pos_x = args.position[0];
                opArgs.pos_y = args.position[1];
                opArgs.pos_z = args.position[2];
              }
              if (args.rotation !== undefined) {
                opArgs.rot_x = args.rotation[0];
                opArgs.rot_y = args.rotation[1];
                opArgs.rot_z = args.rotation[2];
              }
              ops.push({ id: `a${i + 1}`, op: 'assistant.object_add', args: opArgs });
            }

            const timeoutMs = (args.kind === 'model' || args.kind === 'particles' || args.kind === 'armor' ? 60000 : 20000) + 5000 * n;
            const res = await p.runOps(ops, ctx.turnId, timeoutMs, ctx.signal);
            const { ok, text } = summarize(res);
            const okCount = (res.results ?? []).filter((r) => !!(r as OpResultEntry).ok).length;
            ctx.createdN += okCount;

            let outText = text;
            if (n < want) {
              outText +=
                `\n\nTOPE: has pedido ${want} y solo se han creado ${n} (tope ${CREATE_CAP_PER_TURN} ` +
                `por turno, llevas ${ctx.createdN}). Dile al usuario EXACTAMENTE cuantos faltan y que ` +
                'te lo pida en un turno nuevo; no vuelvas a intentarlo en este.';
            }
            return { ok, text: outText };
          }),
      ),

      tool(
        'mi3_object_delete',
        'Delete an object (and everything under it) from the live scene. This goes through the ' +
          'editor\'s real delete action, so Ctrl+Z brings it back exactly as it was. Read the ' +
          'scene first and be sure of the ref -- deleting a rig root takes its parts with it.',
        { ref: z.string() },
        async (args) =>
          runTool(p, ctx, 'mi3_object_delete', `borrando ${args.ref}`, async () => {
            const res = await p.runOps(
              [{ id: 'd1', op: 'assistant.object_delete', args: { ref: args.ref } }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_object_duplicate',
        'Duplicate an existing object with all its parts, keyframes and settings, as a sibling of ' +
          'the original. Much cheaper and more faithful than building a second one by hand -- use ' +
          'it for crowds, repeated props, or a variant of something you already posed. Counts ' +
          'against the same 12-objects-per-turn budget as mi3_object_add.',
        {
          ref: z.string(),
          new_name: z.string().min(1).max(60).optional(),
          count: z.number().int().min(1).max(12).default(1).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_object_duplicate', `duplicando ${args.count ?? 1} x ${args.ref}`, async () => {
            const want = args.count ?? 1;
            const left = Math.max(0, CREATE_CAP_PER_TURN - ctx.createdN);
            if (left === 0) {
              return {
                ok: false,
                text:
                  `Ya has creado ${ctx.createdN} objetos en este turno; ese es el tope ` +
                  `(${CREATE_CAP_PER_TURN}). No dupliques mas: dile al usuario cuantos faltan y que te ` +
                  'lo pida en otro turno.',
              };
            }
            const n = Math.min(want, left);

            // Al duplicar en cadena, el ref es SIEMPRE el original (no la copia
            // anterior) -- cada op es independiente contra la misma fuente.
            const ops: Op[] = [];
            for (let i = 0; i < n; i++) {
              const name = args.new_name === undefined ? '' : n === 1 ? args.new_name : `${args.new_name} ${i + 1}`;
              ops.push({ id: `dp${i + 1}`, op: 'object.duplicate', args: { ref: args.ref, name, via: 'ui' } });
            }

            const timeoutMs = 20000 + 5000 * n;
            const res = await p.runOps(ops, ctx.turnId, timeoutMs, ctx.signal);
            const { ok, text } = summarize(res);
            const okCount = (res.results ?? []).filter((r) => !!(r as OpResultEntry).ok).length;
            ctx.createdN += okCount;

            let outText = text;
            if (n < want) {
              outText +=
                `\n\nTOPE: has pedido ${want} y solo se han duplicado ${n} (tope ${CREATE_CAP_PER_TURN} ` +
                `por turno, llevas ${ctx.createdN}). Dile al usuario EXACTAMENTE cuantos faltan y que ` +
                'te lo pida en un turno nuevo; no vuelvas a intentarlo en este.';
            }
            return { ok, text: outText };
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA J: el fondo del proyecto (cielo, suelo, nubes, niebla). Ver
      // SPEC_ASSISTANT_RONDA_J.md seccion 3-4 (cielo/suelo) + RONDA K3
      // seccion 4 (EL SOL). La exclusion del sol de la ronda J desaparece:
      // las hojas 13.5/13.6 pasan de tabla-del-sol-por-defecto a formula con
      // la elevacion y el azimut dentro.
      // -----------------------------------------------------------------

      tool(
        'mi3_background_set',
        'Change the project BACKGROUND / WORLD: THE SUN (azimuth, elevation, strength, colour), ' +
          'ambient light, sky colour, the ground plane, clouds, fog, the moon, the biome and wind. ' +
          'This is the "Background / World" panel, not an object in the scene -- that is why there ' +
          'is no ref for it and why mi3_set_values cannot touch it. It is a real, undoable editor ' +
          'action: everything you change in ONE call is a SINGLE Ctrl+Z for the user. Pass only the ' +
          'fields you actually want to change; anything you omit is left exactly as it is.\n\n' +
          'THE SUN, and what moving it costs you. Default sun: azimuth 180, elevation 60 (it sits ' +
          'on the -Y side and shines toward +Y). sun_azimuth is a compass in the SAME convention as ' +
          'camera ROT_Z: 0 = +Y, 90 = +X, 180 = -Y, 270 = -X, and it says where the sun IS. ' +
          'sun_elevation is degrees above the horizon (0 = on the horizon, 90 = overhead). ' +
          'Cast shadows point at sun_azimuth + 180 and are height * cot(elevation) long -- a ' +
          '32-unit character throws 18.5 units at elevation 60, 55.4 at 30, 119 at 15. The call ' +
          'returns the EFFECTIVE sun_azimuth / sun_elevation / shadow_len_32 / shadow_azimuth: ' +
          'quote those, never your own arithmetic. Which faces of a subject are lit is decided by ' +
          'delta = camera_azimuth - sun_azimuth (camera_azimuth = camera ROT_Z + 180): near 0 the ' +
          'camera sees the lit faces and the shadow hides behind the bodies; near 180 everything ' +
          'visible is backlit and the whole shadow lies in frame. See cheat sheet 13.5.\n\n' +
          'MEASURED recipe for a white studio backdrop (cheat sheet 14): sky_color "#FFFFFF", ' +
          'ground_block "block/white_concrete", clouds_show false. Do NOT use white_wool or ' +
          'quartz_block_top for the floor: both burn out to #FFFFFF and the floor disappears.\n\n' +
          'ANIMATION. These settings live on the project unless the timeline has a BACKGROUND ' +
          'track. ensure_track:true creates that track (it is born with the current values, so ' +
          'creating it does not change how the scene looks) and from then on every call writes a ' +
          'KEYFRAME at the playhead -- pass frame to place it. That is how you animate a sunset.\n\n' +
          'SOFT LIGHT, MEASURED (Cinema). sun_softness is the angular DIAMETER of the sun disc in ' +
          'degrees and it now reaches Cinema (it used to be ignored: 0.526 and 8 gave byte-identical ' +
          'renders). Penumbra width grows LINEARLY with it -- measured on a bar 46 units above the ' +
          'floor: 0.526 deg (the real sun) = 0.6 units of gradient, 2 deg = 2.2, 8 deg = 8.9. ' +
          'sun_softness 0 is a valid hard sun (knife-edge shadow).\n\n' +
          'RE-GOLDEN, ON PURPOSE. Because the channel used to be ignored, every render made before ' +
          'the fix drew the sun disc with one hardcoded softness. Old scenes therefore look ' +
          'different now: that is the fix working, not a regression. The old look is exactly ' +
          'reproducible -- sun_softness 4.0 renders BYTE-IDENTICAL to the pre-fix output (same ' +
          'sha256, measured across the sweep 0.526 / 1 / 2 / 4 / 8). There is no compatibility ' +
          'flag and there will not be one: whoever wants the old frames back sets 4.0.\n\n' +
          'SOFTBOX = EMISSIVE SURFACE + CAM_INVISIBLE (the official recipe for product/portrait ' +
          'light). There is no area-light object: you MAKE one. Add a primitive (SURFACE, or a CUBE ' +
          'flattened with SCA_Z 0.05), park it ABOVE the subject, and set two values on it with ' +
          'mi3_values_set: EMISSIVE 1 and CAM_INVISIBLE 1. It then lights the scene as a real area ' +
          'light (sampled by area with MIS) and never shows up in frame. Leave the FLOOR an ' +
          'ordinary, NON-emissive surface: that is what the contact shadow lands on.\n\n' +
          'HOW BRIGHT YOU CAN MAKE IT -- two ceilings, both measured, neither negotiable. ' +
          '(1) EMISSIVE IS CAPPED AT 1: the engine clamps it, so EMISSIVE 1, 2 and 4 render ' +
          'BYTE-IDENTICAL (same sha256, verified). Asking for 4 buys you nothing. ' +
          '(2) emissive_scale, the multiplier that turns EMISSIVE into radiance, is NOT one of your ' +
          'arguments and is GLOBAL: every render and export you make uses the engine default 5, and ' +
          'that one number scales EVERY emitter in the scene at once -- you cannot use it to make ' +
          'one plate brighter than another. So what you really control is EMISSIVE in 0..1, the ' +
          'plate SIZE and its DISTANCE. Size also buys softness (measured penumbra 3.6 / 6.7 / ' +
          '10.1 / 15.8 units for plates of 16 / 32 / 64 / 128 units), and a plate too big or too ' +
          'close clips the lit floor to 255, which collapses every shadow-by-ratio reading. Tint ' +
          'the plate with RGB_MUL -- MIX_COLOR does NOT tint emission in Cinema.\n\n' +
          'KEY + FILL WITH TWO PLATES. Because emissive_scale is global, the RATIO between two ' +
          'plates is EMISSIVE and nothing else: key plate at EMISSIVE 1, fill plate at EMISSIVE ' +
          '0.05-0.12 on the opposite side. Measured on the product hero (2048 px, key at 3/4 front ' +
          'left): no fill = clean dynamic range 3.61 with the shaded side crushed; fill 0.05 = ' +
          '3.53; fill 0.12 = 3.12 and the cast shadow on the plinth lifts from -106/255 to ' +
          '-92/255. Fill buys back the dark side and the ground shadow; it costs contrast. ' +
          'EMISSIVE is linear (0.5 renders exactly half of 1), so the numbers are dial-able.\n\n' +
          'SHADOW CATCHER -- the floor half of the same recipe, and the reason a product shot is ' +
          'ONE render now. Set SHADOW_CATCHER 1 on the floor (or the plinth, or a plane) with ' +
          'mi3_values_set: that surface stops painting itself and instead writes the shadow it ' +
          'receives into the ALPHA channel. Render with transparent 1 and you get subject + ' +
          'contact shadow on a clean transparent background, ready to drop on any web colour -- ' +
          'no three-pass division, no matte, no compositing. SHADOW_OPACITY 0..1 dims that shadow ' +
          'and is exactly linear (measured 1.0000 / 0.7499 / 0.4999 / 0.2497 for 1 / .75 / .5 / ' +
          '.25) and keyframeable like any other channel. It obeys the light you built: the ' +
          'penumbra tracks sun_softness and the softbox SIZE, and a light never casts a shadow of ' +
          'its own body onto the catcher. Two things to know: the catcher stays in the light ' +
          'transport (the subject keeps its bounce off the floor, which is a third of its ' +
          'brightness), and with transparent 0 you still see it -- the preview paints the ' +
          'background darkened by the shadow, which is what the export will look like.\n\n' +
          'ANTI-PATTERN -- NEVER make the floor or a cyclorama emissive. Emission is added where ' +
          'the ray HITS, with no shadow ray: an emissive surface shines at full brightness even ' +
          'under whatever stands on it, so an emissive floor has NO contact shadow at all. Light ' +
          'from ABOVE with a plate; catch the shadow on a plain floor below.\n\n' +
          'THE DOME AS A REAL LIGHT (cinema_env_light, via mi3_project_set). The project sky/fog/' +
          'ambient colours always described a dome, but the path tracer only ever reached it by ' +
          'BOUNCING into it: with cinema_bounces 1 a scene lit only by the dome renders BLACK ' +
          '(measured 0.0000), and by 4 bounces the contact shadow it casts has faded 92%. Set ' +
          'cinema_env_light > 0 and the dome becomes a sampled light like any other: it lights at ' +
          '1 bounce and its shadow survives. The number is a SAMPLE BUDGET share against the ' +
          'emissive plates and the point/spot lights, not a brightness: 1 gives the dome half the ' +
          'samples, 0.1 about a ninth. Noise: measured only 1.0-1.4x better on a uniform dome ' +
          '(cosine BSDF sampling is already near-perfect there) -- do not turn it on expecting a ' +
          'cleaner render. IT DOES CHANGE THE IMAGE at a fixed bounce count: ON with n bounces ' +
          'equals OFF with n+1, so the same scene comes out ~3-5% brighter (measured +4.8% at 2 ' +
          'bounces, +3.3% at 3), and with a SHADOW CATCHER the alpha changes a lot more -- the ' +
          'catcher now sees the subject blocking the dome and writes that ambient occlusion into ' +
          'the alpha (measured 6x wider shadow coverage). Great for realism, bad if you were ' +
          'matching a previous render. Default 0 = OFF, so an old project looks exactly as it ' +
          'did; turn it on deliberately, not by default.\n\n' +
          'A WHITE BACKGROUND WITHOUT A WHITE DOME (cinema_backdrop, via mi3_project_set). Do NOT ' +
          'paint the sky/fog/ambient white to get a white product background: a white dome BOUNCES ' +
          'LIGHT and flattens the subject -- measured on the hero, -46% of tonal range and -32% of ' +
          'internal contrast versus the same lighting on a dark dome. Use cinema_backdrop instead: ' +
          '1 = solid colour, 2 = linear on-screen gradient between cinema_backdrop_color and ' +
          'cinema_backdrop_color2 along cinema_backdrop_angle (degrees; 0 = left to right, 90 = top ' +
          'to bottom). The camera sees it; nothing else does. It is composited AFTER the tonemap, so ' +
          'the colour you pick is the byte that comes out (F7F6F6 renders as 247/246/246, whatever ' +
          'the exposure and tonemapper are) and the shadow-catcher shadow lands on it exactly as an ' +
          'external compositor would put it. It changes ZERO light: with the same project, backdrop ' +
          'on vs off with transparent=1 renders BIT-IDENTICAL (the flag only reaches the kernel when ' +
          'the film is opaque). Real studio sweeps are never flat -- the published hero backdrop ' +
          'falls 30 codes diagonally, which is mode 2 with angle 30. Default 0 = OFF.\n\n' +
          'There is no "sun_angle": the engine channel with that name is the angular DIAMETER of ' +
          'the sun disc (shadow softness), which is sun_softness here. Asking for sun_angle fails ' +
          'on purpose.',
        {
          // --- sol / ambiente (K3) ---
          sun_azimuth: z.number().optional(),
          sun_elevation: z.number().min(-90).max(90).optional(),
          sun_time: z.number().optional(),
          sun_rotation: z.number().optional(),
          sun_strength: z.number().min(0).max(4).optional(),
          sun_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB, e.g. "#FFE0B0"').optional(),
          sun_softness: z.number().min(0).max(30).optional(),
          sun_scale: z.number().min(0).max(20).optional(),
          twilight: z.boolean().optional(),
          ambient_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB').optional(),
          night_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB').optional(),
          // --- cielo / suelo (J) ---
          sky_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB, e.g. "#FFFFFF"').optional(),
          ground_show: z.boolean().optional(),
          ground_block: z.string().min(1).max(80).optional(),
          // --- nubes (J + K3) ---
          clouds_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB, e.g. "#FFFFFF"').optional(),
          clouds_show: z.boolean().optional(),
          clouds: z
            .object({
              show: z.boolean().optional(),
              color: z.string().regex(/^#?[0-9a-fA-F]{6}$/).optional(),
              speed: z.number().optional(),
              height: z.number().optional(),
              offset: z.number().optional(),
            })
            .optional(),
          // --- niebla (J + K3) ---
          fog_show: z.boolean().optional(),
          fog_sky: z.boolean().optional(),
          fog_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/, 'hex RGB, e.g. "#FFFFFF"').optional(),
          fog: z
            .object({
              show: z.boolean().optional(),
              sky: z.boolean().optional(),
              color: z.string().regex(/^#?[0-9a-fA-F]{6}$/).optional(),
              object_color: z.string().regex(/^#?[0-9a-fA-F]{6}$/).optional(),
              distance: z.number().optional(),
              size: z.number().optional(),
              height: z.number().optional(),
            })
            .optional(),
          // --- luna / bioma / viento (K3) ---
          moon: z
            .object({
              phase: z.number().optional(),
              angle: z.number().optional(),
              scale: z.number().optional(),
            })
            .optional(),
          biome: z.string().min(1).max(60).optional(),
          wind: z
            .object({
              on: z.boolean().optional(),
              speed: z.number().optional(),
              strength: z.number().optional(),
              direction: z.number().optional(),
            })
            .optional(),
          // --- animacion (K3) ---
          ensure_track: z.boolean().optional(),
          frame: z.number().int().min(0).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_background_set', 'cambiando el fondo y el sol del proyecto', async () => {
            const opArgs: Record<string, unknown> = {};
            const hex = (v: string) => v.replace(/^#/, '').toUpperCase();
            const dup: string[] = [];
            // Los grupos anidados (fog/clouds/moon/wind) y los campos planos
            // heredados de la ronda J desembocan en LA MISMA clave del op. Si
            // el modelo manda las dos formas, se le dice -- nunca se elige por
            // el en silencio.
            const put = (k: string, v: unknown) => {
              if (v === undefined) return;
              if (Object.prototype.hasOwnProperty.call(opArgs, k)) dup.push(k);
              opArgs[k] = typeof v === 'boolean' ? (v ? 1 : 0) : v;
            };
            const putHex = (k: string, v: string | undefined) => {
              if (v !== undefined) put(k, hex(v));
            };

            // --- sol / ambiente ---
            put('sun_azimuth', args.sun_azimuth);
            put('sun_elevation', args.sun_elevation);
            put('sun_time', args.sun_time);
            put('sun_rotation', args.sun_rotation);
            put('sun_strength', args.sun_strength);
            putHex('sun_color', args.sun_color);
            put('sun_softness', args.sun_softness);
            put('sun_scale', args.sun_scale);
            put('twilight', args.twilight);
            putHex('ambient_color', args.ambient_color);
            putHex('night_color', args.night_color);
            // --- cielo / suelo ---
            putHex('sky_color', args.sky_color);
            put('ground_show', args.ground_show);
            put('ground_block', args.ground_block);
            // --- nubes (plano de J primero, luego el grupo) ---
            putHex('clouds_color', args.clouds_color);
            put('clouds_show', args.clouds_show);
            if (args.clouds) {
              put('clouds_show', args.clouds.show);
              putHex('clouds_color', args.clouds.color);
              put('clouds_speed', args.clouds.speed);
              put('clouds_height', args.clouds.height);
              put('clouds_offset', args.clouds.offset);
            }
            // --- niebla ---
            put('fog_show', args.fog_show);
            put('fog_sky', args.fog_sky);
            putHex('fog_color', args.fog_color);
            if (args.fog) {
              put('fog_show', args.fog.show);
              put('fog_sky', args.fog.sky);
              putHex('fog_color', args.fog.color);
              putHex('fog_object_color', args.fog.object_color);
              put('fog_distance', args.fog.distance);
              put('fog_size', args.fog.size);
              put('fog_height', args.fog.height);
            }
            // --- luna / bioma / viento ---
            if (args.moon) {
              put('moon_phase', args.moon.phase);
              put('moon_angle', args.moon.angle);
              put('moon_scale', args.moon.scale);
            }
            put('biome', args.biome);
            if (args.wind) {
              put('wind', args.wind.on);
              put('wind_speed', args.wind.speed);
              put('wind_strength', args.wind.strength);
              put('wind_direction', args.wind.direction);
            }
            // --- animacion ---
            if (args.ensure_track) opArgs.ensure_track = 1;
            if (args.frame !== undefined) opArgs.frame = args.frame;

            if (dup.length > 0) {
              return {
                ok: false,
                text:
                  'Has mandado dos veces el mismo ajuste (' +
                  dup.join(', ') +
                  '): el campo plano y el del grupo anidado son el MISMO canal. Manda solo uno.',
              };
            }

            const onlyControl =
              Object.keys(opArgs).filter((k) => k !== 'ensure_track' && k !== 'frame').length === 0;
            if (onlyControl) {
              return {
                ok: false,
                text:
                  'No has pedido ningun cambio. Pasa al menos un ajuste, p.ej. sun_azimuth, ' +
                  'sun_elevation, sun_strength, sun_color, ambient_color, sky_color, ground_block, ' +
                  'clouds{}, fog{}, moon{}, biome o wind{}. (ensure_track y frame solo dicen DONDE ' +
                  'se escribe, no QUE se escribe.)',
              };
            }

            const res = await p.runOps(
              [{ id: 'bg1', op: 'assistant.env_set', args: opArgs }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            const { ok, text } = summarize(res);
            if (!ok) return { ok, text };
            const touchedSun =
              opArgs.sun_azimuth !== undefined ||
              opArgs.sun_elevation !== undefined ||
              opArgs.sun_time !== undefined ||
              opArgs.sun_rotation !== undefined;
            return {
              ok,
              text:
                text +
                '\n\nThis whole change is ONE undo step for the user. It affects the world settings ' +
                'only -- lights, cameras, objects and poses were not touched.' +
                (touchedSun
                  ? '\n\nYou MOVED THE SUN. sun_azimuth / sun_elevation / shadow_len_32 / ' +
                    'shadow_azimuth above are the EFFECTIVE values: say them in your answer, say ' +
                    'which way the shadows now fall, and re-derive anything you had planned from ' +
                    'cheat sheet 13.5/13.6 -- those tables are the row for the DEFAULT sun. Then ' +
                    'look at a preview before you describe the result.'
                  : ''),
            };
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA K0: autoreporte de huecos. Sin op de editor -- vive solo en el
      // cerebro (Data/assistant/gap_log.jsonl, ver gap.ts). Ver spec seccion 5:
      // "Regla 1 ampliada" -- la SIGUIENTE accion tras decirle al usuario "esto
      // no puedo hacerlo" es esta llamada, sin preguntar si se registra.
      // -----------------------------------------------------------------

      tool(
        'mi3_report_gap',
        'Record that the user asked for something you could not do. Call this the moment you have ' +
          'to tell the user "I cannot do X" -- it is how the developer finds out what to build next. ' +
          'One call per distinct missing capability. Do NOT use it for things you simply did not ' +
          'want to do, for user mistakes, or for anything you actually managed to do another way. ' +
          'This never fails and does not touch the live editor -- it only writes to a local log; ' +
          'there is no need to ask the user for permission before calling it. ' +
          'Call it again every time the user asks for the same missing thing, in this turn or a ' +
          'later one -- the log dedupes by content and counts the repeats, and that count is the ' +
          'ranking. Repeats are the point, not noise.',
        {
          want: z.string().min(GAP_LIMITS.minWantChars).max(GAP_LIMITS.maxWantChars),
          why: z.enum(['no_tool', 'tool_failed', 'not_measured', 'refused_by_policy']),
          tried: z.string().max(GAP_LIMITS.maxTriedChars).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_report_gap', `registrando hueco: ${args.want.slice(0, 60)}`, async () => {
            const r = ctx.gapLog.report({
              want: args.want,
              why: args.why,
              tried: args.tried,
              turnId: ctx.turnId,
              project: ctx.projectName,
              model: ctx.model,
            });
            p.emit({ type: 'gap', turn_id: ctx.turnId, want: args.want, n: r.n });
            return {
              ok: true,
              text:
                `Registrado (visto ${r.n} ${r.n === 1 ? 'vez' : 'veces'} en total). El usuario ya ve ` +
                'un aviso discreto en el chat -- no le preguntes si quiere que lo registres, y no ' +
                'hace falta que lo menciones a menos que encaje de forma natural en tu respuesta.',
            };
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA K1: ajustes de proyecto/render por lote + linea de tiempo
      // (marcadores, longitud, playhead). Ver SPEC_ASSISTANT_RONDA_K.md
      // seccion 7.1/7.2. mi3_project_set toca ~60 campos con UNA sola
      // entrada de historial por llamada (assistant.project_set,
      // mi3_assistant_project.gml); mi3_timeline envuelve assistant.marker
      // (ya existia), timeline.set_length (ya existia) y assistant.markers
      // (nuevo, mi3_assistant_timeline.gml).
      // -----------------------------------------------------------------

      tool(
        'mi3_project_set',
        'Read or change PROJECT-WIDE render/output settings -- the "Render" tab of the project, not ' +
          'a per-camera override. Covers: render engine, samples, SSAA, render distance, shadows ' +
          '(master + per-light buffer sizes + transparent shadows), SSAO, indirect light (GI), ' +
          'reflections (SSR), glow/bloom, antialiasing, tonemapper/exposure/gamma/ambient strength, ' +
          'vanilla Minecraft face-shade, bend style, subsurface scattering, block material toggles ' +
          '(opaque leaves, liquid animation, water reflections, block emissive, PBR material maps), ' +
          'enchant glint, texture filtering, global alpha mode, overscan/tiled rendering, the Cinema ' +
          "engine's bounces/noise/denoise/clip/env_light and its FILM BACKDROP " +
          '(cinema_backdrop 0=off 1=solid 2=gradient, cinema_backdrop_color, ' +
          'cinema_backdrop_color2, cinema_backdrop_angle), frame rate (tempo), and output video ' +
          "width/height/" +
          'aspect/template. Unlike a camera\'s own tonemapper/exposure/gamma overrides -- which the ' +
          'live preview and the quality render do NOT apply -- these PROJECT-level settings show up ' +
          'in both, so this is the tool to reach for when the user asks for a look, not camera value ' +
          'overrides.\n\n' +
          'op:"get" reads every supported field back as one line "field=value;field2=value2;...". ' +
          'op:"set" applies a batch of named fields (pass "settings": {field: value, ...}) as ONE ' +
          'undo step for the user -- Ctrl+Z reverts the whole batch together. Unknown field names are ' +
          'never silently ignored: they come back in "skipped" together with the full list of valid ' +
          'names. op:"preset_list" lists the built-in .mirender presets (performance/product/' +
          'cinematic/...) by name. op:"preset_load" applies one of those by "preset" name -- this is ' +
          'what "put it in cinema quality" means. ' +
          '"engine" picks the renderer and only accepts 0, 1 or 2 (anything else is an explicit ' +
          'error): 0 = classic (the old forward renderer: no SSAO/GI/SSR, fastest, and it is what ' +
          'a project saved with the "performance"/"balanced"/"extreme" preset comes back as); ' +
          '1 = next-gen raster, the default and the right answer almost always (deferred, real ' +
          'shadows, SSAO/GI/SSR, every measured number in the cheat sheet was taken here); ' +
          '2 = Cinema, the path tracer -- it has no UI yet but it WORKS from here (measured: a ' +
          '480x270/8spp frame took 5.3 s against 0.1 s on engine 1, no error), it is what ' +
          'mi3_export op:"layers" needs for real depth slicing, and it is minutes per frame at ' +
          'delivery sizes. Say which engine you are on when it matters, and put it back to 1 when ' +
          'you are done with layers. ' +
          '"bend_style" is a STRING, not a number: "blocky" (default, sharp corners) or ' +
          '"realistic" (rounded bends). Anything else is rejected with an explicit error. It is ' +
          'the one field that costs a SECOND undo step: it goes through its own editor action ' +
          '(it rebuilds every bent mesh), so a batch that mixes it with other fields needs two ' +
          'Ctrl+Z -- the result tells you so in "history_entries". ' +
          'Before telling the user specific numbers you changed, call op:"get" and read them back ' +
          '-- never assume a write landed exactly as sent.',
        {
          op: z.enum(['get', 'set', 'preset_list', 'preset_load']),
          preset: z.string().optional(),
          settings: z.record(z.string(), z.union([z.number(), z.string(), z.boolean()])).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_project_set', `proyecto: ${args.op}`, async () => {
            if (args.op === 'get') {
              const res = await p.runOps(
                [{ id: 'pg1', op: 'assistant.project_get', args: {} }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'set') {
              if (!args.settings || Object.keys(args.settings).length === 0) {
                return {
                  ok: false,
                  text: 'No has pasado "settings". Pasa al menos un campo "campo: valor" (usa op:"get" primero si no sabes los nombres).',
                };
              }
              const res = await p.runOps(
                [{ id: 'ps1', op: 'assistant.project_set', args: { settings: args.settings } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'preset_list') {
              const res = await p.runOps(
                [{ id: 'pl1', op: 'assistant.preset_list', args: {} }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            // preset_load
            if (!args.preset) {
              return {
                ok: false,
                text: 'Falta "preset" (el nombre del .mirender, p.ej. "cinematic"). Llama a op:"preset_list" si no lo sabes.',
              };
            }
            const res = await p.runOps(
              [{ id: 'plo1', op: 'project.load_preset', args: { name: args.preset } }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_timeline',
        'Move the timeline playhead, change the timeline\'s total length, or manage scene markers ' +
          '(named, coloured pins on the timeline ruler that mark where a scene/shot begins -- pure ' +
          'organisation, they do not affect the render). op:"goto" moves the playhead to "frame" ' +
          '(same effect as clicking the ruler; mi3_keyframe_add moves it for you too, this is for ' +
          'when you just want to look at or narrate a specific moment). op:"length" sets the total ' +
          'timeline length in frames. op:"marker_add" creates one at "frame" (optional "name" and ' +
          '"color" 0-8; omit either for sensible defaults). op:"marker_edit" changes name/color/frame ' +
          'of an existing marker addressed by "ref" -- either its exact name or its 1-based position ' +
          'in the list op:"markers" returns. op:"marker_delete" removes it by the same "ref". ' +
          'op:"markers" lists every marker as "name|frame|color". Each write is its own undo step.',
        {
          op: z.enum(['goto', 'length', 'marker_add', 'marker_edit', 'marker_delete', 'markers']),
          frame: z.number().int().min(0).optional(),
          length: z.number().int().min(1).optional(),
          name: z.string().max(60).optional(),
          color: z.number().int().min(0).max(8).optional(),
          ref: z.string().optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_timeline', `timeline: ${args.op}`, async () => {
            if (args.op === 'goto') {
              if (args.frame === undefined) return { ok: false, text: 'Falta "frame".' };
              const res = await p.runOps(
                [{ id: 'tg1', op: 'assistant.marker', args: { frame: args.frame } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'length') {
              if (args.length === undefined) return { ok: false, text: 'Falta "length".' };
              const res = await p.runOps(
                [{ id: 'tl1', op: 'timeline.set_length', args: { frames: args.length } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'markers') {
              const res = await p.runOps(
                [{ id: 'tm1', op: 'assistant.markers', args: { action: 'list' } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'marker_add') {
              if (args.frame === undefined) return { ok: false, text: 'Falta "frame".' };
              const opArgs: Record<string, unknown> = { action: 'add', frame: args.frame };
              if (args.name !== undefined) opArgs.name = args.name;
              if (args.color !== undefined) opArgs.color = args.color;
              const res = await p.runOps(
                [{ id: 'tma1', op: 'assistant.markers', args: opArgs }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'marker_edit') {
              if (!args.ref) return { ok: false, text: 'Falta "ref" (nombre o indice del marcador; usa op:"markers" para verlos).' };
              const opArgs: Record<string, unknown> = { action: 'edit', ref: args.ref };
              if (args.name !== undefined) opArgs.name = args.name;
              if (args.color !== undefined) opArgs.color = args.color;
              if (args.frame !== undefined) opArgs.frame = args.frame;
              const res = await p.runOps(
                [{ id: 'tme1', op: 'assistant.markers', args: opArgs }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            // marker_delete
            if (!args.ref) return { ok: false, text: 'Falta "ref" (nombre o indice del marcador; usa op:"markers" para verlos).' };
            const res = await p.runOps(
              [{ id: 'tmd1', op: 'assistant.markers', args: { action: 'delete', ref: args.ref } }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA K2: keyframes (list/delete/move/transition/copy/paste), edicion
      // de objeto (rename/reparent/hide/appearance/inherit/color_tag/
      // rot_point/transform copy-paste/mirror_pose/focus_on/ortho/ik) y redo.
      // Ver SPEC_ASSISTANT_RONDA_K.md seccion 7.3/7.4/7.5.
      // -----------------------------------------------------------------

      tool(
        'mi3_keyframes',
        'List, delete, move (retime), re-transition, copy or paste the keyframes of ONE object. ' +
          'This is what turns single poses (mi3_set_values / mi3_keyframe_add) into actual ' +
          'animation you can iterate on. op:"list" returns every keyframe as ' +
          '"frame|transition|channels;..." -- "channels" is how many value channels actually change ' +
          'AT that keyframe versus the previous one (or versus the object\'s defaults for the first ' +
          'keyframe), so you can tell a full pose from a one-value tweak without re-reading every ' +
          'channel by hand. op:"delete" removes the keyframes at "frames" (positions, not indices) -- ' +
          'a real editor delete, one undo step. op:"move" retimes the keyframes at "frames": pass ' +
          '"delta" to shift all of them by the same number of frames, or "to" to say where the FIRST ' +
          'one in "frames" should land (the rest keep their relative spacing). Values are NEVER ' +
          'touched -- the exact same keyframe is relocated, so every channel stays byte-identical. ' +
          'op:"transition" sets the easing curve of the keyframes at "frames" to "transition" -- this ' +
          'is what gives motion weight (a linear fall looks like it is floating; easeoutbounce or ' +
          'easeoutquad on the landing keyframe looks like it actually has mass). "smooth" is a real, ' +
          'implemented Catmull-Rom curve through neighboring keyframes, not a placeholder. An unknown ' +
          'transition name fails with the full 34-name catalog in the error -- call it again with the ' +
          'catalog, do not guess a second name. op:"copy" copies the keyframes at "frames" (all of ' +
          'them if you omit "frames") from "ref" into a session clipboard, aligned so the earliest one ' +
          'is offset 0 -- perfect for cloning a walk-cycle beat onto another character. op:"paste" ' +
          'drops that clipboard onto "target_ref" (defaults to "ref") starting at frame "at"; call ' +
          'copy first in the same or an earlier turn or this fails. Every write here is a real, ' +
          'undoable (mi3_undo) / redoable (mi3_redo) editor action, and counts against a per-turn ' +
          'budget of 80 keyframe writes total.',
        {
          ref: z.string(),
          action: z.enum(['list', 'delete', 'move', 'transition', 'copy', 'paste']),
          frames: z.array(z.number().int()).max(64).optional(),
          to: z.number().int().optional(),
          delta: z.number().int().optional(),
          transition: z
            .enum([
              'linear', 'instant', 'bezier', 'smooth',
              'easeinquad', 'easeoutquad', 'easeinoutquad',
              'easeincubic', 'easeoutcubic', 'easeinoutcubic',
              'easeinquart', 'easeoutquart', 'easeinoutquart',
              'easeinquint', 'easeoutquint', 'easeinoutquint',
              'easeinsine', 'easeoutsine', 'easeinoutsine',
              'easeinexpo', 'easeoutexpo', 'easeinoutexpo',
              'easeincirc', 'easeoutcirc', 'easeinoutcirc',
              'easeinelastic', 'easeoutelastic', 'easeinoutelastic',
              'easeinback', 'easeoutback', 'easeinoutback',
              'easeinbounce', 'easeoutbounce', 'easeinoutbounce',
            ])
            .optional(),
          target_ref: z.string().optional(),
          at: z.number().int().optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_keyframes', `keyframes: ${args.action} en ${args.ref}`, async () => {
            // Tope por turno (2.4 del spec): vive en el CODIGO. "list"/"copy" no
            // escriben nada y no cuentan.
            if (['delete', 'move', 'transition', 'paste'].includes(args.action)) {
              const want = args.action === 'paste' ? 1 : (args.frames?.length ?? 0);
              // "paste" no se sabe cuantos keyframes trae el portapapeles desde
              // aqui (vive en el editor); se cuenta como 1 escritura de solicitud
              // y se corrige con el numero real que devuelva el op.
              if (ctx.keyframeWritesN + want > KEYFRAME_WRITES_CAP_PER_TURN) {
                return {
                  ok: false,
                  text:
                    `Ya llevas ${ctx.keyframeWritesN} escrituras de keyframe en este turno (tope ` +
                    `${KEYFRAME_WRITES_CAP_PER_TURN}); esta llamada pediria ${want} mas. Pideselo al ` +
                    'usuario en otro turno, o reduce cuantos frames tocas de una vez.',
                };
              }
            }

            const opArgs: Record<string, unknown> = { ref: args.ref, action: args.action };
            if (args.frames !== undefined) opArgs.frames = args.frames;
            if (args.to !== undefined) opArgs.to = args.to;
            if (args.delta !== undefined) opArgs.delta = args.delta;
            if (args.transition !== undefined) opArgs.transition = args.transition;
            if (args.target_ref !== undefined) opArgs.target_ref = args.target_ref;
            if (args.at !== undefined) opArgs.at = args.at;

            if (args.action === 'delete' && !args.frames?.length)
              return { ok: false, text: 'Falta "frames" (posiciones de los keyframes a borrar).' };
            if (args.action === 'move') {
              if (!args.frames?.length) return { ok: false, text: 'Falta "frames" (posiciones de los keyframes a mover).' };
              if (args.to === undefined && args.delta === undefined)
                return { ok: false, text: 'Falta "to" o "delta".' };
            }
            if (args.action === 'transition') {
              if (!args.frames?.length) return { ok: false, text: 'Falta "frames" (posiciones de los keyframes a retransicionar).' };
              if (!args.transition) return { ok: false, text: 'Falta "transition".' };
            }
            if (args.action === 'paste' && args.at === undefined)
              return { ok: false, text: 'Falta "at" (frame de destino del pegado).' };

            const res = await p.runOps(
              [{ id: 'kf1', op: 'assistant.keyframes', args: opArgs }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            const { ok, text } = summarize(res);
            if (ok) {
              const r0 = res.results?.[0] as OpResultEntry | undefined;
              const n =
                (typeof r0?.deleted === 'number' && r0.deleted) ||
                (typeof r0?.moved === 'number' && r0.moved) ||
                (typeof r0?.applied === 'number' && r0.applied) ||
                (typeof r0?.pasted === 'number' && r0.pasted) ||
                0;
              ctx.keyframeWritesN += n;
            }
            return { ok, text };
          }),
      ),

      tool(
        'mi3_object_edit',
        'Edit properties of an EXISTING object that do not fit mi3_set_values (those live on ' +
          'value[] channels; these are separate object properties, but still real, undoable editor ' +
          'actions). op:"rename" changes its display name. op:"reparent" attaches it under "parent" ' +
          '(pass "" to move it to the scene root) -- this is the "attach" operation: dressing a ' +
          'character in armor, rigging a prop to a hand, etc. IMPORTANT for armor/attachments: parent ' +
          'EACH piece to the body part of the SAME NAME on the wearer (Body-to-Body, Head-to-Head, ' +
          'Right arm-to-Right arm...) -- NEVER parent the whole armor set to the wearer\'s Body or ' +
          'root. The wearer\'s legs are SIBLINGS of Body, not children of it, so hanging a leg-armor ' +
          'piece off Body is exactly what breaks a walk cycle. After attaching, verify: move one of ' +
          'the wearer\'s limbs and confirm with mi3_object_get that the matching armor piece moved ' +
          'with it. op:"hide" shows/hides it -- this ALWAYS cascades to every child (matches what the ' +
          'real editor does when you hide a parent; there is no way to hide only the parent and leave ' +
          'children visible). op:"appearance" sets up to 7 rendering flags (backfaces, shadows, ssao, ' +
          'glow, blend_mode: normal/add/subtract/multiply/screen, alpha_mode: blend/hashed/default, ' +
          'fog) -- these ALSO always cascade to the whole subtree, and the whole batch you pass in one ' +
          'call is ONE undo step no matter how many descendants it touches. op:"inherit" toggles ' +
          'whether THIS object inherits a property from its parent instead of using its own value -- ' +
          'keys: position, rotation, scale, bend, pose, rot_point, texture, color, alpha, glow_color, ' +
          'subsurface, surface, visibility, select (each a boolean); get this wrong on an attached ' +
          'object and it can inherit a parent\'s scale and warp. op:"color_tag" sets the object\'s ' +
          'organizer color (0-9, 0 = none) -- purely organizational, no render effect. op:"rot_point" ' +
          'moves the pivot the object rotates/scales around, as a LOCAL [x,y,z] offset -- use this ' +
          'before animating an orbit or a hinge that should not rotate around the model\'s center. ' +
          'op:"transform_copy" copies POS/ROT/SCA off "ref"; op:"transform_paste" pastes that onto ' +
          '"ref" as a real keyframed write (whatever channels the destination actually has). ' +
          'op:"mirror_pose" copies ROT+BEND from each named body part of "ref" onto the SAME-NAME part ' +
          'of "target" ONCE (does not flip left/right, and does not keep tracking -- for armor you do ' +
          'NOT need it: kind:"armor" with wear_on already tracks the wearer live). ' +
          'op:"focus_on" sets a camera\'s ("ref") depth-of-field focus distance to the planar distance ' +
          'to "target" -- use this instead of guessing a focus distance by hand. op:"ortho" toggles ' +
          'orthographic mode on a camera ("ref") while automatically rescaling CAM_ORTHO_SCALE to keep ' +
          'the same framing (pass "enable"). op:"ik" is the one place object properties overlap with ' +
          'value[] channels: it sets a two-bone IK "target" (another object\'s ref -- this is a live ' +
          'object reference, which is why mi3_set_values cannot do it), "ik_blend" (0-1, how much the ' +
          'IK solver overrides the normal pose) and/or "ik_angle_offset" (degrees). IK is NOT a well- ' +
          'measured recipe here -- use it when asked, then actually look at a preview before ' +
          'describing the result; do not claim a specific bend looks right without checking. ' +
          'op:"trim" re-trims an armor object already in the scene (ShinyTrims O6): "piece" ' +
          '(all/helmet/chestplate/leggings/boots, default all), "pattern", "material", "effect" ' +
          '(one of the 20, or none), "intensity". On a PLAIN armor object (built without "trim" at ' +
          'creation, or the template pipeline) all four fields are editable. On an armor object that ' +
          'was CREATED with "trim" at mi3_object_add time, the baked pattern/material are part of the ' +
          'asset\'s identity and cannot change in place -- only effect/intensity are editable there; ' +
          'to change pattern/material, delete and recreate with mi3_object_add.\n\n' +
          'op:"item_trim" is the same idea for an ITEM object -- a sword, axe, pickaxe, shovel, hoe, ' +
          'mace, spear, bow or crossbow added with mi3_object_add kind:"item" (ShinyTrims O7). Use it ' +
          'when the ref is an item and op:"trim" when the ref is armor; they are separate ops because ' +
          'the mechanism is different (an item\'s trim is painted into the item\'s own picture), and ' +
          'picking the wrong one fails with a message naming the right one instead of doing damage. ' +
          'Unlike armor, an item\'s "pattern" and "material" ARE editable in place, as often as you ' +
          'like, and pattern:"none" removes the trim and gives the plain vanilla item back. Not every ' +
          'item can be trimmed -- only the tool/weapon shapes listed above; there is no mask for an ' +
          'apple or a block, and the call says so if you try. "piece" does not apply here.',
        {
          op: z.enum([
            'rename', 'reparent', 'hide', 'appearance', 'inherit', 'color_tag', 'rot_point',
            'transform_copy', 'transform_paste', 'mirror_pose', 'focus_on', 'ortho', 'ik', 'trim',
            'item_trim',
          ]),
          ref: z.string(),
          name: z.string().min(1).max(60).optional(),
          parent: z.string().optional(),
          hidden: z.boolean().optional(),
          tree: z.boolean().default(false).optional(),
          appearance: z
            .object({
              backfaces: z.boolean().optional(),
              shadows: z.boolean().optional(),
              ssao: z.boolean().optional(),
              glow: z.boolean().optional(),
              blend_mode: z.enum(['normal', 'add', 'subtract', 'multiply', 'screen']).optional(),
              alpha_mode: z.enum(['blend', 'hashed', 'default']).optional(),
              fog: z.boolean().optional(),
            })
            .optional(),
          inherit: z.record(z.string(), z.boolean()).optional(),
          color_tag: z.number().int().min(0).max(9).optional(),
          rot_point: z.array(z.number()).length(3).optional(),
          target: z.string().optional(),
          enable: z.boolean().optional(),
          ik_blend: z.number().min(0).max(1).optional(),
          ik_angle_offset: z.number().optional(),
          // op: 'trim' (ShinyTrims O6) -- mismo schema que el op headless
          // armor.set_trim, 1:1, sin renombrados.
          trim: z
            .object({
              piece: z.enum(['all', 'helmet', 'chestplate', 'leggings', 'boots']).optional(),
              pattern: z.string().optional(),
              material: z.string().optional(),
              effect: zTrimEffect.optional(),
              intensity: z.number().min(0).max(4).optional(),
              // ShinyTrims O9 -- SOLO op:'item_trim'. La CASCARA DEL OUTLINE
              // 3D: un halo de geometria (no de shader) que abraza la silueta
              // del item. 'thick' es el grosor que el pack usa en tercera
              // persona y 'thin' el de primera persona; en MI3 no hay
              // contextos de display, asi que se elige a mano. Solo tiene
              // sentido con patron: outline sin trim FALLA con "primero dale
              // un patron", no se ignora.
              outline: z.enum(['none', 'thin', 'thick']).optional(),
              // Winding de la cascara. 'o2' (por defecto) emite las caras
              // hacia fuera; 'o1' es el cubo invertido literal del pack. Es un
              // flag de COMPARACION, no una preferencia: los dos dibujan el
              // mismo borde y solo cambian en angulos oblicuos y en la normal
              // del G-buffer. No lo toques salvo que te lo pidan.
              outline_mode: z.enum(['o1', 'o2']).optional(),
            })
            .optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_object_edit', `object_edit ${args.op} en ${args.ref}`, async () => {
            if (args.op === 'reparent') {
              const res = await p.runOps(
                [{ id: 'oe1', op: 'object.parent', args: { ref: args.ref, parent: args.parent ?? '' } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'transform_copy') {
              const res = await p.runOps(
                [{ id: 'oe1', op: 'transform.copy', args: { ref: args.ref } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'transform_paste') {
              const res = await p.runOps(
                [{ id: 'oe1', op: 'transform.paste', args: { ref: args.ref } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'mirror_pose') {
              if (!args.target) return { ok: false, text: 'Falta "target" (el objeto que recibe la pose reflejada).' };
              const res = await p.runOps(
                [{ id: 'oe1', op: 'object.mirror_pose', args: { from: args.ref, to: args.target } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'focus_on') {
              if (!args.target) return { ok: false, text: 'Falta "target" (el objeto a enfocar).' };
              const res = await p.runOps(
                [{ id: 'oe1', op: 'camera.focus_on', args: { ref: args.ref, target: args.target } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'ortho') {
              const res = await p.runOps(
                [{ id: 'oe1', op: 'camera.ortho_toggle', args: { ref: args.ref, enable: (args.enable ?? true) ? 1 : 0 } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'item_trim') {
              // ShinyTrims O7 (tool trims). Op SEPARADO de 'trim' a proposito:
              // el trim de un item se compone en su PNG y el de una armadura
              // vive en armor_array. No se adivina cual toca mirando el ref --
              // el motor ya devuelve un error que NOMBRA el op correcto si el
              // modelo se equivoca, que es mas barato que un despacho
              // implicito que acierte casi siempre.
              if (!args.trim || Object.keys(args.trim).length === 0)
                return {
                  ok: false,
                  text: 'Falta "trim" (pattern/material/effect/intensity/outline -- al menos uno; pattern:"none" quita el trim).',
                };
              const ti = args.trim;
              if (ti.piece !== undefined)
                return { ok: false, text: '"piece" no aplica a un item (no tiene piezas); quitalo o usa op:"trim" si el ref es una armadura.' };
              if (ti.intensity !== undefined && (ti.intensity < 0 || ti.intensity > 4))
                return { ok: false, text: 'intensity fuera de rango (0-4).' };
              const itArgs: Record<string, unknown> = {};
              if (ti.pattern !== undefined) itArgs.pattern = ti.pattern;
              if (ti.material !== undefined) itArgs.material = ti.material;
              if (ti.effect !== undefined) itArgs.effect = ti.effect;
              if (ti.intensity !== undefined) itArgs.intensity = ti.intensity;
              // ShinyTrims O9. No se pre-valida "outline sin patron" aqui: la
              // jaula vive en el motor (item.set_trim valida ANTES de elegir
              // rama) y duplicarla aqui daria dos textos distintos para el
              // mismo fallo. Lo que si se hace es pasarlo tal cual, sin
              // renombrar, que es la regla de esta tool desde O6.
              if (ti.outline !== undefined) itArgs.outline = ti.outline;
              if (ti.outline_mode !== undefined) itArgs.outline_mode = ti.outline_mode;
              const res = await p.runOps(
                [{ id: 'oe1', op: 'item.set_trim', args: { target: args.ref, ...itArgs } }],
                ctx.turnId,
                20000,
                ctx.signal,
              );
              return summarize(res);
            }
            if (args.op === 'trim') {
              if (!args.trim || Object.keys(args.trim).length === 0)
                return { ok: false, text: 'Falta "trim" (piece/pattern/material/effect/intensity -- al menos uno).' };
              const t = args.trim;
              // Rango de intensity se rechaza aqui (zod ya lo valida arriba,
              // pero el mensaje explicito importa para K5: falla ANTES de
              // tocar el editor y no consume tope de turno).
              if (t.intensity !== undefined && (t.intensity < 0 || t.intensity > 4))
                return { ok: false, text: 'intensity fuera de rango (0-4).' };
              const trimArgs: Record<string, unknown> = {};
              if (t.piece !== undefined) trimArgs.piece = t.piece;
              if (t.pattern !== undefined) trimArgs.pattern = t.pattern;
              if (t.material !== undefined) trimArgs.material = t.material;
              if (t.effect !== undefined) trimArgs.effect = t.effect;
              if (t.intensity !== undefined) trimArgs.intensity = t.intensity;
              const res = await p.runOps(
                [{ id: 'oe1', op: 'armor.set_trim', args: { target: args.ref, ...trimArgs } }],
                ctx.turnId,
                15000,
                ctx.signal,
              );
              return summarize(res);
            }

            // rename / hide / appearance / inherit / color_tag / rot_point / ik
            // -> assistant.object_edit {ref, action, ...}
            const opArgs: Record<string, unknown> = { ref: args.ref, action: args.op };
            if (args.op === 'rename') {
              if (!args.name) return { ok: false, text: 'Falta "name".' };
              opArgs.name = args.name;
            } else if (args.op === 'hide') {
              if (args.hidden === undefined) return { ok: false, text: 'Falta "hidden".' };
              opArgs.hidden = args.hidden;
            } else if (args.op === 'appearance') {
              if (!args.appearance || Object.keys(args.appearance).length === 0)
                return { ok: false, text: 'Falta "appearance" (al menos uno de backfaces/shadows/ssao/glow/blend_mode/alpha_mode/fog).' };
              opArgs.appearance = args.appearance;
            } else if (args.op === 'inherit') {
              if (!args.inherit || Object.keys(args.inherit).length === 0)
                return { ok: false, text: 'Falta "inherit" (al menos un campo: position/rotation/scale/bend/pose/rot_point/texture/color/alpha/glow_color/subsurface/surface/visibility/select).' };
              opArgs.inherit = args.inherit;
            } else if (args.op === 'color_tag') {
              if (args.color_tag === undefined) return { ok: false, text: 'Falta "color_tag" (0-9).' };
              opArgs.color_tag = args.color_tag;
            } else if (args.op === 'rot_point') {
              if (!args.rot_point) return { ok: false, text: 'Falta "rot_point" ([x,y,z]).' };
              opArgs.rot_point = args.rot_point;
            } else if (args.op === 'ik') {
              if (args.target === undefined && args.ik_blend === undefined && args.ik_angle_offset === undefined)
                return { ok: false, text: 'ik: nada que aplicar (pasa target, ik_blend o ik_angle_offset).' };
              if (args.target !== undefined) opArgs.target = args.target;
              if (args.ik_blend !== undefined) opArgs.ik_blend = args.ik_blend;
              if (args.ik_angle_offset !== undefined) opArgs.ik_angle_offset = args.ik_angle_offset;
            }

            const res = await p.runOps(
              [{ id: 'oe1', op: 'assistant.object_edit', args: opArgs }],
              ctx.turnId,
              15000,
              ctx.signal,
            );
            return summarize(res);
          }),
      ),

      tool(
        'mi3_redo',
        'Redo the last N editor actions undone with mi3_undo (like pressing Ctrl+Y N times).',
        {
          count: z.number().int().min(1).max(20).default(1).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_redo', `rehaciendo ${args.count ?? 1} paso(s)`, async () => {
            const n = args.count ?? 1;
            const ops: Op[] = Array.from({ length: n }, (_, i) => ({ id: `r${i + 1}`, op: 'edit.redo' }));
            const res = await p.runOps(ops, ctx.turnId, 15000, ctx.signal);
            return summarize(res);
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA K4: traer cosas de fuera, con jaula de rutas (spec seccion 2.3
      // + 7.6). Una ruta SOLO es valida si (a) el usuario la escribio
      // TEXTUALMENTE en el mensaje de ESTE turno, o (b) cae dentro de una
      // raiz designada resuelta por el ANFITRION (nunca por el modelo, ver
      // paths.ts). "library" es la UNICA forma de enumerar disco -- una
      // raiz fija, sin argumento de ruta, no recursiva, tope 200.
      // -----------------------------------------------------------------

      tool(
        'mi3_import',
        'Bring something in from outside the project: a skin PNG, a model/particle/object asset ' +
          '(.bbmodel/.miobject/.miproject/.miparticles), a Minecraft world/schematic, a generic ' +
          'resource, a sticker-pack item, or a LISTING of what is available (op:"library"). Every ' +
          'path is checked by the host against a cage BEFORE anything touches disk: it only works ' +
          'if the path is either (a) literally written in the user\'s message THIS turn, or (b) an ' +
          'attachment the user just sent, or (c) something op:"library" already showed you. You can ' +
          'NEVER explore the disk yourself, and a path from memory or from your own earlier guess ' +
          'is always rejected -- if you are not sure a file exists, call op:"library" first (up to ' +
          '3 listings per turn) instead of guessing a name. Budget: 4 imports per turn (op:"library" ' +
          'does not count against that, it has its own 3-per-turn cap) -- imports can be slow ' +
          '(a world box can take up to ~2 minutes), so do not call this speculatively.\n\n' +
          'op:"skin" needs "ref" (the character to re-skin) and either "attachment" (an image the ' +
          'user attached THIS turn -- the preferred, path-free route) or "path". op:"asset" imports ' +
          'a .bbmodel/.miobject/.miproject/.miparticles as a new scene object (name/parent optional) ' +
          '-- prefer mi3_object_add kind:"model"/"particles" for the common case, this is for ' +
          '.miobject/.miproject specifically. op:"world" imports a Minecraft save folder (fixed ' +
          '64x64x64 box around the world origin) or a .schematic/.nbt/.blocks file, placed at ' +
          '"position" (default origin). op:"resource" imports a generic asset by "resource_type" ' +
          '(e.g. "texture", "font", "sound") without spawning a scene object. op:"sticker_item" adds ' +
          'a custom 2D item from an installed sticker pack -- pass "pack" and "item" exactly as ' +
          'op:"library" kind:"sticker_packs"/"sticker_items" reported them.',
        {
          op: z.enum(['skin', 'asset', 'world', 'resource', 'library', 'sticker_item']),
          path: z.string().optional(),
          attachment: z.string().optional(),
          ref: z.string().optional(),
          name: z.string().max(60).optional(),
          parent: z.string().optional(),
          position: z.array(z.number()).length(3).optional(),
          resource_type: z.string().optional(),
          pack: z.string().optional(),
          item: z.string().optional(),
          kind: z
            .enum(['projects', 'project_assets', 'particles', 'renders', 'attachments', 'sticker_packs', 'sticker_items'])
            .optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_import', `import ${args.op}`, async () => {
            // ---- op:"library" -- listado enjaulado, tope APARTE ----
            if (args.op === 'library') {
              if (!args.kind) return { ok: false, text: 'Falta "kind" (projects/project_assets/particles/renders/attachments/sticker_packs/sticker_items).' };
              const left = Math.max(0, LIBRARY_LISTS_CAP_PER_TURN - ctx.libraryListsN);
              if (left === 0) {
                return {
                  ok: false,
                  text: `Ya has pedido ${ctx.libraryListsN} listados en este turno; ese es el tope (${LIBRARY_LISTS_CAP_PER_TURN}). Pidelo en otro turno.`,
                };
              }
              // RONDA L (H3b): "projects" ya NO pasa por el op del motor (ese
              // listado es plano a proposito y no ve <nombre>/<nombre>.miproject).
              // Lo hace el anfitrion, que ya conoce roots.projects (via
              // assistant.paths, nunca del modelo) -- gasta el mismo cupo.
              if (args.kind === 'projects') {
                const roots = await getDesignatedRoots(p, ctx);
                if (!roots.projects) return { ok: false, text: 'esa raiz no esta disponible ahora mismo (projects)' };
                ctx.libraryListsN++;
                const listing = listProjectsHost(roots.projects);
                return formatProjectsListing('assistant.library', listing);
              }
              const opArgs: Record<string, unknown> = { kind: args.kind };
              if (args.kind === 'sticker_items') {
                if (!args.pack) return { ok: false, text: 'Falta "pack" (kind:"sticker_items" necesita el nombre exacto de un pack -- pide kind:"sticker_packs" primero).' };
                opArgs.pack = args.pack;
              }
              if (args.kind === 'attachments') {
                const roots = await getDesignatedRoots(p, ctx);
                opArgs.dir = roots.attachments;
              }
              ctx.libraryListsN++;
              const res = await p.runOps([{ id: 'lib1', op: 'assistant.library', args: opArgs }], ctx.turnId, 15000, ctx.signal);
              return summarize(res);
            }

            // ---- op:"sticker_item" -- catalogo cerrado, no jaula de rutas ----
            if (args.op === 'sticker_item') {
              if (!args.pack || !args.item) {
                return { ok: false, text: 'Faltan "pack"/"item" (pide mi3_import op:"library" kind:"sticker_packs"/"sticker_items" primero).' };
              }
              const left = Math.max(0, IMPORTS_CAP_PER_TURN - ctx.importsN);
              if (left === 0) {
                return {
                  ok: false,
                  text: `Ya has importado ${ctx.importsN} archivos en este turno; ese es el tope (${IMPORTS_CAP_PER_TURN}). Pidelo en otro turno.`,
                };
              }
              const opArgs: Record<string, unknown> = { pack: args.pack, item: args.item };
              if (args.name !== undefined) opArgs.name = args.name;
              if (args.parent !== undefined) opArgs.parent = args.parent;
              ctx.importsN++;
              const res = await p.runOps([{ id: 'sk1', op: 'assistant.sticker_add', args: opArgs }], ctx.turnId, 20000, ctx.signal);
              const { ok, text } = summarize(res);
              const okCount = (res.results ?? []).filter((r) => !!(r as OpResultEntry).ok).length;
              ctx.createdN += okCount;
              return { ok, text };
            }

            // ---- skin/asset/world/resource -- la jaula de rutas de verdad ----
            const left = Math.max(0, IMPORTS_CAP_PER_TURN - ctx.importsN);
            if (left === 0) {
              return {
                ok: false,
                text: `Ya has importado ${ctx.importsN} archivos en este turno; ese es el tope (${IMPORTS_CAP_PER_TURN}). Pidele al usuario que te lo pida en otro turno.`,
              };
            }

            let resolvedPath: string | undefined;
            if (args.attachment) {
              const attached = resolveAttachmentPath(ctx.dataDir, ctx.turnId, args.attachment);
              if (!attached) {
                return { ok: false, text: `No encuentro el adjunto "${args.attachment}" en este turno. Solo puedo usar imagenes que el usuario acaba de adjuntar.` };
              }
              resolvedPath = attached;
            } else if (args.path) {
              const roots = await getDesignatedRoots(p, ctx);
              const cage = checkPathCage(args.path, ctx.userText, roots);
              if (!cage.ok || !cage.resolved) {
                return { ok: false, text: `No puedo abrir "${args.path}": ${cage.reason}.` };
              }
              resolvedPath = cage.resolved;
            } else {
              return { ok: false, text: 'Falta "path" o "attachment".' };
            }

            if (args.op === 'skin') {
              if (!args.ref) return { ok: false, text: 'Falta "ref" (el personaje al que ponerle la skin).' };
              // RESCATE K4: UN solo op, con historial real
              // (assistant.skin_apply envuelve new_res + history_set_res +
              // model_tex, igual que action_lib_model_tex, el camino de la UI).
              // El par res.import + object.set_skin que habia aqui aplicaba la
              // skin pero no dejaba NADA que deshacer (medido: 2x edit.undo y
              // el recurso seguia en res.list) -- era lo que suspendia la mitad
              // "1 undo lo revierte" de G4.3.
              ctx.importsN++;
              const res = await p.runOps(
                [{ id: 'im1', op: 'assistant.skin_apply', args: { ref: args.ref, path: toEditorPath(resolvedPath) } }],
                ctx.turnId,
                30000,
                ctx.signal,
              );
              return summarize(res);
            }

            if (args.op === 'asset') {
              const opArgs: Record<string, unknown> = { path: toEditorPath(resolvedPath) };
              if (args.name !== undefined) opArgs.name = args.name;
              if (args.parent !== undefined) opArgs.parent = args.parent;
              ctx.importsN++;
              const res = await p.runOps([{ id: 'im1', op: 'asset.import', args: opArgs }], ctx.turnId, 60000, ctx.signal);
              return summarize(res);
            }

            if (args.op === 'world') {
              const opArgs: Record<string, unknown> = { path: toEditorPath(resolvedPath) };
              if (args.position !== undefined) {
                opArgs.x = args.position[0];
                opArgs.y = args.position[1];
                opArgs.z = args.position[2];
              }
              ctx.importsN++;
              const res = await p.runOps([{ id: 'im1', op: 'world.import', args: opArgs }], ctx.turnId, 150000, ctx.signal);
              return summarize(res);
            }

            // op === 'resource'
            if (!args.resource_type) return { ok: false, text: 'Falta "resource_type" (p.ej. "texture", "font", "sound").' };
            const opArgs: Record<string, unknown> = { path: toEditorPath(resolvedPath), type: args.resource_type };
            ctx.importsN++;
            const res = await p.runOps([{ id: 'im1', op: 'res.import', args: opArgs }], ctx.turnId, 30000, ctx.signal);
            return summarize(res);
          }),
      ),

      // -----------------------------------------------------------------
      // RONDA K5: entregables (mi3_export) y gestion de proyecto
      // (mi3_project_file). Ver SPEC_ASSISTANT_RONDA_K.md seccion 7.7/7.8 y
      // seccion 3 (el guardarrail de dos turnos). Reutiliza la maquinaria de
      // mi3_render_quality (render.ts: assistant.render_snapshot -> instancia
      // aislada -> op-script -> validacion IHDR) -- NUNCA se abre un segundo
      // camino de ejecucion. "out_name" es un NOMBRE, no una ruta: el destino
      // (<project_folder>/exports/) lo decide el anfitrion, y el resultado
      // NUNCA se adjunta como imagen (a diferencia de camera_preview/
      // render_quality) -- la jaula de Read no crece (spec 2.1).
      // -----------------------------------------------------------------

      tool(
        'mi3_export',
        'Render a real DELIVERABLE file -- not a preview -- to disk: a single image, a PNG ' +
          'sequence, an image plus its "white details" line-art layer, or a full set of depth-' +
          'parallax layers. This is slow (reuses the same isolated-instance pipeline as ' +
          'mi3_render_quality) and writes into the current project\'s own "exports" folder -- ' +
          'you get back the path, dimensions and timing in text, but you CANNOT Read the file ' +
          'yourself (it is outside your sandbox on purpose). If you need to see what you exported, ' +
          'take a mi3_camera_preview or mi3_render_quality of the SAME camera/frame afterwards and ' +
          'look at THAT. The project must have been saved at least once (Ctrl+S) or there is nowhere ' +
          'to put the export. Budget: 2 exports per turn.\n\n' +
          'op:"image" renders ONE frame ("frame", default the current marker) to ' +
          '"<out_name>.png". op:"sequence" renders every frame from "from" to "to" (inclusive) every ' +
          '"step" frames (default 1) to "<out_name>_NNNN.png" (4-digit, zero-padded) -- up to 240 ' +
          'frames per call, and a total pixel budget on top of that; split a long animation across ' +
          'several calls if you hit it. op:"white_details" renders one frame AND runs the measured ' +
          'GOLD white-details post-process on it in the same call, producing both ' +
          '"<out_name>.png" and "<out_name>_whitedetails.png" (optional "wd_density" 0-100, ' +
          '"wd_angle" degrees, "wd_width" line width -- omit for the product defaults). ' +
          'op:"layers" splits the SAME frame into depth-based parallax layers for compositing: ' +
          '"<out_name>_full.png" (the complete shot) plus "<out_name>_layer00.png", "_layer01.png", ' +
          '... (nearest first) and "<out_name>_layers.json" (the manifest: per-layer depth range, ' +
          'pixel coverage, near/far). "layers" (2-8) forces a layer count; omit it to let the engine ' +
          'decide from the depth histogram. Depth slicing is Cinema-engine-only (project engine 2, ' +
          'see mi3_project_set op:"set" settings:{engine:2}) -- on engine 0/1 it still succeeds but ' +
          'degrades to a single "_full"+"_layer00" pair (no crash, just nothing to composite). Even ' +
          'on Cinema, the engine can deliver FEWER layers than requested if the scene does not have ' +
          'that much real depth separation (a flat scene stays 1-2 layers no matter what N you ask ' +
          'for) -- read "layers_n"/the manifest back rather than assuming your request was honored ' +
          'exactly.\n\n' +
          'op:"movie" renders a REAL video (H.264 in an MP4 container, the engine\'s own embedded ' +
          'encoder -- no external ffmpeg.exe involved) covering "from".."to" (inclusive, same frame ' +
          'numbers as op:"sequence"), or -- if you omit "from"/"to" -- "duration_s" seconds (default ' +
          '2) starting at the current marker. "fps" (default 24) is the OUTPUT video\'s playback rate ' +
          '-- it is independent of the project\'s own tempo. "format" chooses the container/codec: ' +
          '"mp4" (default) is the real, always-available path. "webm" is NOT available on this build ' +
          '(the embedded FFmpeg has no libvpx/webm muxer compiled in -- true for every install of this ' +
          'engine, not a one-off failure) -- asking for it gets you a PNG sequence instead, and the ' +
          'result text says so plainly; call mi3_report_gap if the user specifically needed webm. ' +
          '"png_sequence" gives you the frames as separate PNGs on purpose (same output as op:"sequence"). ' +
          'width/height must end up even (H.264 4:2:0 chroma) -- odd values get rounded. Budget: on ' +
          'top of the shared 2-exports-per-turn budget, movies are ALSO capped at 1 per turn (they are ' +
          'the slowest thing this tool does: one full render per output frame).\n\n' +
          'Resolution defaults to 1920x1080 if you omit width/height -- pass them explicitly for ' +
          'anything else. "transparent" only applies to op:"image"/"sequence" (alpha background); ' +
          'layers/white_details/movie always keep their engine-decided alpha (movie has none: video ' +
          'has no transparency here). "camera" defaults to the scene\'s active camera.',
        {
          op: z.enum(['image', 'sequence', 'white_details', 'layers', 'movie']),
          camera: z.string().optional(),
          frame: z.number().int().min(0).optional(),
          from: z.number().int().min(0).optional(),
          to: z.number().int().min(0).optional(),
          step: z.number().int().min(1).default(1).optional(),
          width: z.number().int().optional(),
          height: z.number().int().optional(),
          samples: z.number().int().optional(),
          transparent: z.boolean().default(false).optional(),
          out_name: z.string().max(80).optional(),
          layers: z.number().int().min(2).max(8).optional(),
          wd_density: z.number().min(0).max(100).optional(),
          wd_angle: z.number().optional(),
          wd_width: z.number().min(1).optional(),
          format: z.enum(['mp4', 'webm', 'png_sequence']).optional(),
          fps: z.number().int().min(1).max(60).optional(),
          duration_s: z.number().min(0.1).max(30).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_export', `export ${args.op}`, async () => {
            if (ctx.exportsN >= EXPORTS_CAP_PER_TURN) {
              return {
                ok: false,
                text: `Ya has exportado ${ctx.exportsN} vez/veces en este turno; ese es el tope (${EXPORTS_CAP_PER_TURN}). Pidele al usuario que te lo pida en otro turno.`,
              };
            }
            // RONDA K6 (spec 2.4, CAPS.moviePerTurn): tope APARTE del de
            // exportsN, mas restrictivo -- un video es un render por cada
            // frame de salida, lo mas caro que hace esta tool. Se cuenta
            // igual sea cual sea "format" (incluso si degrada a PNG, el
            // trabajo de render ya se hizo).
            if (args.op === 'movie' && ctx.movieN >= MOVIE_CAP_PER_TURN) {
              return {
                ok: false,
                text: `Ya has exportado ${ctx.movieN} video(s) en este turno; ese es el tope (${MOVIE_CAP_PER_TURN}). Pidele al usuario que te lo pida en otro turno.`,
              };
            }

            const roots = await getDesignatedRoots(p, ctx);
            if (!roots.project) {
              return {
                ok: false,
                text: 'No puedo exportar: el proyecto nunca se ha guardado. Pidele al usuario que pulse Ctrl+S y vuelve a intentarlo.',
              };
            }
            const exportsDir = roots.exports;
            try {
              fs.mkdirSync(exportsDir, { recursive: true });
            } catch (e) {
              return { ok: false, text: `No se pudo crear la carpeta de exportacion (${exportsDir}): ${String(e)}` };
            }

            const baseName = sanitizeFileStem(args.out_name);
            const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, Math.round(v)));
            const width = clamp(args.width ?? 1920, EXPORT_LIMITS.minW, EXPORT_LIMITS.maxW);
            const height = clamp(args.height ?? 1080, EXPORT_LIMITS.minH, EXPORT_LIMITS.maxH);
            const spp = clamp(args.samples ?? 64, EXPORT_LIMITS.minSpp, EXPORT_LIMITS.maxSpp);
            const transparent = args.transparent ?? false;

            let plan: ExportPlan;
            let degradedNote = '';
            if (args.op === 'sequence') {
              if (args.from === undefined || args.to === undefined) {
                return { ok: false, text: 'op:"sequence" necesita "from" y "to" (numeros de frame).' };
              }
              if (args.to < args.from) return { ok: false, text: '"to" tiene que ser >= "from".' };
              const step = args.step ?? 1;
              const frameCount = Math.floor((args.to - args.from) / step) + 1;
              if (frameCount > EXPORT_LIMITS.maxSequenceFrames) {
                return {
                  ok: false,
                  text: `La secuencia pedida tiene ${frameCount} frames; el tope por llamada es ${EXPORT_LIMITS.maxSequenceFrames}. Baja el rango, sube "step", o pidela en varias llamadas.`,
                };
              }
              const totalBudget = frameCount * width * height * spp;
              if (totalBudget > EXPORT_LIMITS.maxSequenceBudget) {
                return {
                  ok: false,
                  text: `Demasiado trabajo: ${frameCount} frames a ${width}x${height} y ${spp} muestras. Baja resolucion, muestras, o el numero de frames.`,
                };
              }
              plan = {
                kind: 'sequence',
                width,
                height,
                spp,
                denoise: true,
                transparent,
                camera: args.camera,
                from: args.from,
                to: args.to,
                step,
                outDir: exportsDir,
                baseName,
              };
            } else if (args.op === 'movie') {
              const format = args.format ?? 'mp4';
              const fps = clamp(args.fps ?? 24, EXPORT_LIMITS.minFps, EXPORT_LIMITS.maxFps);

              // RONDA K6 (spec 2.5, "degradacion elegante ... documentada, no
              // silenciosa"): webm NO es un fallo transitorio en esta maquina --
              // el FFmpeg embebido de este motor esta compilado SIN libvpx/sin
              // muxer webm (ver SPEC_VIDEO_WEB.md SS1.3, grep verificado sobre
              // libavformat.a/libavcodec.a). Nunca va a funcionar en NINGUNA
              // instalacion de este build, asi que se degrada SIEMPRE, no se
              // intenta primero y se cae despues -- y se DICE, en el propio
              // texto de vuelta, para que nunca se lea como un exito silencioso.
              const wantsSequence = format === 'webm' || format === 'png_sequence';
              if (wantsSequence) {
                if (format === 'webm') {
                  degradedNote =
                    'AVISO: pediste "webm" pero este motor no puede producirlo -- su FFmpeg embebido no trae el ' +
                    'muxer/codec de WebM compilado (nunca lo tiene, en ninguna instalacion de este build; no es un ' +
                    'fallo pasajero). Te entrego una secuencia PNG en su lugar.';
                }
                let from = args.from;
                let to = args.to;
                if (from === undefined || to === undefined) {
                  const durS = args.duration_s ?? 2;
                  from = from ?? 0;
                  to = to ?? from + Math.max(1, Math.round(durS * fps)) - 1;
                }
                if (to < from) return { ok: false, text: '"to" tiene que ser >= "from".' };
                const step = args.step ?? 1;
                const frameCount = Math.floor((to - from) / step) + 1;
                if (frameCount > EXPORT_LIMITS.maxSequenceFrames) {
                  return {
                    ok: false,
                    text: `El video pedido tiene ${frameCount} frames; el tope por llamada es ${EXPORT_LIMITS.maxSequenceFrames}. Baja "duration_s"/el rango, sube "fps"/"step", o pidelo en varias llamadas.`,
                  };
                }
                const totalBudget = frameCount * width * height * spp;
                if (totalBudget > EXPORT_LIMITS.maxSequenceBudget) {
                  return {
                    ok: false,
                    text: `Demasiado trabajo: ${frameCount} frames a ${width}x${height} y ${spp} muestras. Baja resolucion, muestras, o la duracion.`,
                  };
                }
                plan = {
                  kind: 'sequence',
                  width,
                  height,
                  spp,
                  denoise: true,
                  transparent,
                  camera: args.camera,
                  from,
                  to,
                  step,
                  outDir: exportsDir,
                  baseName,
                };
              } else {
                // format 'mp4' -- el video real. "from"/"to" pueden quedar sin
                // resolver aqui (dependen de man.marker, que se conoce DESPUES
                // del snapshot): resolveMovieRange() en render.ts hace el mismo
                // calculo cuando toca. Aqui solo se topa el CASO EXPLICITO
                // (from/to ya dados) -- si depende de duration_s, el tope se
                // comprueba con el "peor caso" declarado (duration_s*fps).
                const step = args.step ?? 1;
                const durS = args.duration_s ?? 2;
                const frameCount =
                  args.from !== undefined && args.to !== undefined
                    ? Math.floor((args.to - args.from) / step) + 1
                    : Math.max(1, Math.round(durS * fps));
                if (args.from !== undefined && args.to !== undefined && args.to < args.from) {
                  return { ok: false, text: '"to" tiene que ser >= "from".' };
                }
                if (frameCount > EXPORT_LIMITS.maxMovieFrames) {
                  return {
                    ok: false,
                    text: `El video pedido tiene ${frameCount} frames; el tope por llamada es ${EXPORT_LIMITS.maxMovieFrames}. Baja "duration_s"/el rango, sube "fps"/"step", o pidelo en varias llamadas.`,
                  };
                }
                const totalBudget = frameCount * width * height * spp;
                if (totalBudget > EXPORT_LIMITS.maxMovieBudget) {
                  return {
                    ok: false,
                    text: `Demasiado trabajo: ${frameCount} frames a ${width}x${height} y ${spp} muestras. Baja resolucion, muestras, o la duracion.`,
                  };
                }
                // H.264 4:2:0 exige dimensiones pares -- se redondean aqui, no
                // en el op de GML (que solo valida y falla claro si algo se
                // le cuela impar).
                const evenW = width % 2 === 0 ? width : width - 1;
                const evenH = height % 2 === 0 ? height : height - 1;
                plan = {
                  kind: 'movie',
                  width: evenW,
                  height: evenH,
                  spp,
                  denoise: true,
                  transparent: false,
                  camera: args.camera,
                  from: args.from,
                  to: args.to,
                  step,
                  outDir: exportsDir,
                  baseName,
                  fps,
                  durationS: durS,
                };
              }
            } else {
              const budget = width * height * spp;
              if (budget > EXPORT_LIMITS.maxBudget) {
                return {
                  ok: false,
                  text: `Demasiado trabajo: ${width}x${height} a ${spp} muestras. Baja resolucion o muestras (por ejemplo 1920x1080 a 32 muestras) y vuelve a llamar.`,
                };
              }
              if (args.op === 'layers') {
                plan = {
                  kind: 'layers',
                  width,
                  height,
                  spp,
                  denoise: true,
                  transparent: false,
                  camera: args.camera,
                  frame: args.frame,
                  outDir: exportsDir,
                  baseName,
                  layers: args.layers,
                };
              } else {
                plan = {
                  kind: args.op,
                  width,
                  height,
                  spp,
                  denoise: true,
                  transparent,
                  camera: args.camera,
                  frame: args.frame,
                  outDir: exportsDir,
                  baseName,
                  wdDensity: args.wd_density,
                  wdAngle: args.wd_angle,
                  wdWidth: args.wd_width,
                };
              }
            }

            if (args.op === 'movie') ctx.movieN++;
            const n = ++ctx.exportsN;
            const safeTurn = ctx.turnId.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 40);
            const jobId = `${safeTurn}_e${n}`;

            p.emit({ type: 'status', state: 'acting', turn_id: ctx.turnId });
            const onPhase = (phase: 'snapshot' | 'instance' | 'copy' | 'render' | 'done' | 'failed', detail?: string, ms?: number) =>
              p.emit({ type: 'job', turn_id: ctx.turnId, job_id: jobId, phase, detail, ms });

            onPhase('snapshot');
            const res = await p.runOps(
              [{ id: 'esn1', op: 'assistant.render_snapshot', args: { job_id: jobId, camera: args.camera } }],
              ctx.turnId,
              30000,
              ctx.signal,
            );
            const { ok, text } = summarize(res);
            if (!ok) {
              onPhase('failed', 'snapshot');
              const r0 = res.results?.[0] as OpResultEntry | undefined;
              if (String(r0?.error ?? '').includes('nofolder')) {
                return {
                  ok: false,
                  text:
                    'No puedo exportar porque el proyecto nunca se ha guardado. Pidele al usuario que pulse Ctrl+S ' +
                    'y vuelve a intentarlo.\n\n' + text,
                };
              }
              return { ok: false, text };
            }

            const r0 = res.results?.[0] as OpResultEntry as unknown as RenderManifest;
            const man: RenderManifest = {
              job_id: String(r0.job_id),
              job_dir: String(r0.job_dir),
              scene: String(r0.scene),
              project_folder: String(r0.project_folder),
              project_file: String(r0.project_file),
              project_name: String(r0.project_name),
              install_dir: String(r0.install_dir),
              marker: Number(r0.marker),
              engine: Number(r0.engine),
              camera: String(r0.camera ?? ''),
            };

            try {
              const out = await runExportJob(p, man, plan, onPhase, ctx.signal);
              p.emit({ type: 'status', state: 'thinking', turn_id: ctx.turnId });

              const lines: string[] = [];
              if (degradedNote) lines.push(degradedNote);
              if (plan.kind === 'image') {
                const f = out.files[0];
                lines.push(`Imagen exportada: ${f.path}`);
                lines.push(`${f.w}x${f.h}${transparent ? ', con transparencia' : ''}.`);
              } else if (plan.kind === 'sequence') {
                lines.push(`Secuencia exportada: ${out.files.length} frames en ${exportsDir}`);
                lines.push(`Nombres: ${baseName}_NNNN.png (frames ${plan.from}..${plan.to}, paso ${plan.step ?? 1}).`);
                lines.push(`${width}x${height} cada uno.`);
              } else if (plan.kind === 'white_details') {
                lines.push(`Imagen exportada: ${out.files[0]?.path ?? '(?)'}`);
                lines.push(`Detalles blancos: ${out.files[1]?.path ?? '(no generado)'}`);
              } else if (plan.kind === 'movie') {
                const f = out.files[0];
                lines.push(`Video exportado: ${f.path}`);
                lines.push(
                  `${f.w}x${f.h}, ${String(out.info.fps ?? plan.fps ?? '?')} fps, ${String(out.info.frames ?? '?')} frames ` +
                    `(~${(Number(out.info.duration_s ?? 0)).toFixed(1)} s), ${String(out.info.bytes ?? '?')} bytes. Formato: MP4 (H.264).`,
                );
                lines.push(`Videos usados este turno: ${ctx.movieN}/${MOVIE_CAP_PER_TURN}.`);
              } else {
                lines.push(`Capas exportadas en ${exportsDir}, base "${baseName}":`);
                lines.push(`${out.files.length} PNG en total (1 completa "_full" + ${out.info.layers ?? '?'} capa(s)).`);
                if (out.info.manifest) lines.push(`Manifiesto: ${String(out.info.manifest)}`);
              }
              lines.push(`Tiempo: ${Math.round(out.ms / 1000)} s. Exportaciones usadas este turno: ${n}/${EXPORTS_CAP_PER_TURN}.`);
              lines.push(
                'No puedes leer estos ficheros (estan fuera de tu jaula). Si quieres verificar el resultado, pide ' +
                  'un mi3_camera_preview o mi3_render_quality de la misma camara y describe SOLO lo que veas ahi.',
              );

              return { ok: true, text: lines.join('\n') };
            } catch (e) {
              p.emit({ type: 'status', state: 'thinking', turn_id: ctx.turnId });
              if (e instanceof RenderAborted) return { ok: false, text: 'Export cancelado por el usuario.' };
              if (e instanceof RenderError) return { ok: false, text: `El export fallo: ${e.message}` };
              throw e;
            }
          }),
      ),

      tool(
        'mi3_project_file',
        'Save, open, create, restore or list PROJECT FILES -- the operations that can DISCARD the ' +
          'user\'s unsaved work. op:"save" writes over the current file (safe, never asks -- if the ' +
          'project has never been saved there is nowhere to write and it fails plainly). op:"projects" ' +
          'lists available .miproject files by name (no path argument, capped at 200; it sees both a ' +
          'loose <name>.miproject and the usual <name>/<name>.miproject folder) so you ' +
          'never have to guess a name. op:"open" (by "name" or a literal "path" from the user\'s ' +
          'message), op:"new" (optional "name"), op:"checkpoint_restore" ("name" = checkpoint id or ' +
          'name, see mi3_checkpoint action:"list"), and op:"save_as" ("name" or "path" for the ' +
          'destination) ALL go through a two-turn confirmation guardrail whenever they would actually ' +
          'discard something (an unsaved change for open/new/checkpoint_restore, an existing ' +
          '.miproject already at the destination for save_as): the FIRST call tells you what is at ' +
          'stake and returns isError WITHOUT touching anything. At that point you must ask the user ' +
          'in chat and END YOUR TURN -- do not retry with confirm in the same turn, it will be ' +
          'rejected outright (the confirmation has to come from the user, in a later turn, not from ' +
          'you). Once they answer, in a NEW turn, call this again with the exact same op/name and ' +
          'confirm:"save_first" (saves the current project first, then proceeds) or confirm:"discard" ' +
          '(proceeds without saving). If the project has no unsaved changes, the operation just runs ' +
          '-- no question asked. Budget: 1 open/new/restore/save-as per turn (save does not count).',
        {
          op: z.enum(['save', 'save_as', 'open', 'new', 'checkpoint_restore', 'projects']),
          name: z.string().max(120).optional(),
          path: z.string().optional(),
          confirm: z.enum(['save_first', 'discard']).optional(),
        },
        async (args) =>
          runTool(p, ctx, 'mi3_project_file', `project_file: ${args.op}`, async () => {
            if (args.op === 'projects') {
              // RONDA L (H3b): idem mi3_import op:"library" kind:"projects" --
              // el listado lo hace el anfitrion, no el op plano del motor.
              const roots = await getDesignatedRoots(p, ctx);
              if (!roots.projects) return { ok: false, text: 'esa raiz no esta disponible ahora mismo (projects)' };
              const listing = listProjectsHost(roots.projects);
              return formatProjectsListing('assistant.library', listing);
            }

            if (args.op === 'save') {
              const res = await p.runOps([{ id: 'pfs1', op: 'project.save', args: {} }], ctx.turnId, 30000, ctx.signal);
              return summarize(res);
            }

            // El tope se comprueba SIEMPRE aqui -- tambien cuando esta llamada trae
            // "confirm" -- para que un turno que ya gasto su cupo con otra operacion
            // de fichero no pueda colar una segunda via confirmacion.
            if (ctx.projectFileOpsN >= PROJECT_FILE_OPS_CAP_PER_TURN) {
              return {
                ok: false,
                text:
                  `Ya has hecho ${ctx.projectFileOpsN} operacion(es) de abrir/crear/restaurar/guardar-como en este ` +
                  `turno; ese es el tope (${PROJECT_FILE_OPS_CAP_PER_TURN}). Pidelo en otro turno.`,
              };
            }

            if (args.op === 'open') {
              const nameOrPath = args.path ?? args.name;
              if (!nameOrPath) return { ok: false, text: 'Falta "name" o "path" (que proyecto abrir).' };
              const resolved = await resolveProjectRef(p, ctx, nameOrPath);
              if (!resolved.ok) return { ok: false, text: resolved.text };
              return guardedProjectOp(
                p,
                ctx,
                'open',
                resolved.path,
                'Abrir',
                args.confirm,
                async () => {
                  const d = await checkProjectDirty(p, ctx);
                  return {
                    need: d.dirty,
                    detail: `El proyecto abierto tiene cambios sin guardar${d.objects !== undefined ? ` (${d.objects} objetos)` : ''}.`,
                  };
                },
                async () => {
                  const r = await p.runOps(
                    [{ id: 'pfo1', op: 'project.open', args: { path: toEditorPath(resolved.path) } }],
                    ctx.turnId,
                    60000,
                    ctx.signal,
                  );
                  return summarize(r);
                },
              );
            }

            if (args.op === 'new') {
              const target = args.name ?? '(sin nombre)';
              return guardedProjectOp(
                p,
                ctx,
                'new',
                target,
                'Crear un proyecto nuevo',
                args.confirm,
                async () => {
                  const d = await checkProjectDirty(p, ctx);
                  return {
                    need: d.dirty,
                    detail: `El proyecto abierto tiene cambios sin guardar${d.objects !== undefined ? ` (${d.objects} objetos)` : ''}.`,
                  };
                },
                async () => {
                  const opArgs: Record<string, unknown> = {};
                  if (args.name !== undefined) opArgs.name = args.name;
                  const r = await p.runOps([{ id: 'pfn1', op: 'project.new', args: opArgs }], ctx.turnId, 30000, ctx.signal);
                  return summarize(r);
                },
              );
            }

            if (args.op === 'checkpoint_restore') {
              if (!args.name) return { ok: false, text: 'Falta "name" (id o nombre del checkpoint; usa mi3_checkpoint action:"list").' };
              const target = args.name;
              return guardedProjectOp(
                p,
                ctx,
                'checkpoint_restore',
                target,
                'Restaurar el checkpoint',
                args.confirm,
                async () => {
                  const d = await checkProjectDirty(p, ctx);
                  return {
                    need: d.dirty,
                    detail: `El estado actual del proyecto tiene cambios sin guardar${d.objects !== undefined ? ` (${d.objects} objetos)` : ''}.`,
                  };
                },
                async () => {
                  const r = await p.runOps(
                    [{ id: 'pfr1', op: 'project.checkpoint_restore', args: { ref: args.name } }],
                    ctx.turnId,
                    30000,
                    ctx.signal,
                  );
                  return summarize(r);
                },
              );
            }

            // save_as
            const destNameOrPath = args.path ?? args.name;
            if (!destNameOrPath) return { ok: false, text: 'Falta "name" o "path" (donde guardar como).' };
            const dest = await resolveSaveAsDest(p, ctx, destNameOrPath);
            if (!dest.ok) return { ok: false, text: dest.text };
            return guardedProjectOp(
              p,
              ctx,
              'save_as',
              dest.path,
              'Guardar como (sobreescribiendo)',
              args.confirm,
              async () => {
                let exists = false;
                try {
                  exists = fs.existsSync(dest.path);
                } catch {
                  exists = false;
                }
                return { need: exists, detail: `Ya existe un proyecto guardado en "${dest.path}".` };
              },
              async () => {
                const r = await p.runOps(
                  [{ id: 'pfsa1', op: 'project.save', args: { path: toEditorPath(dest.path) } }],
                  ctx.turnId,
                  30000,
                  ctx.signal,
                );
                const { ok: saveOk, text: saveText } = summarize(r);
                const r0 = r.results?.[0] as OpResultEntry | undefined;
                const leftBehind = truthy(r0?.assets_left_behind);
                return {
                  ok: saveOk,
                  text:
                    saveText +
                    (leftBehind
                      ? '\n\nAVISO: los assets del proyecto (texturas/skins importadas) se han quedado en la carpeta ' +
                        'ANTERIOR -- no viajan solos a la carpeta nueva.'
                      : ''),
                };
              },
            );
          }),
      ),
    ],
  });
}
