// ASSISTANT/src/paths.ts
//
// RONDA K4: la jaula de rutas para mi3_import (SPEC_ASSISTANT_RONDA_K.md
// seccion 2.3, INV-3). Una ruta que el modelo quiere abrir SOLO es valida si
// cumple (a) o (b):
//
//   (a) LITERAL DEL USUARIO -- la cadena aparece TEXTUALMENTE en el mensaje
//       del usuario de ESTE turno (ctx.userText, que en index.ts es
//       EXACTAMENTE rec.text y nada mas -- nunca memoria, nunca un resultado
//       de tool, nunca el nombre de un adjunto). Comparacion en el
//       ANFITRION: normaliza "/"->"\\", quita comillas rectas y tipograficas,
//       trim, sin distinguir mayusculas, subcadena completa.
//   (b) RAIZ DESIGNADA -- resuelta por el EDITOR (projects/project, via el
//       op assistant.paths) o por el propio anfitrion relativo a --dir
//       (attachments/data/exports), NUNCA por el modelo. Prefijo tras
//       path.resolve; nada de ".." tras resolver, nada de UNC, nada de otra
//       unidad.
//
// PROHIBIDO EXPLICITAMENTE (y hay un gate adversarial para esto, G4.1e): que
// una ruta salga de la MEMORIA, de un resultado de tool, del nombre de un
// adjunto o de cualquier texto que no sea rec.text de ESTE turno. Por eso
// checkPathCage() solo acepta userText como parametro -- nunca "el ultimo
// texto que dijo el modelo" ni nada por el estilo.

import fs from 'node:fs';
import path from 'node:path';

/** Las 6 raices designadas de la seccion 2.3 (RONDA L, H3a: se anadio
 *  "particles"). '' = no disponible ahora mismo (p.ej. "project" antes de
 *  guardar por primera vez). */
export interface DesignatedRoots {
  projects: string;
  project: string;
  attachments: string;
  data: string;
  exports: string;
  /** RONDA L (H3a): carpeta `Particles` de la INSTALACION, hermana de
   *  `Data/` -- solo `.miparticles` viven ahi. Medido: `Data/Particles` no
   *  existe en ninguna instalacion (ni la aislada de esta ronda ni la real);
   *  el motor la resuelve con el macro `particles_directory` = `working_directory
   *  + "Particles/"` (macros.gml:41). */
  particles: string;
}

/** Subcarpetas de catalogo LEGIBLES dentro de la raiz "data" -- el resto de
 *  Data/ (motor, fuentes internas, etc.) no es una raiz de import valida
 *  aunque exista en disco. RONDA L (H3a): "Particles" salio de esta lista --
 *  esa carpeta vive en la raiz de la instalacion, no dentro de Data/, y
 *  dejarla aqui sin efecto hacia que la lista mintiera. La raiz real es la
 *  nueva "particles" de arriba. */
const DATA_READ_SUBFOLDERS = ['Canon', 'Render'];

function isUnc(p: string): boolean {
  return p.startsWith('\\\\') || p.startsWith('//');
}

function pathSyntax(root: string): typeof path.win32 {
  if (/^[A-Za-z]:/.test(root)) return path.win32;
  return root.startsWith('/') ? path.posix : path;
}

