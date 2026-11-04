import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import { pathToFileURL } from 'node:url';
import {
  acquireActionHostLock,
  assertNativeChatIdle,
  releaseActionHostLock,
} from './action-host-lock.js';
import { Protocol } from './protocol.js';
import {
  renderLlmActionJson,
  runColorBatchAction,
  runRenderLlmAction,
} from './renderllm-actions.js';

interface Options {
  dir: string;
  objectId: string;
  colors: string[];
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
}

function valueAfter(argv: string[], name: string): string {
  const index = argv.indexOf(name);
  const value = index >= 0 ? argv[index + 1] : undefined;
  if (!value || value.startsWith('--')) throw new Error(`Missing ${name}`);
  return value;
}

function boundedInteger(argv: string[], name: string, min: number, max: number): number {
  const value = Number(valueAfter(argv, name));
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new Error(`${name} must be an integer from ${min} to ${max}`);
  }
  return value;
}

export function parseBatchUiOptions(argv: string[]): Options {
  const colors = valueAfter(argv, '--colors').split(',').map((value) => value.toUpperCase());
  if (colors.length !== 4 || colors.some((value) => !/^#[0-9A-F]{6}$/.test(value))) {
    throw new Error('--colors must contain exactly four comma-separated #RRGGBB values');
  }
  const objectId = valueAfter(argv, '--object');
  if (objectId.length > 128 || /[\u0000-\u001f]/.test(objectId)) {
    throw new Error('--object is not a valid scene ID');
  }
  const denoise = valueAfter(argv, '--denoise');
  if (denoise !== 'true' && denoise !== 'false') throw new Error('--denoise must be true or false');
  return {
    dir: path.resolve(valueAfter(argv, '--dir')),
    objectId,
    colors,
    width: boundedInteger(argv, '--width', 320, 1920),
    height: boundedInteger(argv, '--height', 180, 1080),
    samples: boundedInteger(argv, '--samples', 1, 256),
    denoise: denoise === 'true',
  };
}

async function main(): Promise<void> {
  const options = parseBatchUiOptions(process.argv.slice(2));
  assertNativeChatIdle(options.dir);
  const lock = acquireActionHostLock(options.dir);
  const protocol = new Protocol(options.dir, { actionOnly: true });
  const controller = new AbortController();
  const input = readline.createInterface({ input: process.stdin });
  input.on('line', (line) => { if (line.trim() === 'cancel') controller.abort(); });
  input.on('close', () => controller.abort());
  const context = { turnId: '', signal: controller.signal };
  try {
    const snapshot = renderLlmActionJson(await runRenderLlmAction(protocol, context, 'scene_get', {}));
    if (snapshot.success !== true) throw new Error(String(snapshot.error ?? 'could not read the open scene'));
    const revision = Number(snapshot.revision);
    const documentBefore = String(snapshot.document ?? '');
    if (!Number.isInteger(revision) || revision < 0) throw new Error('scene returned an invalid revision');
    const batch = await runColorBatchAction(protocol, context, {
      object_id: options.objectId,
      colors: options.colors,
      expected_revision: revision,
      width: options.width,
      height: options.height,
      samples: options.samples,
      denoise: options.denoise,
    });
    const after = renderLlmActionJson(await runRenderLlmAction(protocol, context, 'scene_get', {}));
    if (after.success !== true || Number(after.revision) !== revision || String(after.document ?? '') !== documentBefore) {
      throw new Error('Batch changed the open composition');
    }
    const result = { ...batch, open_revision: revision, open_document_unchanged: true };
    const resultDir = path.join(options.dir, 'batch-ui');
    fs.mkdirSync(resultDir, { recursive: true });
    fs.writeFileSync(path.join(resultDir, 'latest.json'), `${JSON.stringify(result, null, 2)}\n`, 'utf8');
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } catch (error) {
    if (controller.signal.aborted) process.stdout.write(`${JSON.stringify({ cancelled: true })}\n`);
    else throw error;
  } finally {
    input.close();
    protocol.close();
    releaseActionHostLock(lock);
  }
}

const isMain = process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
