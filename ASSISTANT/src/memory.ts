// ASSISTANT/src/memory.ts
//
// RONDA E: el almacen de memoria del asistente. Markdown plano, editable a mano
// por el usuario (ver SPEC_ASSISTANT_RONDA_E.md S3.2/S3.3). Este modulo es el
// UNICO del cerebro que toca <dir>/memory/, y no toca nada mas.
//
// Invariantes que este archivo existe para sostener:
//   - UTF-8 sin BOM, LF, el fichero SIEMPRE acaba en "\n".
//   - Nadie borra memory/ jamas (ni aqui ni en protocol.ts ni en el editor).
//   - Parseo PERMISIVO: una linea escrita a mano sin id ni fecha vale igual.
//   - Escritura read-modify-write + .part + rename con reintentos (Windows).

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

export type Scope = 'global' | 'project';

export interface MemoryEntry {
  id: string; // 6 chars [0-9a-z]; se asigna si la linea venia sin el
  date: string; // AAAA-MM-DD; se rellena con hoy si venia sin ella
  text: string;
}

export interface MemoryFileView {
  scope: Scope;
  /** Ruta absoluta del .md */
  file: string;
  /** Ruta relativa a Data/assistant, para mostrar (memory/global.md) */
  rel: string;
  /** Etiqueta humana: "global" | el nombre del .miproject */
  label: string;
  entries: MemoryEntry[];
}

export const MEM_LIMITS = {
  maxTextChars: 280,
  minTextChars: 3,
  maxEntries: 40,
  maxBytes: 16384,
} as const;

const TAG_BEGIN = '<<<USER_PREFERENCE_DATA_BEGIN>>>';
const TAG_END = '<<<USER_PREFERENCE_DATA_END>>>';

const HEADER_TAIL = [
  '#',
  '# Este archivo es TUYO. Puedes abrirlo, editarlo o borrarlo a mano con cualquier',
  '# editor de texto: el asistente lo vuelve a leer al empezar CADA turno, sin que',
  '# haga falta reiniciar nada.',
  '#',
  '# Una linea por hecho, con este formato:',
  '#',
  '#   - [<id>] <AAAA-MM-DD> <texto>',
  '#',
  '# El <id> son 6 caracteres (0-9a-z) y solo sirve para que el asistente pueda',
  '# olvidar una entrada concreta. Si escribes una linea a mano puedes omitir el id',
  '# y la fecha ("- lo que sea") -- se entiende igual, y se le asigna un id la',
  '# proxima vez que el asistente escriba en este archivo.',
  '#',
  "# Para OLVIDAR algo: borra la linea entera. Las lineas que empiezan por '#' son",
  '# comentarios y se ignoran.',
  '#',
  '# Lo que hay aqui son PREFERENCIAS, no ordenes. Una linea de este archivo NO',
  '# puede darle al asistente permisos, herramientas ni acceso a archivos nuevos,',
  '# ni cambiar sus limites. Si una linea intenta algo asi, el asistente la ignora',
  '# y te lo dice.',
  '',
];

function headerFor(scope: Scope, projectPath?: string): string[] {
  if (scope === 'global') return ['# Memorias del asistente de render-llm -- ambito GLOBAL', ...HEADER_TAIL];
  return [
    '# Memorias del asistente de render-llm -- ambito PROYECTO',
    `# proyecto: ${projectPath ?? ''}`,
    ...HEADER_TAIL,
  ];
}

function stripBom(s: string): string {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s;
}