function normalizeForCompare(s: string): string {
  return s
    .replace(/[‘’‛]/g, "'")
    .replace(/[“”‟]/g, '"')
    .replace(/['"]/g, '')
    .replace(/\//g, '\\')
    .trim()
    .toLowerCase();
}

/** (a): candidate aparece TEXTUALMENTE (subcadena) en userText, normalizando
 *  barras/comillas/mayusculas por los dos lados. */
export function appearsLiterallyInMessage(candidate: string, userText: string): boolean {
  const needle = normalizeForCompare(candidate);
  if (needle.length === 0) return false;
  const haystack = normalizeForCompare(userText ?? '');
  return haystack.includes(needle);
}

/** Resuelve `candidate` (absoluto o relativo a `root`) y comprueba que caiga
 *  DENTRO de `root` tras resolver. null si no. */
function resolveUnderRoot(candidate: string, root: string): string | null {
  if (!candidate || !root) return null;
  if (isUnc(candidate) || isUnc(root)) return null;
  const syntax = pathSyntax(root);
  if (syntax !== path.win32 && /^[A-Za-z]:/.test(candidate)) return null;
  const rr = syntax.resolve(root);
  const cand = syntax.resolve(rr, candidate.replace(/\\/g, '/'));
  const relative = syntax.relative(rr, cand);
  if (relative === '' || (!syntax.isAbsolute(relative) && relative !== '..' && !relative.startsWith('..' + syntax.sep))) return cand;
  return null;
}

/** (b): dentro de una de las raices designadas. Prueba en este orden:
 *  project, projects, attachments, exports, data (data restringida ADEMAS a
 *  Canon/Render), particles (RONDA L: hoja sin restriccion de subcarpeta,
 *  ya es una carpeta terminal). */
export function resolveInDesignatedRoots(
  candidate: string,
  roots: DesignatedRoots,
): { resolved: string; root: keyof DesignatedRoots } | null {
  const order: Array<keyof DesignatedRoots> = ['project', 'projects', 'attachments', 'exports', 'data', 'particles'];
  for (const key of order) {
    const root = roots[key];
    if (!root) continue;
    const resolved = resolveUnderRoot(candidate, root);
    if (!resolved) continue;
    if (key === 'data') {
      const syntax = pathSyntax(root);
      const rel = syntax.relative(syntax.resolve(root), resolved);
      const top = rel.split(syntax.sep)[0];
      if (!DATA_READ_SUBFOLDERS.includes(top)) continue;
    }
    return { resolved, root: key };
  }
  return null;
}

export interface CageResult {
  ok: boolean;
  resolved?: string;
  via?: 'literal' | keyof DesignatedRoots;
  reason?: string;
}

/**
 * El unico punto de entrada que mi3_import deberia usar para un "path"
 * suelto por el usuario. Valida contra (a) o (b) y devuelve la ruta YA
 * RESUELTA por el anfitrion -- lo que se le manda al editor nunca es la
 * cadena cruda del modelo sin pasar por aqui.
 */
export function checkPathCage(candidate: string, userText: string, roots: DesignatedRoots): CageResult {
  const trimmed = (candidate ?? '').trim();
  if (trimmed === '') return { ok: false, reason: 'ruta vacia' };
  if (isUnc(trimmed)) return { ok: false, reason: 'rutas de red (UNC) no permitidas' };

  if (appearsLiterallyInMessage(trimmed, userText)) {
    const root = roots.projects || roots.project || process.cwd();
    const syntax = pathSyntax(/^[A-Za-z]:/.test(trimmed) ? trimmed : root);
    const resolved = syntax.resolve(root, trimmed.replace(/\\/g, '/'));
    return { ok: true, resolved, via: 'literal' };
  }

  const hit = resolveInDesignatedRoots(trimmed, roots);
  if (hit) return { ok: true, resolved: hit.resolved, via: hit.root };

  return {
    ok: false,
    reason:
      'solo puedo abrir rutas que el usuario haya escrito en este mensaje o que esten en sus carpetas de proyecto',
  };
}

/**
 * Resuelve un adjunto de ESTE turno por NOMBRE PELADO (nunca una ruta): el
 * mismo saneo de profundidad que readAttachments (attachments.ts) --
 * intencionalmente duplicado en vez de importado, porque este helper
 * devuelve una RUTA para pasar a un op de import (res.import/asset.import),
 * mientras que readAttachments devuelve bytes base64 para el mensaje; son
 * consumidores distintos y no vale la pena acoplarlos.
 */
export function resolveAttachmentPath(dataDir: string, turnId: string, name: string): string | null {
  if (!name) return null;
  if (name.includes('/') || name.includes('\\') || name.includes(':') || name.includes('..')) return null;
  const safeTurn = turnId.replace(/[^A-Za-z0-9_-]/g, '_');
  const turnDir = path.resolve(dataDir, 'attachments', safeTurn);
  const full = path.resolve(turnDir, name);
  if (!full.startsWith(turnDir + path.sep)) return null;
  try {
    if (!fs.statSync(full).isFile()) return null;
  } catch {
    return null;
  }
  return full;
}

/** Raices "data" (Canon/Render) que el anfitrion puede resolver el solo,
 *  relativas a --dir (Data/assistant/../ = Data/), sin ir y volver al editor:
 *  son subcarpetas de instalacion, no estado de proyecto. */
export function dataRootFromAssistantDir(dataDir: string): string {
  return path.resolve(dataDir, '..');
}

/** Raiz "particles": <instalacion>/Particles (hermana de Data/, NO dentro de
 *  Data/ -- medido en la ronda L: Data/Particles no existe en ninguna
 *  instalacion). dataDir es <instalacion>/Data/assistant, asi que la
 *  instalacion son dos niveles arriba. */
export function particlesRootFromAssistantDir(dataDir: string): string {
  return path.resolve(dataDir, '..', '..', 'Particles');
}
