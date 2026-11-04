import path from 'node:path';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { Protocol } from './protocol.js';
import { buildRenderLlmMcp, EXTERNAL_ACTION_TURN_ID } from './renderllm-tools.js';
import {
  acquireActionHostLock,
  assertNativeChatIdle,
  releaseActionHostLock,
} from './action-host-lock.js';

function requiredDir(argv: string[]): string {
  const index = argv.indexOf('--dir');
  const value = index >= 0 ? argv[index + 1] : undefined;
  if (!value || value.startsWith('--')) {
    throw new Error('Usage: node dist/mcp-stdio.js --dir <absolute Data/assistant directory>');
  }
  return path.resolve(value);
}

async function main(): Promise<void> {
  const dir = requiredDir(process.argv.slice(2));
  assertNativeChatIdle(dir);
  const lock = acquireActionHostLock(dir);
  const protocol = new Protocol(dir, { actionOnly: true });
  const server = buildRenderLlmMcp(protocol, { turnId: EXTERNAL_ACTION_TURN_ID });
  const transport = new StdioServerTransport();
  let closed = false;

  const close = async (): Promise<void> => {
    if (closed) return;
    closed = true;
    protocol.close();
    releaseActionHostLock(lock);
    await server.instance.close();
  };

  process.once('exit', () => releaseActionHostLock(lock));
  process.once('SIGINT', () => { void close(); });
  process.once('SIGTERM', () => { void close(); });

  try {
    await server.instance.connect(transport);
  } catch (error) {
    await close();
    throw error;
  }
}

main().catch((error) => {
  process.stderr.write(`render-llm MCP host failed: ${error instanceof Error ? error.message : String(error)}\n`);
  process.exitCode = 1;
});
