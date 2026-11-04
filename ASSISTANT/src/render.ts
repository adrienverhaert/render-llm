// ASSISTANT/src/render.ts
//
// Orquestacion del render de calidad fuera del proceso del editor.
// Este modulo es codigo de la APP, no superficie del modelo: el modelo solo ve
// la tool mi3_render_quality. Aqui se hace, en orden:
//
//   1. clonar (una vez por sesion) la instancia del editor a render_inst/
//   2. copiar la carpeta del proyecto al job y volcar el snapshot en su raiz
//   3. escribir el op-script y correrlo con tools/run_render_inst.ps1
//   4. validar el PNG (IHDR) y devolver su ruta absoluta
//
// Cancelacion: matar el powershell hijo (taskkill /T) Y los procesos cuyo Path
// sea el exe de render_inst. Jamas por nombre de proceso.

import { spawn, spawnSync, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Protocol } from './protocol.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** Nombre del ejecutable dentro de la instancia clonada. */
export const RENDER_EXE_NAME = 'render-llm.exe';

/** Limites duros del render de calidad (la tool los aplica antes de llamar). */
export const LIMITS = {
  minW: 320, maxW: 1920,
  minH: 180, maxH: 1080,
  minSpp: 1, maxSpp: 256,
  /** w*h*spp maximo aceptado. 1280x720x128 = 117.9M queda dentro. */
  maxBudget: 130_000_000,
  /** Renders de calidad por turno: blando (pide confirmacion) y absoluto. */
  softPerTurn: 3,
  hardPerTurn: 6,
};

/**
 * Timeout del render. La constante de la derecha (kilo-muestras-pixel -> ms) es
 * una ESTIMACION: el gate B1 mide el ms real y, si el ritmo medido se desvia
 * mas de 2x, hay que reajustarla y dejar el numero literal en el reporte.
 *
 * RECALIBRADA en el gate B1 (escena de prueba: Cubo/Camara/Luz, engine
 * Cinema, GPU sin contienda en el momento de medir): 960x540 a 64 spp
 * (33192 kilo-muestras contando el warmup 160x90x1) tardo ms_render=15084ms
 * de verdad -- la formula original (constante 6) predecia
 * renderTimeoutMs()/3 = 86355ms, mas de 5x el valor medido. Bajar a 2 deja
 * margen (~4.4x el ritmo medido) para escenas mas pesadas y para la GPU
 * COMPARTIDA con las flotas hermanas (R7: renders lentos son normales), sin
 * volver a la sobreestimacion original. Con el clamp 180s-600s intacto, un
 * render de 1280x720 a 128 spp pasa de estar SIEMPRE topado a 600s (con
 * constante 6) a un timeout real de ~296s (con constante 2) que deja
 * margen real antes de disparar.
 */
export function renderTimeoutMs(w: number, h: number, spp: number): number {
  const kilo = (w * h * spp) / 1000;
  return Math.max(180_000, Math.min(600_000, Math.round(60_000 + kilo * 2)));
}

export interface RenderManifest {
  job_id: string;
  job_dir: string;        // relativo a Data/assistant
  scene: string;          // relativo a Data/assistant
  project_folder: string;
  project_file: string;
  project_name: string;
  install_dir: string;
  marker: number;
  engine: number;
  camera: string;
}

export interface RenderRequest {
  width: number;
  height: number;
  spp: number;
  denoise: boolean;
  camera?: string;
}

export type JobPhase = 'snapshot' | 'instance' | 'copy' | 'render' | 'done' | 'failed';
export type OnPhase = (phase: JobPhase, detail?: string, ms?: number) => void;

export class RenderError extends Error {
  constructor(message: string) { super(message); this.name = 'RenderError'; }
}
export class RenderAborted extends Error {
  constructor(message: string) { super(message); this.name = 'RenderAborted'; }
}

/** Reject fallback artifacts when the retained renderer reports that it never completed. */
export function requireCompletedRenderResult(result: Record<string, unknown>, label: string): string {
  const cinemaError = String(result.cinema_error ?? '').trim();
  if (cinemaError) throw new RenderError(`${label} Flame failed: ${cinemaError}`);
  const status = String(result.render_status ?? '').trim();
  if (!status || !/(?:^| )state=finished(?: |$)/.test(status) || !/(?:^| )error=none(?: |$)/.test(status)) {
    throw new RenderError(`${label} did not complete in Flame: ${status || 'missing renderer status'}`);
  }
  return status;
}

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

const PNG_SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Lee w/h de la cabecera IHDR. null si no es un PNG legible. */
export function pngDims(file: string): { w: number; h: number } | null {
  let fd: number | undefined;
  try {
    fd = fs.openSync(file, 'r');
    const buf = Buffer.alloc(24);
    const n = fs.readSync(fd, buf, 0, 24, 0);
    if (n < 24) return null;
    if (!buf.subarray(0, 8).equals(PNG_SIG)) return null;
    if (buf.subarray(12, 16).toString('latin1') !== 'IHDR') return null;
    return { w: buf.readUInt32BE(16), h: buf.readUInt32BE(20) };
  } catch {
    return null;
  } finally {
    if (fd !== undefined) { try { fs.closeSync(fd); } catch { /* noop */ } }
  }
}

