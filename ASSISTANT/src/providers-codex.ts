import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import type { SDKMessage } from '@anthropic-ai/claude-agent-sdk';
import type { ProviderMessage, ProviderQueryArgs } from './providers.js';
import { RENDER_LLM_SHORT_NAMES } from './renderllm-tools.js';

type JsonRecord = Record<string, unknown>;

const DISABLED_FEATURES = [
  'apps',
  'browser_use',
  'computer_use',
  'image_generation',
  'multi_agent',
  'plugins',
  'shell_tool',
  'skill_search',
  'sleep_tool',
  'unified_exec',
  'view_image',
  'workspace_dependencies',
] as const;
const ALLOWED_MCP_TOOLS = new Set<string>(RENDER_LLM_SHORT_NAMES);
const ENABLED_MCP_TOOLS_TOML = `[${RENDER_LLM_SHORT_NAMES.map(tomlString).join(',')}]`;
const FORBIDDEN_ITEM_TYPES = new Set(['command_execution', 'file_change', 'web_search', 'computer_use']);
const HOST_CONTEXT_ENV = [
  'CODEX_APP_TOOLS_PIPE_PATH', 'CODEX_MCP_NODE_PATH', 'CODEX_PERMISSION_PROFILE',
  'CODEX_SESSION_ID', 'CODEX_THREAD_ID', 'CODEX_SAGE_BACKFILL_TRACKER_TAB_REUSE',
  'CODEX_INTERNAL_ORIGINATOR_OVERRIDE',
] as const;

function tomlString(value: string): string {
  return JSON.stringify(value.replace(/\\/g, '/'));
}

