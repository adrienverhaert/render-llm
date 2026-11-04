import path from 'node:path';
import readline from 'node:readline';
import { pathToFileURL } from 'node:url';
import { acquireActionHostLock, assertNativeChatIdle, releaseActionHostLock } from './action-host-lock.js';
import { Protocol } from './protocol.js';
import { renderLlmActionJson, runRenderLlmAction, runStillAction, runVideoAction } from './renderllm-actions.js';

export function parseExportOptions(argv: string[]) {
  const arg = (key: string) => {
    const i = argv.indexOf(key);
    if (i < 0 || !argv[i + 1] || argv[i + 1].startsWith('--')) throw new Error(`Missing ${key}`);
    return argv[i + 1];
  };
  const integer = (key: string, min: number, max: number) => {
    const n = Number(arg(key));
    if (!Number.isInteger(n) || n < min || n > max) throw new Error(`Invalid ${key}`);
    return n;
  };
  const kind = arg('--kind');
  if (kind !== 'image' && kind !== 'video' && kind !== 'sequence') throw new Error('Invalid export kind');
  return { dir: path.resolve(arg('--dir')), kind,
    width: integer('--width', 320, 1920), height: integer('--height', 180, 1080),
    samples: integer('--samples', 1, 256), fps: integer('--fps', 1, 60),
    frames: integer('--frames', 1, 240) };
}

async function main() {
  const opts = parseExportOptions(process.argv.slice(2));
  assertNativeChatIdle(opts.dir);
  const lock = acquireActionHostLock(opts.dir);
  const protocol = new Protocol(opts.dir, { actionOnly: true });
  const controller = new AbortController();
  const input = readline.createInterface({ input: process.stdin });
  input.on('line', (line) => { if (line.trim() === 'cancel') controller.abort(); });
  // If the native owner closes its pipe unexpectedly, stop the owned render too.
  input.on('close', () => controller.abort());
  const emit = (value: unknown) => process.stdout.write(`${JSON.stringify(value)}\n`);
  try {
    const context = { turnId: '', signal: controller.signal };
    const before = renderLlmActionJson(await runRenderLlmAction(protocol, context, 'scene_get', {}));
    if (before.success !== true) throw new Error(String(before.error ?? 'Could not read the scene'));
    const snapshot = JSON.parse(String(before.document));
    const args = { ...opts, expected_revision: Number(before.revision), denoise: snapshot.render?.denoise !== false,
      format: opts.kind === 'sequence' ? 'png-sequence' as const : 'mp4' as const };
    const phase = (name: string, detail?: string) => emit({ type: 'phase', phase: name, detail });
    const result = opts.kind === 'image'
      ? await runStillAction(protocol, context, args, phase)
      : await runVideoAction(protocol, context, args, phase);
    emit({ type: 'result', ...result });
  } catch (error) {
    if (controller.signal.aborted) emit({ type: 'cancelled' });
    else throw error;
  } finally {
    input.close(); protocol.close(); releaseActionHostLock(lock);
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((e) => { process.stderr.write(`${e instanceof Error ? e.message : String(e)}\n`); process.exitCode = 1; });
}