/**
 * RONDA K6: comprobacion de cordura de un MP4 recien exportado -- no hay un
 * "IHDR" tan simple como el de PNG, pero un contenedor ISO-BMFF (mp4/mov)
 * siempre empieza con una caja "ftyp": 4 bytes de tamano + 4 bytes de tipo
 * literal "ftyp" en el offset 4. Si eso no esta, el fichero no es un MP4
 * valido (proceso matado a mitad de escritura, disco lleno, etc.) -- igual
 * que pngDims(), esto es la ULTIMA linea de defensa del host: nunca se confia
 * en el codigo de salida del proceso ni en el tamano del fichero a solas.
 */
export function looksLikeMp4(file: string): boolean {
  let fd: number | undefined;
  try {
    fd = fs.openSync(file, 'r');
    const buf = Buffer.alloc(12);
    const n = fs.readSync(fd, buf, 0, 12, 0);
    if (n < 8) return false;
    return buf.subarray(4, 8).toString('latin1') === 'ftyp';
  } catch {
    return false;
  } finally {
    if (fd !== undefined) { try { fs.closeSync(fd); } catch { /* noop */ } }
  }
}

function psQuote(s: string): string {
  return "'" + s.replace(/'/g, "''") + "'";
}

/** robocopy sincrono. Codigos < 8 son exito (0 = nada que copiar, 1 = copiado). */
function robocopy(src: string, dst: string, extra: string[]): number {
  const r = spawnSync('robocopy', [src, dst, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1', '/MT:8', ...extra], {
    windowsHide: true,
    encoding: 'utf8',
  });
  const code = typeof r.status === 'number' ? r.status : 16;
  if (code >= 8) {
    throw new RenderError(`robocopy fallo (${code}) copiando ${src} -> ${dst}: ${(r.stderr || r.stdout || '').slice(0, 400)}`);
  }
  return code;
}

function rmQuiet(p: string): void {
  try { fs.rmSync(p, { recursive: true, force: true }); } catch { /* noop */ }
}

/** Mata todo proceso cuyo Path sea EXACTAMENTE `exe`. Nunca por nombre. */
export function killByExePath(exe: string): void {
  spawnSync(
    'powershell',
    ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command',
      `Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -eq ${psQuote(exe)}) } | Stop-Process -Force -ErrorAction SilentlyContinue`],
    { windowsHide: true },
  );
}

function killTree(pid: number): void {
  spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true });
}

// ---------------------------------------------------------------------------
// 1. Instancia dedicada
// ---------------------------------------------------------------------------

/** Stage immutable runtime resources only; editor state can change during a render. */
export function ensureRenderInstance(p: Protocol, installDir: string, onPhase: OnPhase): string {
  const renderInst = path.resolve(p.dir, 'render_inst');
  const exe = path.join(renderInst, RENDER_EXE_NAME);
  const marker = path.join(renderInst, '.renderllm_render_inst.json');
  if (path.dirname(renderInst) !== path.resolve(p.dir) || path.resolve(installDir) === renderInst) {
    throw new RenderError('Invalid render instance location');
  }
  onPhase('instance', 'preparing native runtime');
  let runtimeDlls: string[];
  const srcExe = path.join(installDir, RENDER_EXE_NAME);
  let srcStamp: string;
  try {
    const st = fs.statSync(srcExe);
    if (!st.isFile() || st.size === 0) throw new Error('Missing executable');
    runtimeDlls = fs.readdirSync(installDir, { withFileTypes: true })
      .filter((entry) => entry.isFile() && /\.dll$/i.test(entry.name)).map((entry) => entry.name).sort();
    srcStamp = [RENDER_EXE_NAME, ...runtimeDlls].map((name) => {
      const info = fs.statSync(path.join(installDir, name));
      return `${name}:${info.size}:${info.mtimeMs}`;
    }).join('|');
  } catch {
    throw new RenderError(`Native renderer is unavailable: ${srcExe}`);
  }
  if (fs.existsSync(exe) && fs.existsSync(marker)) {
    try {
      const saved = JSON.parse(fs.readFileSync(marker, 'utf8'));
      if (saved.layout === 3 && saved.runtime_stamp === srcStamp) {
        p.log(`render_inst reused: ${renderInst}`);
        return renderInst;
      }
    } catch { /* An incomplete or old staging marker requires a fresh copy. */ }
  }
  const t0 = Date.now();
  fs.rmSync(renderInst, { recursive: true, force: true });
  fs.mkdirSync(renderInst, { recursive: true });
  const copy = (relative: string): void => {
    const source = path.join(installDir, relative);
    if (!fs.existsSync(source)) return;
    fs.cpSync(source, path.join(renderInst, relative), {
      recursive: true,
      filter: (entry) => {
        if (fs.lstatSync(entry).isSymbolicLink()) throw new RenderError('Runtime resources must not contain symbolic links');
        return true;
      },
    });
  };
  copy(RENDER_EXE_NAME);
  for (const name of runtimeDlls) copy(name);
  for (const folder of ['platforms', 'imageformats', 'styles']) copy(folder);
  // Retained legacy resources are optional; user scenes and mutable Data are never copied.
  for (const name of ['Fonts', 'Languages', 'Libraries', 'Render', 'Canon', 'Minecraft', 'ShinyTrims', 'Splashes', 'languages.midata', 'legacy.midata']) {
    copy(path.join('Data', name));
  }
  fs.writeFileSync(marker, JSON.stringify({ layout: 3, created: Date.now(), source: installDir, exe, runtime_stamp: srcStamp }), 'utf8');
  const ms = Date.now() - t0;
  p.log(`render_inst staged in ${ms} ms from ${installDir}`);
  onPhase('instance', 'runtime ready', ms);
  return renderInst;
}

