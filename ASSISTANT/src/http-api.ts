import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {
  acquireActionHostLock,
  assertNativeChatIdle,
  releaseActionHostLock,
} from './action-host-lock.js';
import { createRenderLlmHttpServer } from './http-server.js';
import { Protocol } from './protocol.js';
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
  type ColorBatchActionArgs,
  type RenderLlmActionArgs,
  type RenderLlmActionName,
  type MediaJobArgs,
  type StillActionArgs,
  type VideoActionArgs,
} from './renderllm-actions.js';

function option(argv: string[], name: string): string | undefined {
  const index = argv.indexOf(name);
  const value = index >= 0 ? argv[index + 1] : undefined;
  return value && !value.startsWith('--') ? value : undefined;
}

function requiredDir(argv: string[]): string {
  const value = option(argv, '--dir');
  if (!value) throw new Error('Usage: node dist/http-api.js --dir <absolute Data/assistant directory> [--port 8765]');
  return path.resolve(value);
}

function requiredPort(argv: string[]): number {
  const value = option(argv, '--port') ?? '8765';
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('HTTP port must be an integer from 1 to 65535');
  return port;
}

function loadOrCreateToken(dir: string): { path: string; value: string } {
  const tokenPath = path.join(dir, 'renderllm-http.token');
  try {
    const value = fs.readFileSync(tokenPath, 'utf8').trim();
    if (Buffer.byteLength(value, 'utf8') < 32) throw new Error('existing HTTP token is shorter than 32 bytes');
    return { path: tokenPath, value };
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
  }

  const value = crypto.randomBytes(32).toString('base64url');
  fs.writeFileSync(tokenPath, `${value}\n`, { encoding: 'utf8', flag: 'wx', mode: 0o600 });
  return { path: tokenPath, value };
}

async function main(): Promise<void> {
  const argv = process.argv.slice(2);
  const dir = requiredDir(argv);
  const port = requiredPort(argv);
  fs.mkdirSync(dir, { recursive: true });
  assertNativeChatIdle(dir);
  const lock = acquireActionHostLock(dir);
  const token = loadOrCreateToken(dir);
  const protocol = new Protocol(dir, { actionOnly: true });
  const server = createRenderLlmHttpServer({
    token: token.value,
    execute: async (name, args, signal) => {
      if (name === 'color_batch') {
        return runColorBatchAction(protocol, { turnId: '', signal }, args as ColorBatchActionArgs);
      }
      if (name === 'batch_start') {
        return startColorBatchAction(protocol, { turnId: '', signal }, args as ColorBatchActionArgs);
      }
      if (name === 'batch_status') {
        return statusColorBatchAction(protocol, args as MediaJobArgs);
      }
      if (name === 'batch_cancel') {
        return cancelColorBatchAction(protocol, args as MediaJobArgs);
      }
      if (name === 'render_image') {
        return runStillAction(protocol, { turnId: '', signal }, args as StillActionArgs);
      }
      if (name === 'render_start') {
        return startStillAction(protocol, { turnId: '', signal }, args as StillActionArgs);
      }
      if (name === 'render_status') {
        return statusStillAction(protocol, args as MediaJobArgs);
      }
      if (name === 'render_cancel') {
        return cancelStillAction(protocol, args as MediaJobArgs);
      }
      if (name === 'video_export') {
        return runVideoAction(protocol, { turnId: '', signal }, args as VideoActionArgs);
      }
      if (name === 'video_start') {
        return startVideoAction(protocol, { turnId: '', signal }, args as VideoActionArgs);
      }
      if (name === 'video_status') {
        return statusVideoAction(protocol, args as MediaJobArgs);
      }
      if (name === 'video_cancel') {
        return cancelVideoAction(protocol, args as MediaJobArgs);
      }
      return renderLlmActionJson(await runRenderLlmAction(
        protocol,
        { turnId: '', signal },
        name as RenderLlmActionName,
        args as RenderLlmActionArgs[RenderLlmActionName],
      ));
    },
  });
  let closed = false;

  const close = async (): Promise<void> => {
    if (closed) return;
    closed = true;
    protocol.close();
    await new Promise<void>((resolve) => server.close(() => resolve()));
    releaseActionHostLock(lock);
  };

  const shutdown = (): void => {
    void close().then(
      () => process.exit(0),
      (error) => {
        process.stderr.write(`render-llm HTTP API shutdown failed: ${error instanceof Error ? error.message : String(error)}\n`);
        process.exit(1);
      },
    );
  };

  process.once('exit', () => releaseActionHostLock(lock));
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
  if (argv.includes('--shutdown-on-stdin-close')) {
    process.stdin.resume();
    process.stdin.once('end', shutdown);
  }

  try {
    await new Promise<void>((resolve, reject) => {
      server.once('error', reject);
      server.listen(port, '127.0.0.1', () => resolve());
    });
    process.stdout.write(`render-llm HTTP API listening on http://127.0.0.1:${port}; bearer token file: ${token.path}\n`);
  } catch (error) {
    await close();
    throw error;
  }
}

main().catch((error) => {
  process.stderr.write(`render-llm HTTP API failed: ${error instanceof Error ? error.message : String(error)}\n`);
  process.exitCode = 1;
});
