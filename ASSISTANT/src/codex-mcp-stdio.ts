import path from 'node:path';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { Protocol } from './protocol.js';
import { buildRenderLlmMcp, EXTERNAL_ACTION_TURN_ID } from './renderllm-tools.js';

function option(name: string): string {
  const index = process.argv.indexOf(name);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (!value || value.startsWith('--')) throw new Error(`Missing ${name}`);
  return value;
}

async function main(): Promise<void> {
  const dir = path.resolve(option('--dir'));
  option('--turn-id');
  const protocol = new Protocol(dir, { actionOnly: true });
  // The native editor owns non-empty chat turn IDs. Nested/external MCP hosts
  // must use the protocol's empty turn ID or the editor correctly rejects the
  // request as belonging to a cancelled native-chat turn.
  const server = buildRenderLlmMcp(protocol, { turnId: EXTERNAL_ACTION_TURN_ID });
  const transport = new StdioServerTransport();
  try {
    await server.instance.connect(transport);
  } finally {
    protocol.close();
  }
}

main().catch((error) => {
  process.stderr.write(`render-llm Codex MCP host failed: ${error instanceof Error ? error.message : String(error)}\n`);
  process.exitCode = 1;
});