// ---------------------------------------------------------------------------
// 2. Carpeta del job
// ---------------------------------------------------------------------------

/**
 * Copia la carpeta VIVA del proyecto al job y deja el snapshot del editor como
 * .miproject de la RAIZ de esa copia. Ese ultimo paso es lo que hace que los
 * assets resuelvan: project_load fija project_folder = filename_dir(fichero) y
 * los recursos se buscan ahi (mismo razonamiento que mi3_checkpoint_restore).
 * Se excluyen checkpoints/ y los .backup: no aportan nada al render y pueden
 * multiplicar el tamano de la copia.
 */
export function prepareJobFolder(p: Protocol, man: RenderManifest, onPhase: OnPhase): { projDir: string; sceneInProj: string } {
  const t0 = Date.now();
  onPhase('copy', 'copiando la carpeta del proyecto');

  const jobDirAbs = path.join(p.dir, man.job_dir);
  const projDir = path.join(jobDirAbs, 'proj');
  const sceneAbs = path.join(p.dir, man.scene);

  if (!fs.existsSync(sceneAbs)) {
    throw new RenderError(`el editor no dejo el snapshot: ${sceneAbs}`);
  }

  fs.mkdirSync(projDir, { recursive: true });
  robocopy(man.project_folder, projDir, [
    '/XD', path.join(man.project_folder, 'checkpoints'),
    // RONDA B (verificacion): MI3 numera los backups (<nombre>.backup1 ..
    // .backup5), asi que el patron '*.backup' a secas NO excluia ninguno y la
    // copia se llevaba 5 proyectos enteros de mas en cada job (R5).
    '/XF', '*.backup', '*.backup?', '*.backup??', '*.miproject.rescue',
  ]);

  const sceneInProj = path.join(projDir, man.project_name);
  fs.copyFileSync(sceneAbs, sceneInProj);

  const ms = Date.now() - t0;
  p.log(`job ${man.job_id}: carpeta copiada en ${ms} ms -> ${projDir}`);
  onPhase('copy', 'copia lista', ms);
  return { projDir, sceneInProj };
}

// ---------------------------------------------------------------------------
// 3. Op-script + corrida headless
// ---------------------------------------------------------------------------

export function buildOpScript(man: RenderManifest, req: RenderRequest, sceneInProj: string, jobDirAbs: string): unknown {
  const camRef = man.camera ? { ref: man.camera } : {};
  const outPng = path.join(jobDirAbs, 'out.png').replace(/\\/g, '/');
  const warmPng = path.join(jobDirAbs, 'warmup.png').replace(/\\/g, '/');
  return {
    stop_on_error: 1,
    ops: [
      { id: 'open', op: 'project.open', args: { path: sceneInProj.replace(/\\/g, '/') } },
      // RONDA L (H7): project_load re-importa el preset con nombre DESPUES del
      // bloque "render" del proyecto (project_load.gml:93-100), y los presets
      // performance/balanced/extreme traen "engine": 0. Medido: un proyecto con
      // "engine": 1 guardado y render_settings "performance" (lo que trae TODO
      // proyecto nuevo, macros.gml:72) se abre en motor CLASICO. Los ojos del
      // asistente tienen que renderizar con el MISMO motor que ve el usuario.
      { id: 'eng', op: 'project.set', args: { field: 'render_engine', value: man.engine } },
      { id: 'mark', op: 'timeline.set_marker', args: { frame: man.marker } },
      // TRAMPA CONOCIDA: el PRIMER render de una corrida sale con la pose sin
      // asentar. Se quema uno diminuto antes del bueno.
      { id: 'warm', op: 'render.final', args: { ...camRef, width: 160, height: 90, samples: 1, denoise: 0, transparent: 0, out: warmPng } },
      { id: 'mark2', op: 'timeline.set_marker', args: { frame: man.marker } },
      { id: 'shot', op: 'render.final', args: { ...camRef, width: req.width, height: req.height, samples: req.spp, denoise: req.denoise ? 1 : 0, transparent: 0, out: outPng } },
    ],
  };
}