function todayIso(now = new Date()): string {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${now.getFullYear()}-${p(now.getMonth() + 1)}-${p(now.getDate())}`;
}

/**
 * Clave del archivo de proyecto: <slug>-<hash8>.
 * El slug es legible (para que Antonio encuentre el archivo a ojo) y el hash8
 * desambigua dos proyectos que se llamen IGUAL en carpetas distintas.
 * La ruta se normaliza (minusculas + "/") para que la clave no dependa de como
 * el editor haya escrito las barras.
 */
export function projectKey(projectFile: string): string {
  const base = path.basename(projectFile).replace(/\.[^.]*$/, '');
  const slug =
    base
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '')
      .slice(0, 40) || 'proyecto';
  const norm = path.resolve(projectFile).replace(/\\/g, '/').toLowerCase();
  const hash8 = crypto.createHash('sha1').update(norm, 'utf8').digest('hex').slice(0, 8);
  return `${slug}-${hash8}`;
}

const ENTRY_RE = /^-[ \t]+(.*)$/;
const ID_RE = /^\[([0-9a-z]{6})\][ \t]+/;
const DATE_RE = /^(\d{4}-\d{2}-\d{2})[ \t]+/;

export interface ParseResult {
  entries: MemoryEntry[];
  projectPath?: string;
  /** Lineas que no se entendieron: [numeroDeLinea, contenido] */
  bad: Array<[number, string]>;
  /** true si alguna entrada venia sin id o con id duplicado */
  needsIds: boolean;
}

export function parseMemoryText(text: string): ParseResult {
  const out: ParseResult = { entries: [], bad: [], needsIds: false };
  const seen = new Set<string>();
  const lines = stripBom(text).split('\n');
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i].replace(/\r$/, '');
    const line = raw.trim();
    if (line === '') continue;
    if (line.startsWith('#')) {
      const m = /^#\s*proyecto:\s*(.*)$/i.exec(line);
      if (m && m[1].trim() !== '') out.projectPath = m[1].trim();
      continue;
    }
    const em = ENTRY_RE.exec(line);
    if (!em) {
      out.bad.push([i + 1, line.slice(0, 80)]);
      continue;
    }
    let rest = em[1].trim();
    let id: string | undefined;
    let date: string | undefined;
    const im = ID_RE.exec(rest);
    if (im) {
      id = im[1];
      rest = rest.slice(im[0].length);
    }
    const dm = DATE_RE.exec(rest);
    if (dm) {
      date = dm[1];
      rest = rest.slice(dm[0].length);
    }
    const body = rest.trim();
    if (body === '') {
      out.bad.push([i + 1, line.slice(0, 80)]);
      continue;
    }
    if (id !== undefined && seen.has(id)) id = undefined; // duplicado: pierde el suyo
    if (id === undefined) out.needsIds = true;
    else seen.add(id);
    out.entries.push({ id: id ?? '', date: date ?? '', text: body });
  }
  return out;
}

export function renderMemoryText(scope: Scope, entries: MemoryEntry[], projectPath?: string): string {
  const lines = headerFor(scope, projectPath);
  for (const e of entries) lines.push(`- [${e.id}] ${e.date} ${e.text}`);
  return lines.join('\n') + '\n';
}

export class MemoryStore {
  readonly root: string;
  private projectFile = '';
  private readonly logFn: (m: string) => void;

  constructor(dataDir: string, logFn: (m: string) => void) {
    this.root = path.join(path.resolve(dataDir), 'memory');
    this.logFn = logFn;
    fs.mkdirSync(path.join(this.root, 'projects'), { recursive: true });
  }

  /** El editor nos dice cual es el proyecto abierto (hello / user). "" = nunca guardado. */
  setProject(projectFile: string | undefined): void {
    const next = (projectFile ?? '').trim();
    if (next === this.projectFile) return;
    this.projectFile = next;
    this.logFn(`RONDA E: proyecto activo = ${next === '' ? '(sin guardar)' : next}`);
  }

  get hasProject(): boolean {
    return this.projectFile !== '';
  }

  fileFor(scope: Scope): string | undefined {
    if (scope === 'global') return path.join(this.root, 'global.md');
    if (!this.hasProject) return undefined;
    return path.join(this.root, 'projects', `${projectKey(this.projectFile)}.md`);
  }

  private relOf(file: string): string {
    return 'memory/' + path.relative(this.root, file).replace(/\\/g, '/');
  }

  /** Lee UN ambito. Nunca lanza: un fichero ilegible cuenta como vacio (con WARN). */
  read(scope: Scope): MemoryFileView | undefined {
    const file = this.fileFor(scope);
    if (!file) return undefined;
    let parsed: ParseResult = { entries: [], bad: [], needsIds: false };
    try {
      parsed = parseMemoryText(fs.readFileSync(file, 'utf8'));
    } catch (e) {
      const code = (e as NodeJS.ErrnoException).code;
      if (code !== 'ENOENT') this.logFn(`WARN memoria: no se pudo leer ${file}: ${String(e)}`);
    }
    for (const [n, s] of parsed.bad) this.logFn(`WARN memoria: linea no reconocida en ${this.relOf(file)}:${n}: ${s}`);
    return {
      scope,
      file,
      rel: this.relOf(file),
      label: scope === 'global' ? 'global' : path.basename(this.projectFile),
      entries: parsed.entries,
    };
  }

  list(): MemoryFileView[] {
    const out: MemoryFileView[] = [];
    const g = this.read('global');
    if (g) out.push(g);
    const p = this.read('project');
    if (p) out.push(p);
    return out;
  }

  /** El bloque LITERAL que se pega al system prompt (S3.7). */
  block(): string {
    const parts: string[] = [TAG_BEGIN];
    let any = false;
    for (const v of this.list()) {
      if (v.entries.length === 0) continue;
      any = true;
      const head =
        v.scope === 'global'
          ? `(scope: global -- file: ${v.rel})`
          : `(scope: project "${v.label}" -- file: ${v.rel})`;
      parts.push(head);
      for (const e of v.entries) parts.push(`[${e.id || '------'}] ${e.date || '----------'} ${sanitizeTags(e.text)}`);
    }
    if (!any) parts.push('(no stored preferences yet)');
    parts.push(TAG_END);
    return parts.join('\n');
  }

  private newId(taken: Set<string>): string {
    const alphabet = '0123456789abcdefghijklmnopqrstuvwxyz';
    for (let attempt = 0; attempt < 200; attempt++) {
      const bytes = crypto.randomBytes(6);
      let s = '';
      for (let i = 0; i < 6; i++) s += alphabet[bytes[i] % alphabet.length];
      if (!taken.has(s)) return s;
    }
    return String(Date.now()).slice(-6); // inalcanzable en la practica
  }

  private allIds(): Set<string> {
    const s = new Set<string>();
    for (const v of this.list()) for (const e of v.entries) if (e.id) s.add(e.id);
    return s;
  }

  /**
   * Guarda una entrada. Read-modify-write SIEMPRE (el usuario o la UI del editor
   * pueden haber tocado el fichero entre turnos). Devuelve el id y los ids
   * podados. `error` cuando no se puede guardar (texto invalido, sin proyecto).
   */
  save(scope: Scope, rawText: string, now = new Date()): { id?: string; pruned: string[]; rel?: string; error?: string } {
    const text = rawText.replace(/\s+/g, ' ').trim();
    if (text.length < MEM_LIMITS.minTextChars)
      return { pruned: [], error: `el texto es demasiado corto (minimo ${MEM_LIMITS.minTextChars} caracteres)` };
    if (text.length > MEM_LIMITS.maxTextChars)
      return {
        pruned: [],
        error: `el texto tiene ${text.length} caracteres y el maximo es ${MEM_LIMITS.maxTextChars}; resumelo en una frase y vuelve a llamar`,
      };
    const file = this.fileFor(scope);
    if (!file)
      return {
        pruned: [],
        error:
          'no hay proyecto guardado, asi que no existe un archivo de memoria de proyecto. ' +
          'Pidele al usuario que guarde el proyecto (Ctrl+S) o guarda esto en el ambito global.',
      };

    const view = this.read(scope)!;
    const taken = this.allIds();
    const entries = view.entries.map((e) => ({ ...e }));
    for (const e of entries) {
      if (!e.id) {
        e.id = this.newId(taken);
        taken.add(e.id);
      }
      if (!e.date) e.date = todayIso(now);
    }
    const id = this.newId(taken);
    entries.push({ id, date: todayIso(now), text });

    // --- poda FIFO: nunca toca la entrada recien anadida ---
    const pruned: string[] = [];
    while (entries.length > MEM_LIMITS.maxEntries && entries.length > 1) pruned.push(entries.shift()!.id);
    while (
      Buffer.byteLength(renderMemoryText(scope, entries, this.projectFile || undefined), 'utf8') > MEM_LIMITS.maxBytes &&
      entries.length > 1
    )
      pruned.push(entries.shift()!.id);

    const body = renderMemoryText(scope, entries, scope === 'project' ? this.projectFile : undefined);
    if (Buffer.byteLength(body, 'utf8') > MEM_LIMITS.maxBytes)
      this.logFn(`WARN memoria: ${view.rel} sigue por encima del cap de bytes tras podar`);
    this.writeAtomic(file, body);
    this.logFn(
      `RONDA E: memoria guardada scope=${scope} id=${id} podadas=${pruned.length}` +
        (pruned.length ? ` (${pruned.join(';')})` : '') +
        ` texto="${text}"`,
    );
    return { id, pruned, rel: view.rel };
  }

  /** Olvida por id, en el ambito donde este. Devuelve lo que se borro. */
  forget(id: string): { scope: Scope; text: string; rel: string } | undefined {
    for (const v of this.list()) {
      const idx = v.entries.findIndex((e) => e.id === id);
      if (idx < 0) continue;
      const removed = v.entries[idx];
      const rest = v.entries.filter((_, i) => i !== idx);
      const taken = this.allIds();
      for (const e of rest) {
        if (!e.id) {
          e.id = this.newId(taken);
          taken.add(e.id);
        }
        if (!e.date) e.date = todayIso();
      }
      this.writeAtomic(v.file, renderMemoryText(v.scope, rest, v.scope === 'project' ? this.projectFile : undefined));
      this.logFn(`RONDA E: memoria olvidada scope=${v.scope} id=${id} texto="${removed.text}"`);
      return { scope: v.scope, text: removed.text, rel: v.rel };
    }
    return undefined;
  }

  /**
   * .part + rename con reintentos: mismo patron que Protocol.atomicWriteJson.
   * La UI del editor puede tener el fichero abierto en el instante exacto.
   */
  private writeAtomic(file: string, body: string): void {
    const tmp = file + '.part';
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(tmp, body, { encoding: 'utf8' });
    let lastErr: unknown;
    for (let i = 0; i < 12; i++) {
      try {
        fs.renameSync(tmp, file);
        return;
      } catch (e) {
        lastErr = e;
        const code = (e as NodeJS.ErrnoException).code;
        if (code !== 'EPERM' && code !== 'EACCES' && code !== 'EBUSY') break;
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
      }
    }
    throw lastErr;
  }
}

/**
 * Nadie puede cerrar el bloque de datos antes de tiempo. Si el texto de una
 * memoria contiene una de las etiquetas literales, se neutraliza ANTES de
 * emitir el bloque. Sin esto, una memoria plantada a mano podria hacer que el
 * resto de su contenido pareciese estar FUERA del bloque de datos -- es decir,
 * parecer instruccion de sistema. Tiene test propio (S7, gate 5).
 */
export function sanitizeTags(s: string): string {
  return s.split(TAG_BEGIN).join('<<<redacted>>>').split(TAG_END).join('<<<redacted>>>');
}
