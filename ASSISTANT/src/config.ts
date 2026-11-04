// ASSISTANT/src/config.ts
//
// Command-line configuration: --dir, --provider, --model, --fake, --debug.
// launch.cmd currently relies on the default provider and passes --dir/--model.

import path from 'node:path';
import type { ProviderId } from './providers.js';

export const DEFAULT_MODEL = 'gpt-5.6-luna';

/**
 * RONDA UI (F2): nivel de razonamiento del SDK (Options.effort). 'high' es el
 * default del propio SDK; 'low' NO se ofrece en el desplegable del editor (en
 * un asistente que edita escenas de verdad produce trabajo que hay que
 * deshacer), pero se acepta por --effort para no mentir sobre el tipo.
 */
export type EffortLevel = 'low' | 'medium' | 'high' | 'xhigh' | 'max';
export const DEFAULT_EFFORT: EffortLevel = 'high';
const EFFORT_LEVELS: readonly string[] = ['low', 'medium', 'high', 'xhigh', 'max'];

export interface Config {
  /** Absolute path to Data/assistant/ (the protocol folder), created by the editor. */
  dir: string;
  /** Model id for the next turn; can change at runtime via a chat_in "model" record. */
  model: string;
  /** Provider adapter. Optional only for compatibility with older embedded callers. */
  provider?: ProviderId;
  /** RONDA UI (F2): effort del SDK; cambia en caliente via un record "effort". */
  effort: EffortLevel;
  /** --fake: run the deterministic scripted brain instead of the real SDK loop. */
  fake: boolean;
  /** --debug: verbose SDK debug logging via stderr. */
  debug: boolean;
}

export function parseArgs(argv: string[] = process.argv.slice(2)): Config {
  let dir: string | undefined;
  let model = DEFAULT_MODEL;
  let provider: ProviderId = 'codex';
  let effort: EffortLevel = DEFAULT_EFFORT;
  let fake = false;
  let debug = false;

  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    switch (a) {
      case '--dir':
        dir = argv[++i];
        break;
      case '--model':
        if (argv[i + 1] && !argv[i + 1].startsWith('--')) model = argv[++i];
        break;
      case '--provider': {
        const value = argv[++i];
        if (value !== 'codex' && value !== 'claude') throw new Error(`Unsupported visual-agent provider: ${value ?? ''}`);
        provider = value;
        break;
      }
      case '--effort':
        if (argv[i + 1] && !argv[i + 1].startsWith('--')) {
          const v = argv[++i];
          if (EFFORT_LEVELS.includes(v)) effort = v as EffortLevel;
        }
        break;
      case '--fake':
        fake = true;
        break;
      case '--debug':
        debug = true;
        break;
      default:
        // Be lenient about a bare positional path, but launch.cmd always
        // passes --dir/--model explicitly.
        if (dir === undefined && !a.startsWith('--')) dir = a;
        break;
    }
  }

  if (!dir) {
    throw new Error(
      '--dir <ruta absoluta> es obligatorio (carpeta Data/assistant del editor). ' +
        'Ejemplo: node dist/index.js --dir "C:\\...\\Data\\assistant" --model gpt-5.6-luna',
    );
  }

  return { dir: path.resolve(dir), model, provider, effort, fake, debug };
}