export interface HeadlessOutcome { stdout: string; stderr: string; code: number | null }

export function runHeadless(
  p: Protocol,
  renderInst: string,
  opScriptPath: string,
  pidFile: string,
  timeoutMs: number,
  signal: AbortSignal | undefined,
  onTick: (elapsedMs: number) => void,
): Promise<HeadlessOutcome> {
  const ps1 = path.join(__dirname, '..', 'tools', 'run_render_inst.ps1');
  if (!fs.existsSync(ps1)) throw new RenderError(`falta el lanzador de render: ${ps1}`);
  const exe = path.join(renderInst, RENDER_EXE_NAME);

  return new Promise<HeadlessOutcome>((resolve, reject) => {
    const child: ChildProcess = spawn(
      'powershell',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ps1,
        '-OpScript', opScriptPath, '-InstanceDir', renderInst,
        '-TimeoutSec', String(Math.ceil(timeoutMs / 1000)),
        '-ExeName', RENDER_EXE_NAME, '-PidFile', pidFile],
      { windowsHide: true },
    );

    let stdout = '';
    let stderr = '';
    child.stdout?.on('data', (d) => { stdout += String(d); });
    child.stderr?.on('data', (d) => { stderr += String(d); });

    const t0 = Date.now();
    let settled = false;
    let hardTimer: ReturnType<typeof setTimeout> | undefined;
    const tick = setInterval(() => onTick(Date.now() - t0), 5000);

    const cleanup = () => {
      clearInterval(tick);
      if (hardTimer) clearTimeout(hardTimer);
      signal?.removeEventListener('abort', onAbort);
    };

    function killAll(): void {
      if (child.pid) killTree(child.pid);
      killByExePath(exe);
    }

    function onAbort(): void {
      if (settled) return;
      settled = true;
      p.log(`job cancelado: matando ${exe}`);
      killAll();
      cleanup();
      reject(new RenderAborted('render cancelado por el usuario (STOP)'));
    }

    if (signal?.aborted) { onAbort(); return; }
    signal?.addEventListener('abort', onAbort, { once: true });

    hardTimer = setTimeout(() => {
      if (settled) return;
      settled = true;
      killAll();
      cleanup();
      reject(new RenderError(`el render no termino en ${Math.round(timeoutMs / 1000)} s`));
    }, timeoutMs + 20_000);

    child.on('error', (e) => {
      if (settled) return;
      settled = true;
      clearTimeout(hardTimer);
      cleanup();
      reject(new RenderError(`no se pudo lanzar powershell: ${String(e)}`));
    });

    child.on('close', (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(hardTimer);
      cleanup();
      resolve({ stdout, stderr, code });
    });
  });
}

// ---------------------------------------------------------------------------
// 4. Orquestacion completa
// ---------------------------------------------------------------------------

export interface RenderResult {
  outAbs: string;
  outRel: string;   // relativo a Data/assistant (para el evento preview)
  w: number;
  h: number;
  spp: number;
  ms: number;
  engine: number;
}

export async function runRenderJob(
  p: Protocol,
  man: RenderManifest,
  req: RenderRequest,
  onPhase: OnPhase,
  signal?: AbortSignal,
): Promise<RenderResult> {
  const jobDirAbs = path.join(p.dir, man.job_dir);
  const renderInst = ensureRenderInstance(p, man.install_dir, onPhase);
  const { sceneInProj } = prepareJobFolder(p, man, onPhase);

  const script = buildOpScript(man, req, sceneInProj, jobDirAbs);
  const scriptPath = path.join(jobDirAbs, 'job.json');
  fs.writeFileSync(scriptPath, JSON.stringify(script, null, 1), 'utf8');

  const timeoutMs = renderTimeoutMs(req.width, req.height, req.spp);
  const pidFile = path.join(jobDirAbs, 'render.pid');
  const t0 = Date.now();
  onPhase('render', `${req.width}x${req.height}, ${req.spp} spp${req.denoise ? ', denoise' : ''}`);

  const outcome = await runHeadless(p, renderInst, scriptPath, pidFile, timeoutMs, signal, (elapsed) => {
    onPhase('render', `${Math.round(elapsed / 1000)} s`);
  });
  const ms = Date.now() - t0;

  const outAbs = path.join(jobDirAbs, 'out.png');
  try {
    if (outcome.stdout.trim()) fs.writeFileSync(path.join(jobDirAbs, 'result.json'), outcome.stdout, 'utf8');
  } catch { /* diagnostico, nunca fatal */ }

  if (!fs.existsSync(outAbs)) {
    onPhase('failed', 'sin PNG', ms);
    throw new RenderError(
      `el render no produjo imagen (codigo ${outcome.code}). ${(outcome.stderr || '').slice(0, 300)}`,
    );
  }
  const dims = pngDims(outAbs);
  if (!dims) {
    onPhase('failed', 'PNG ilegible', ms);
    throw new RenderError('el PNG generado no tiene una cabecera valida');
  }
  if (dims.w !== req.width || dims.h !== req.height) {
    // Familia de bug conocida (guard T4 del export por capas): un frame sale a
    // resolucion de preview. Se reporta, no se silencia.
    onPhase('failed', `dimensiones ${dims.w}x${dims.h}`, ms);
    throw new RenderError(`el PNG salio a ${dims.w}x${dims.h} y se pidio ${req.width}x${req.height}`);
  }

  onPhase('done', `${dims.w}x${dims.h}`, ms);
  pruneJobs(p, 2);

  return {
    outAbs,
    outRel: `${man.job_dir}/out.png`,
    w: dims.w,
    h: dims.h,
    spp: req.spp,
    ms,
    engine: man.engine,
  };
}