function textOf(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function standaloneCodexEnv(): NodeJS.ProcessEnv {
  const env = { ...process.env };
  for (const name of HOST_CONTEXT_ENV) delete env[name];
  return env;
}

function renderLlmToolName(item: JsonRecord): string {
  const server = textOf(item.server);
  if (server && server !== 'render-llm') throw new Error(`Codex attempted an unconfigured MCP server: ${server} (tool ${textOf(item.tool) || textOf(item.name) || 'missing'})`);
  const raw = textOf(item.tool) || textOf(item.name);
  const name = raw.replace(/^mcp__render-llm__/, '');
  if (!ALLOWED_MCP_TOOLS.has(name)) throw new Error(`Codex attempted an unavailable render-llm action: ${raw || '(missing)'}`);
  return name;
}

function isCodeModeDispatch(item: JsonRecord): boolean {
  // Codex CLI represents its built-in exec/wait carrier as a synthetic MCP item.
  // It is not a scene action or an additional external server. The actual server
  // stays confined by enabled_tools; shell/computer/network tool surfaces remain off.
  return item.server === 'codex' && (item.tool === 'exec' || item.tool === 'wait');
}

function instructionsForTurn(systemPrompt: unknown, isResume: boolean, userPrompt: string): string {
  const full = textOf(systemPrompt);
  if (!isResume) return full;
  if (userPrompt.startsWith('Continue. You hit the per-leg turn limit')) return '';
  const marker = '\n\n---\n\n';
  const memoryAt = full.lastIndexOf(marker);
  return memoryAt >= 0 ? full.slice(memoryAt + marker.length) : '';
}

async function materializePrompt(
  prompt: ProviderQueryArgs['prompt'],
  cwd: string,
  turnId: string,
): Promise<{ text: string; images: string[]; cleanup: () => void }> {
  if (typeof prompt === 'string') return { text: prompt, images: [], cleanup: () => {} };

  const imageDir = path.join(cwd, 'attachments', `.codex-${turnId.replace(/[^A-Za-z0-9_-]/g, '_')}-${process.pid}`);
  const images: string[] = [];
  const texts: string[] = [];
  let imageN = 0;

  for await (const message of prompt) {
    const content = message.message.content;
    if (!Array.isArray(content)) continue;
    for (const block of content) {
      if (block.type === 'text') {
        texts.push(block.text);
      } else if (block.type === 'image' && block.source.type === 'base64') {
        fs.mkdirSync(imageDir, { recursive: true });
        const extension = block.source.media_type === 'image/jpeg' ? 'jpg' : 'png';
        const imagePath = path.join(imageDir, `input-${++imageN}.${extension}`);
        fs.writeFileSync(imagePath, Buffer.from(block.source.data, 'base64'));
        images.push(imagePath);
      }
    }
  }

  return {
    text: texts.join('\n'),
    images,
    cleanup: () => {
      try {
        fs.rmSync(imageDir, { recursive: true, force: true });
      } catch {
        // A failed best-effort cleanup cannot turn a completed model turn into an error.
      }
    },
  };
}

function assistantMessage(text: string): SDKMessage {
  return {
    type: 'assistant',
    uuid: crypto.randomUUID(),
    session_id: '',
    parent_tool_use_id: null,
    message: {
      id: crypto.randomUUID(),
      type: 'message',
      role: 'assistant',
      content: [{ type: 'text', text }],
      model: '',
      stop_reason: 'end_turn',
      stop_sequence: null,
      usage: { input_tokens: 0, output_tokens: 0 },
    },
  } as SDKMessage;
}

/**
 * ChatGPT-backed Codex CLI adapter. Codex owns authentication; render-llm adds
 * exactly one stdio MCP server and disables its general computer/shell surfaces.
 * JSONL is normalized to the small SDK event subset consumed by index.ts.
 */
export async function* codexQuery(args: ProviderQueryArgs): AsyncIterable<ProviderMessage> {
  const options = args.options;
  const cwd = path.resolve(options.cwd ?? process.cwd());
  const turnId = options.renderLlmTurnId ?? 'turn';
  const prompt = await materializePrompt(args.prompt, cwd, turnId);
  const started = Date.now();
  const codexExe = process.env.CODEX_CLI_PATH?.trim() || 'codex';
  const testDriver = process.env.RENDER_LLM_CODEX_TEST_DRIVER?.trim();
  const mcpHost = path.join(path.dirname(fileURLToPath(import.meta.url)), 'codex-mcp-stdio.js');

  const common = [
    '--ignore-user-config',
    '--ignore-rules',
    '--strict-config',
    '--json',
    '--skip-git-repo-check',
    '-m',
    options.model ?? 'gpt-5.6-luna',
    '-c',
    `model_reasoning_effort=${tomlString(options.effort ?? 'medium')}`,
    '--enable',
    'skip_host_skill_discovery',
    '-c',
    'mcp_servers={}',
    '-c',
    `mcp_servers.render-llm.command=${tomlString(process.execPath)}`,
    '-c',
    `mcp_servers.render-llm.args=[${[mcpHost, '--dir', cwd, '--turn-id', turnId].map(tomlString).join(',')}]`,
    '-c',
    `mcp_servers.render-llm.enabled_tools=${ENABLED_MCP_TOOLS_TOML}`,
    '-c',
    'mcp_servers.render-llm.default_tools_approval_mode="approve"',
    '-c',
    'mcp_servers.render-llm.required=true',
    '-c',
    'mcp_servers.render-llm.tool_timeout_sec=1800',
    '-c',
    'sandbox_mode="read-only"',
    '-c',
    `model_instructions_file=${tomlString(path.join(path.dirname(fileURLToPath(import.meta.url)), '../prompts/render-llm-role.md'))}`,
  ];
  for (const feature of DISABLED_FEATURES) common.push('--disable', feature);

  const isResume = typeof options.resume === 'string' && options.resume.length > 0;
  const cliArgs = isResume
    ? ['exec', 'resume', ...common, options.resume as string]
    : ['exec', ...common, '-C', cwd];
  for (const image of prompt.images) cliArgs.push('-i', image);
  cliArgs.push('-');

  const instructions = instructionsForTurn(options.systemPrompt, isResume, prompt.text);
  const role = instructions ? `${instructions}\n\n---\n\n` : '';
  const child = spawn(testDriver ? process.execPath : codexExe, testDriver ? [testDriver, ...cliArgs] : cliArgs, {
    cwd,
    env: standaloneCodexEnv(),
    windowsHide: true,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const exit = new Promise<number | null>((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  const abort = () => child.kill();
  options.abortController?.signal.addEventListener('abort', abort, { once: true });
  child.stdin.end(role + prompt.text);

  let stdout = '';
  let stderr = '';
  let sessionId = '';
  let answer = '';
  let usage: JsonRecord = {};
  const toolStarted = new Map<string, number>();
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
    options.stderr?.(chunk);
  });

  try {
    for await (const chunk of child.stdout) {
      stdout += String(chunk);
      for (;;) {
        const newline = stdout.indexOf('\n');
        if (newline < 0) break;
        const line = stdout.slice(0, newline).trim();
        stdout = stdout.slice(newline + 1);
        if (!line) continue;
        let event: JsonRecord;
        try {
          event = JSON.parse(line) as JsonRecord;
        } catch {
          throw new Error(`Codex emitted invalid JSONL: ${line.slice(0, 300)}`);
        }
        if (event.type === 'thread.started') {
          sessionId = textOf(event.thread_id);
          yield {
            type: 'system',
            subtype: 'init',
            session_id: sessionId,
            tools: ['mcp__render-llm__*'],
          } as SDKMessage;
        } else if (event.type === 'item.started') {
          const item = event.item as JsonRecord | undefined;
          if (item?.type === 'mcp_tool_call') {
            if (isCodeModeDispatch(item)) continue;
            const name = renderLlmToolName(item);
            const id = textOf(item.id) || name;
            toolStarted.set(id, Date.now());
            yield { type: 'render_llm_tool', name, phase: 'start' };
          } else if (item && FORBIDDEN_ITEM_TYPES.has(textOf(item.type))) {
            throw new Error(`Codex attempted a disabled tool surface: ${textOf(item.type)}`);
          }
        } else if (event.type === 'item.completed') {
          const item = event.item as JsonRecord | undefined;
          if (item?.type === 'agent_message') {
            answer += textOf(item.text);
          } else if (item?.type === 'mcp_tool_call') {
            if (isCodeModeDispatch(item)) continue;
            const name = renderLlmToolName(item);
            const id = textOf(item.id) || name;
            const status = textOf(item.status);
            yield {
              type: 'render_llm_tool',
              name,
              phase: 'end',
              ok: status !== 'failed' && !item.error,
              ms: Math.max(0, Date.now() - (toolStarted.get(id) ?? Date.now())),
            };
          } else if (item && FORBIDDEN_ITEM_TYPES.has(textOf(item.type))) {
            throw new Error(`Codex attempted a disabled tool surface: ${textOf(item.type)}`);
          }
        } else if (event.type === 'turn.completed') {
          usage = (event.usage as JsonRecord | undefined) ?? {};
        } else if (event.type === 'turn.failed' || event.type === 'error') {
          const nested = event.error as JsonRecord | undefined;
          throw new Error(textOf(event.message) || textOf(nested?.message) || 'Codex turn failed');
        }
      }
    }

    const exitCode = await exit;
    if (exitCode !== 0) throw new Error(stderr.trim() || `Codex exited with code ${exitCode ?? 'unknown'}`);
    if (answer) yield assistantMessage(answer);
    options.stderr?.(`[codex usage] ${JSON.stringify(usage)}\n`);
    yield {
      type: 'result',
      subtype: 'success',
      session_id: sessionId,
      stop_reason: 'end_turn',
      terminal_reason: 'end_turn',
      duration_ms: Date.now() - started,
      duration_api_ms: Date.now() - started,
      is_error: false,
      num_turns: 1,
      result: answer,
      usage: { input_tokens: 0, output_tokens: 0 },
      modelUsage: {},
      permission_denials: [],
      uuid: crypto.randomUUID(),
    } as unknown as SDKMessage;
  } finally {
    options.abortController?.signal.removeEventListener('abort', abort);
    prompt.cleanup();
  }
}
