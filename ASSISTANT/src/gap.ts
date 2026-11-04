// ASSISTANT/src/gap.ts
//
// RONDA K0: autoreporte de huecos ("no vuelvas a mandarme capturas"). Ver
// SPEC_ASSISTANT_RONDA_K.md seccion 5. Buzon LOCAL AL CEREBRO -- como
// memory.ts, sin op de editor -- hacia Data/assistant/gap_log.jsonl.
//
// Invariantes que este archivo existe para sostener:
//   - Una linea = una capacidad faltante DISTINTA. Pedir lo mismo con otras
//     palabras no anade linea: dedupe por texto normalizado, sube `n`.
//   - No hay tool de LECTURA: esto es un buzon hacia el desarrollador, no
//     memoria del modelo (a diferencia de memory.ts, que SI se reinyecta en
//     el prompt). mi3_report_gap solo escribe.
//   - UTF-8 sin BOM, escritura .part + rename (mismo patron que memory.ts /
//     protocol.ts: el usuario o el editor pueden tener el fichero abierto).
//   - Tope 300 entradas, poda por `n` ASCENDENTE y luego antiguedad -- nunca
//     se poda la entrada que la propia llamada acaba de tocar.

import fs from 'node:fs';
import path from 'node:path';

export const GAP_LIMITS = {
  maxEntries: 300,
  minWantChars: 5,
  maxWantChars: 200,
  maxTriedChars: 200,
} as const;

export type GapWhy = 'no_tool' | 'tool_failed' | 'not_measured' | 'refused_by_policy';
const GAP_WHY_VALUES: readonly GapWhy[] = ['no_tool', 'tool_failed', 'not_measured', 'refused_by_policy'];

interface GapEntry {
  t: number; // epoch seconds -- ultima vez que se vio
  turn_id: string; // del ULTIMO reporte
  project: string; // '' si no hay proyecto abierto
  want: string; // texto original (sin normalizar) del ULTIMO reporte
  why: GapWhy;
  tried?: string;
  model: string;
  n: number; // veces vista, tras dedupe
}

/**
 * Clave de dedupe: minusculas, sin acentos (normaliza a NFD y quita las
 * marcas combinantes U+0300-U+036F, asi "exportacion" y "exportación" caen
 * en la misma clave), sin numeros, sin puntuacion, espacios colapsados.
 */
function normalizeKey(want: string): string {
  return want
    .toLowerCase()
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/[0-9]/g, '')
    .replace(/[^a-z\s]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

export class GapLog {
  readonly file: string;
  private readonly logFn: (m: string) => void;

  constructor(dataDir: string, logFn: (m: string) => void) {
    this.file = path.join(path.resolve(dataDir), 'gap_log.jsonl');
    this.logFn = logFn;
  }

  private readAll(): GapEntry[] {
    let raw = '';
    try {
      raw = fs.readFileSync(this.file, 'utf8');
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code !== 'ENOENT') {
        this.logFn(`WARN gap_log: no se pudo leer ${this.file}: ${String(e)}`);
      }
      return [];
    }
    const out: GapEntry[] = [];
    const lines = raw.split('\n');
    for (let i = 0; i < lines.length; i++) {
      const s = lines[i].replace(/\r$/, '').trim();
      if (!s) continue;
      try {
        const o = JSON.parse(s) as Partial<GapEntry>;
        if (typeof o.want !== 'string' || o.want === '') continue;
        out.push({
          t: typeof o.t === 'number' && Number.isFinite(o.t) ? o.t : 0,
          turn_id: typeof o.turn_id === 'string' ? o.turn_id : '',
          project: typeof o.project === 'string' ? o.project : '',
          want: o.want,
          why: GAP_WHY_VALUES.includes(o.why as GapWhy) ? (o.why as GapWhy) : 'no_tool',
          tried: typeof o.tried === 'string' ? o.tried : undefined,
          model: typeof o.model === 'string' ? o.model : '',
          n: typeof o.n === 'number' && o.n > 0 ? Math.floor(o.n) : 1,
        });
      } catch {
        this.logFn(`WARN gap_log: linea ${i + 1} no valida ignorada: ${s.slice(0, 80)}`);
      }
    }
    return out;
  }

  private writeAll(entries: GapEntry[]): void {
    const body =
      entries
        .map((e) =>
          JSON.stringify({
            t: e.t,
            turn_id: e.turn_id,
            project: e.project,
            want: e.want,
            why: e.why,
            ...(e.tried !== undefined ? { tried: e.tried } : {}),
            model: e.model,
            n: e.n,
          }),
        )
        .join('\n') + (entries.length ? '\n' : '');
    const tmp = this.file + '.part';
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    fs.writeFileSync(tmp, body, { encoding: 'utf8' });
    let lastErr: unknown;
    for (let i = 0; i < 12; i++) {
      try {
        fs.renameSync(tmp, this.file);
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

  /**
   * Registra un hueco. Dedupe por texto normalizado: si ya existe una entrada
   * con la misma clave, sube `n` y refresca t/turn_id/project/want/why/tried/
   * model EN VEZ de anadir una linea nueva -- el fichero se lee como un
   * ranking de lo que mas falta, no como un log cronologico.
   */
  report(
    args: { want: string; why: GapWhy; tried?: string; turnId: string; project: string; model: string },
    now = new Date(),
  ): { n: number; isNew: boolean } {
    const entries = this.readAll();
    const key = normalizeKey(args.want);
    const t = Math.floor(now.getTime() / 1000);

    const idx = entries.findIndex((e) => normalizeKey(e.want) === key);
    let n: number;
    if (idx >= 0) {
      const e = entries[idx];
      e.t = t;
      e.turn_id = args.turnId;
      e.project = args.project;
      e.want = args.want;
      e.why = args.why;
      e.tried = args.tried;
      e.model = args.model;
      e.n += 1;
      n = e.n;
    } else {
      entries.push({
        t,
        turn_id: args.turnId,
        project: args.project,
        want: args.want,
        why: args.why,
        tried: args.tried,
        model: args.model,
        n: 1,
      });
      n = 1;
    }

    // --- poda: tope de entradas, por `n` ASCENDENTE y luego mas antigua,
    //     nunca la entrada que esta misma llamada acaba de tocar ---
    while (entries.length > GAP_LIMITS.maxEntries) {
      let worstIdx = -1;
      for (let i = 0; i < entries.length; i++) {
        if (normalizeKey(entries[i].want) === key) continue;
        if (
          worstIdx === -1 ||
          entries[i].n < entries[worstIdx].n ||
          (entries[i].n === entries[worstIdx].n && entries[i].t < entries[worstIdx].t)
        ) {
          worstIdx = i;
        }
      }
      if (worstIdx === -1) break; // solo queda la entrada recien tocada
      entries.splice(worstIdx, 1);
    }

    this.writeAll(entries);
    this.logFn(
      `RONDA K0: gap registrado key="${key}" n=${n} why=${args.why} project="${args.project}" want="${args.want}"`,
    );
    return { n, isNew: idx < 0 };
  }
}