// ---------------------------------------------------------------------------
// RONDA K5 (spec seccion 7.7): export de entregables (imagen/secuencia/
// white_details/capas). Reutiliza EXACTAMENTE la misma maquinaria que el
// render de calidad -- ensureRenderInstance + prepareJobFolder + runHeadless
// -- con la diferencia de DONDE cae el resultado: no en
// render_job/<job>/out.png (la unica carpeta que el modelo puede Read, ver
// denyByDefault en index.ts) sino en <project_folder>/exports/, una ruta que
// el ANFITRION (tools.ts, via paths.ts) resuelve y crea -- el modelo nunca ve
// ni construye esa ruta, solo un "out_name" que el anfitrion sanea antes de
// usarlo (spec 2.1: "el modelo NO puede leer ese fichero").
// ---------------------------------------------------------------------------

export type ExportKind = 'image' | 'sequence' | 'white_details' | 'layers' | 'movie';

export interface ExportPlan {
  kind: ExportKind;
  width: number;
  height: number;
  spp: number;
  denoise: boolean;
  transparent: boolean;
  camera?: string;
  /** image / white_details: frame explicito; por defecto el marker del snapshot. */
  frame?: number;
  /** sequence / movie */
  from?: number;
  to?: number;
  step?: number;
  /** Carpeta ABSOLUTA de salida -- el llamador ya la creo (fs.mkdirSync recursive). */
  outDir: string;
  /** Nombre base YA SANEADO por el anfitrion (sin separadores de ruta, sin ".."). */
  baseName: string;
  /** layers: numero de capas pedido, 0/undefined = automatico (histograma de profundidad). */
  layers?: number;
  /** white_details: parametros opcionales del motor GOLD (0-100 en density, grados en angle). */
  wdDensity?: number;
  wdAngle?: number;
  wdWidth?: number;
  /** movie: fotogramas por segundo del VIDEO de salida (no confundir con project_tempo). */
  fps?: number;
  /** movie: si "from"/"to" no vienen, cuantos segundos exportar desde el marker actual. */
  durationS?: number;
}

export interface ExportedFile {
  path: string;
  w: number;
  h: number;
}

export interface ExportOutcome {
  files: ExportedFile[];
  ms: number;
  info: Record<string, unknown>;
}

/** Limites duros de mi3_export (la tool los aplica antes de llamar aqui). Mas
 *  holgados que LIMITS (render de calidad, una vista previa que se descarta):
 *  esto es un ENTREGABLE real, hasta 4K. */
export const EXPORT_LIMITS = {
  minW: 64, maxW: 3840,
  minH: 64, maxH: 2160,
  minSpp: 1, maxSpp: 256,
  /** w*h*spp maximo por LLAMADA (una imagen, o un frame de una secuencia/capa). */
  maxBudget: 260_000_000,
  maxSequenceFrames: 240,
  /** frames*w*h*spp maximo para UNA secuencia completa (spec 7.7: "presupuesto
   *  frames * w * h con corte duro"). A 1920x1080 y 64 muestras (132 704 400
   *  px-muestra por frame) caben 60 frames de una tacada; por encima de eso
   *  hace falta bajar resolucion/muestras o partir la secuencia en varias
   *  llamadas (cada una cuenta contra CAPS.exportsPerTurn igualmente). */
  maxSequenceBudget: 8_000_000_000,
  /** movie: mismo tope de fotogramas POR LLAMADA que una secuencia (spec K6:
   *  "frames * w * h con corte duro"; una llamada de mi3_export ya cuenta
   *  contra CAPS.exportsPerTurn Y CAPS.moviePerTurn). */
  maxMovieFrames: 240,
  maxMovieBudget: 8_000_000_000,
  minFps: 1, maxFps: 60,
};

function padFrame(f: number): string {
  return String(Math.round(f)).padStart(4, '0');
}

/**
 * movie: "from"/"to" pueden venir sin resolver (dependen de man.marker, que
 * solo se conoce tras assistant.render_snapshot -- mismo motivo que
 * `plan.frame ?? man.marker` para image/white_details). Funcion COMPARTIDA
 * entre buildExportOpScript (que fija el rango que de verdad se renderiza) y
 * runExportJob (que reporta frames/duracion en el resultado) para que las dos
 * cuentas NUNCA diverjan.
 */
function resolveMovieRange(man: RenderManifest, plan: ExportPlan): { from: number; to: number; step: number; fps: number } {
  const fps = plan.fps ?? 24;
  const from = plan.from ?? man.marker;
  const to = plan.to ?? from + Math.max(1, Math.round((plan.durationS ?? 2) * fps)) - 1;
  return { from, to, step: Math.max(1, plan.step ?? 1), fps };
}

function buildExportOpScript(man: RenderManifest, plan: ExportPlan, sceneInProj: string, jobDirAbs: string): unknown {
  const camRef = plan.camera ? { ref: plan.camera } : {};
  const warmPng = path.join(jobDirAbs, 'warmup.png').replace(/\\/g, '/');
  const ops: Record<string, unknown>[] = [
    { id: 'open', op: 'project.open', args: { path: sceneInProj.replace(/\\/g, '/') } },
    // RONDA L (H7): project_load re-importa el preset con nombre DESPUES del
    // bloque "render" del proyecto (project_load.gml:93-100), y los presets
    // performance/balanced/extreme traen "engine": 0. Medido: un proyecto con
    // "engine": 1 guardado y render_settings "performance" (lo que trae TODO
    // proyecto nuevo, macros.gml:72) se abre en motor CLASICO. Los ojos del
    // asistente tienen que renderizar con el MISMO motor que ve el usuario.
    { id: 'eng', op: 'project.set', args: { field: 'render_engine', value: man.engine } },
    { id: 'mark0', op: 'timeline.set_marker', args: { frame: man.marker } },
    // Mismo calentamiento que render.final (buildOpScript, mi3_render_quality):
    // el primer render de una corrida sale con la pose sin asentar.
    {
      id: 'warm',
      op: 'render.final',
      args: { ...camRef, width: 160, height: 90, samples: 1, denoise: 0, transparent: 0, out: warmPng },
    },
  ];

  const frame = plan.frame ?? man.marker;

  if (plan.kind === 'image' || plan.kind === 'white_details') {
    const outPng = path.join(plan.outDir, `${plan.baseName}.png`).replace(/\\/g, '/');
    ops.push({ id: 'mark', op: 'timeline.set_marker', args: { frame } });
    ops.push({
      id: 'shot',
      op: 'render.final',
      args: {
        ...camRef,
        width: plan.width,
        height: plan.height,
        samples: plan.spp,
        denoise: plan.denoise ? 1 : 0,
        transparent: plan.transparent ? 1 : 0,
        out: outPng,
      },
    });
    if (plan.kind === 'white_details') {
      const wdOut = path.join(plan.outDir, `${plan.baseName}_whitedetails.png`).replace(/\\/g, '/');
      const wdArgs: Record<string, unknown> = { in: outPng, out: wdOut };
      if (plan.wdDensity !== undefined) wdArgs.density = plan.wdDensity;
      if (plan.wdAngle !== undefined) wdArgs.angle = plan.wdAngle;
      if (plan.wdWidth !== undefined) wdArgs.width = plan.wdWidth;
      ops.push({ id: 'wd', op: 'whitedetails.run', args: wdArgs });
    }
    return { stop_on_error: 1, ops };
  }

  if (plan.kind === 'sequence') {
    ops.push({
      id: 'seq',
      op: 'render.sequence',
      args: {
        from: plan.from,
        to: plan.to,
        step: plan.step ?? 1,
        out_dir: plan.outDir.replace(/\\/g, '/'),
        prefix: plan.baseName,
        width: plan.width,
        height: plan.height,
        samples: plan.spp,
        transparent: plan.transparent ? 1 : 0,
        ref: plan.camera ?? '',
        denoise: plan.denoise ? 1 : 0,
      },
    });
    return { stop_on_error: 1, ops };
  }

  if (plan.kind === 'movie') {
    const outMp4 = path.join(plan.outDir, `${plan.baseName}.mp4`).replace(/\\/g, '/');
    // Si "from"/"to" no vinieron, se exportan "durationS" segundos DESDE el
    // marker del snapshot (spec K6: "exportame 2 segundos de video" no trae
    // numeros de frame -- el anfitrion hace la aritmetica, no el modelo).
    const { from, to, step, fps } = resolveMovieRange(man, plan);
    ops.push({
      id: 'movie',
      op: 'export.movie',
      args: {
        out: outMp4,
        width: plan.width,
        height: plan.height,
        fps,
        from,
        to,
        step,
        samples: plan.spp,
        ...camRef,
      },
    });
    return { stop_on_error: 1, ops };
  }

  // 'layers'
  const outBase = path.join(plan.outDir, plan.baseName).replace(/\\/g, '/');
  ops.push({ id: 'mark', op: 'timeline.set_marker', args: { frame } });
  ops.push({
    id: 'layers',
    op: 'export.layers',
    args: {
      out_base: outBase,
      width: plan.width,
      height: plan.height,
      samples: plan.spp,
      ...camRef,
      layers: plan.layers ?? 0,
    },
  });
  return { stop_on_error: 1, ops };
}

/**
 * Corre un export completo (imagen/secuencia/white_details/capas) en la
 * instancia aislada y valida en el HOST lo que de verdad cayo en disco --
 * nunca se confia ciegamente en el codigo de salida del proceso (T4 del
 * export por capas y el guard de render.sequence ya reintentan del lado del
 * motor; esto es la ULTIMA linea de defensa, igual que runRenderJob).
 */
export async function runExportJob(
  p: Protocol,
  man: RenderManifest,
  plan: ExportPlan,
  onPhase: OnPhase,
  signal?: AbortSignal,
): Promise<ExportOutcome> {
  const jobDirAbs = path.join(p.dir, man.job_dir);
  const renderInst = ensureRenderInstance(p, man.install_dir, onPhase);
  const { sceneInProj } = prepareJobFolder(p, man, onPhase);

  const script = buildExportOpScript(man, plan, sceneInProj, jobDirAbs);
  const scriptPath = path.join(jobDirAbs, 'job.json');
  fs.writeFileSync(scriptPath, JSON.stringify(script, null, 1), 'utf8');

  // Presupuesto de tiempo: una secuencia de N frames tarda ~N x un frame; el
  // export por capas hace varias pasadas completas (capas + full + full2).
  // Estimacion generosa con un techo duro de 30 min -- por encima de eso algo
  // esta mal y es mejor que el job falle con un mensaje claro que colgarse.
  const perFrameMs = renderTimeoutMs(plan.width, plan.height, plan.spp);
  const passes =
    plan.kind === 'sequence'
      ? Math.max(1, Math.floor(((plan.to ?? 0) - (plan.from ?? 0)) / Math.max(1, plan.step ?? 1)) + 1)
      : plan.kind === 'movie'
        ? (() => {
            const r = resolveMovieRange(man, plan);
            return Math.max(1, Math.floor((r.to - r.from) / r.step) + 1);
          })()
        : plan.kind === 'layers'
          ? (plan.layers && plan.layers > 0 ? plan.layers + 2 : 6)
          : 1;
  const timeoutMs = Math.min(1_800_000, Math.max(perFrameMs, perFrameMs * passes));

  const pidFile = path.join(jobDirAbs, 'render.pid');
  const t0 = Date.now();
  onPhase('render', `${plan.kind} ${plan.width}x${plan.height}`);
  const outcome = await runHeadless(p, renderInst, scriptPath, pidFile, timeoutMs, signal, (elapsed) => {
    onPhase('render', `${Math.round(elapsed / 1000)} s`);
  });
  const ms = Date.now() - t0;

  try {
    if (outcome.stdout.trim()) fs.writeFileSync(path.join(jobDirAbs, 'result.json'), outcome.stdout, 'utf8');
  } catch {
    /* diagnostico, nunca fatal */
  }

  const files: ExportedFile[] = [];
  const info: Record<string, unknown> = {};

  if (plan.kind === 'image' || plan.kind === 'white_details') {
    const outAbs = path.join(plan.outDir, `${plan.baseName}.png`);
    if (!fs.existsSync(outAbs)) {
      onPhase('failed', 'sin PNG', ms);
      throw new RenderError(`el export no produjo imagen (codigo ${outcome.code}). ${(outcome.stderr || '').slice(0, 300)}`);
    }
    const dims = pngDims(outAbs);
    if (!dims) {
      onPhase('failed', 'PNG ilegible', ms);
      throw new RenderError('el PNG exportado no tiene una cabecera valida');
    }
    if (dims.w !== plan.width || dims.h !== plan.height) {
      onPhase('failed', `dimensiones ${dims.w}x${dims.h}`, ms);
      throw new RenderError(`el PNG exportado salio a ${dims.w}x${dims.h} y se pidio ${plan.width}x${plan.height}`);
    }
    files.push({ path: outAbs, w: dims.w, h: dims.h });

    if (plan.kind === 'white_details') {
      const wdAbs = path.join(plan.outDir, `${plan.baseName}_whitedetails.png`);
      if (!fs.existsSync(wdAbs)) {
        onPhase('failed', 'sin PNG de white details', ms);
        throw new RenderError('whitedetails.run no produjo la capa de detalles blancos');
      }
      const wdims = pngDims(wdAbs);
      if (!wdims) {
        onPhase('failed', 'white details PNG ilegible', ms);
        throw new RenderError('el PNG de white details no tiene cabecera valida');
      }
      files.push({ path: wdAbs, w: wdims.w, h: wdims.h });
    }
  } else if (plan.kind === 'sequence') {
    const from = plan.from ?? 0;
    const to = plan.to ?? from;
    const step = Math.max(1, plan.step ?? 1);
    for (let f = from; f <= to; f += step) {
      const outAbs = path.join(plan.outDir, `${plan.baseName}_${padFrame(f)}.png`);
      if (!fs.existsSync(outAbs)) {
        onPhase('failed', `falta frame ${padFrame(f)}`, ms);
        throw new RenderError(`la secuencia no genero el frame ${padFrame(f)} (${outAbs})`);
      }
      const dims = pngDims(outAbs);
      if (!dims || dims.w !== plan.width || dims.h !== plan.height) {
        onPhase('failed', `frame ${padFrame(f)} invalido`, ms);
        throw new RenderError(
          `el frame ${padFrame(f)} salio ${dims ? `${dims.w}x${dims.h}` : 'ilegible'}, se pidio ${plan.width}x${plan.height}`,
        );
      }
      files.push({ path: outAbs, w: dims.w, h: dims.h });
    }
    info.frames = files.length;
  } else if (plan.kind === 'movie') {
    const outAbs = path.join(plan.outDir, `${plan.baseName}.mp4`);
    if (!fs.existsSync(outAbs)) {
      onPhase('failed', 'sin MP4', ms);
      throw new RenderError(`el export de video no produjo fichero (codigo ${outcome.code}). ${(outcome.stderr || '').slice(0, 300)}`);
    }
    let size = 0;
    try { size = fs.statSync(outAbs).size; } catch { /* se valida abajo igual */ }
    if (size <= 0 || !looksLikeMp4(outAbs)) {
      onPhase('failed', 'MP4 invalido', ms);
      throw new RenderError(`el MP4 exportado no parece valido (tamano ${size} bytes, cabecera ftyp ${looksLikeMp4(outAbs) ? 'ok' : 'ausente'})`);
    }
    files.push({ path: outAbs, w: plan.width, h: plan.height });
    const range = resolveMovieRange(man, plan);
    const frames = Math.floor((range.to - range.from) / range.step) + 1;
    info.bytes = size;
    info.frames = frames;
    info.fps = range.fps;
    info.duration_s = frames / range.fps;
  } else {
    // 'layers'
    const fullAbs = path.join(plan.outDir, `${plan.baseName}_full.png`);
    if (!fs.existsSync(fullAbs)) {
      onPhase('failed', 'sin capa _full', ms);
      throw new RenderError(
        `el export por capas no produjo ${plan.baseName}_full.png (codigo ${outcome.code}). ${(outcome.stderr || '').slice(0, 300)}`,
      );
    }
    const fdims = pngDims(fullAbs);
    if (!fdims) {
      onPhase('failed', 'capa _full ilegible', ms);
      throw new RenderError('la capa _full.png no tiene cabecera valida');
    }
    files.push({ path: fullAbs, w: fdims.w, h: fdims.h });
    // layer00..layerNN: se listan las que existan de verdad en disco -- el
    // motor decide cuantas capas salen, el host nunca adivina un numero fijo.
    for (let k = 0; k <= 16; k++) {
      const layerAbs = path.join(plan.outDir, `${plan.baseName}_layer${String(k).padStart(2, '0')}.png`);
      if (!fs.existsSync(layerAbs)) break;
      const ldims = pngDims(layerAbs);
      if (ldims) files.push({ path: layerAbs, w: ldims.w, h: ldims.h });
    }
    info.layers = files.length - 1; // sin contar _full
    const manifestAbs = path.join(plan.outDir, `${plan.baseName}_layers.json`);
    if (fs.existsSync(manifestAbs)) info.manifest = manifestAbs;
  }

  onPhase('done', `${files.length} archivo(s)`, ms);
  pruneJobs(p, 2);

  return { files, ms, info };
}

/** Deja como mucho `keep` carpetas de job en disco (las mas recientes). */
export function pruneJobs(p: Protocol, keep: number): void {
  const root = path.join(p.dir, 'render_job');
  let entries: string[];
  try {
    entries = fs.readdirSync(root);
  } catch {
    return;
  }
  const dirs = entries
    .map((n) => ({ n, full: path.join(root, n) }))
    .filter((e) => { try { return fs.statSync(e.full).isDirectory(); } catch { return false; } })
    .map((e) => ({ ...e, t: (() => { try { return fs.statSync(e.full).mtimeMs; } catch { return 0; } })() }))
    .sort((a, b) => b.t - a.t);
  for (const d of dirs.slice(Math.max(0, keep))) rmQuiet(d.full);
}
