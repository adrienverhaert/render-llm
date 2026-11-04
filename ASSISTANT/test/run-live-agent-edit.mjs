import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';

function option(name) {
  const at = process.argv.indexOf(name);
  return at >= 0 ? process.argv[at + 1] : undefined;
}

const exe = path.resolve(option('--exe') ?? '');
const seedPath = path.resolve(option('--seed') ?? '');
const evidencePath = path.resolve(option('--evidence') ?? '');
const model = option('--model') ?? 'gpt-5.6-luna';
for (const file of [exe, seedPath]) {
  if (!fs.statSync(file).isFile()) throw new Error(`Missing required file: ${file}`);
}

const releaseDir = path.dirname(exe);
const dataDir = path.join(releaseDir, 'Data', 'assistant');
const mcpHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'mcp-stdio.js');
const agentHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'index.js');
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitUntil(read, timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() <= end) {
    const value = read();
    if (value) return value;
    await sleep(100);
  }
  return undefined;
}

async function stopOwned(child) {
  if (!child || child.exitCode !== null) return;
  child.kill();
  await Promise.race([new Promise((resolve) => child.once('exit', resolve)), sleep(5000)]);
  if (child.exitCode === null) child.kill('SIGKILL');
}

function records(file) {
  try {
    return fs.readFileSync(file, 'utf8').split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
  } catch (error) {
    if (error?.code === 'ENOENT') return [];
    throw error;
  }
}

function brain(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return undefined;
  }
}

function value(result) {
  const v = result.structuredContent;
  if (!v || v.success !== true || v.ok !== 1) throw new Error(`Native action failed: ${JSON.stringify(v ?? result)}`);
  return v;
}

async function connectMcp(name) {
  const transport = new StdioClientTransport({ command: process.execPath, args: [mcpHost, '--dir', dataDir], stderr: 'pipe' });
  const client = new Client({ name, version: '0.1.0' });
  await client.connect(transport);
  return client;
}

fs.mkdirSync(path.dirname(evidencePath), { recursive: true });
const editor = spawn(exe, [], { cwd: releaseDir, windowsHide: true, stdio: 'ignore' });
let agent;
let client;

try {
  if (!await waitUntil(() => fs.existsSync(dataDir), 30000)) throw new Error('Native editor did not create Data/assistant');
  await sleep(3000);
  client = await connectMcp('render-llm-live-agent-seed');
  const initial = value(await client.callTool({ name: 'scene_get', arguments: {} }));
  const seed = fs.readFileSync(seedPath, 'utf8');
  value(await client.callTool({ name: 'scene_validate', arguments: { document: seed } }));
  value(await client.callTool({ name: 'scene_apply', arguments: { document: seed, expected_revision: Number(initial.revision) } }));
  const baseline = value(await client.callTool({ name: 'scene_get', arguments: {} }));
  const baselineDocument = String(baseline.document);
  const parsed = JSON.parse(baselineDocument);
  const materialId = Object.keys(parsed.materials ?? {})[0];
  if (!materialId) throw new Error('Seed has no material');
  const originalColor = String(parsed.materials[materialId].color);
  await client.close();
  client = undefined;

  const logPath = path.join(dataDir, 'assistant.log');
  const brainPath = path.join(dataDir, 'brain.json');
  const beforeLog = fs.existsSync(logPath) ? fs.statSync(logPath).size : 0;
  agent = spawn(process.execPath, [agentHost, '--dir', dataDir, '--model', model, '--effort', 'low'], {
    cwd: path.join(releaseDir, 'ASSISTANT'), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stderr = '';
  agent.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
  if (!await waitUntil(() => fs.existsSync(logPath) && fs.statSync(logPath).size > beforeLog, 10000)) {
    throw new Error(`Packaged Agent did not start: ${stderr.slice(0, 500)}`);
  }

  let seq = 0;
  const send = (record) => {
    fs.appendFileSync(path.join(dataDir, 'chat_in.jsonl'), JSON.stringify({ seq: ++seq, t: Date.now(), ...record }) + '\n');
    fs.writeFileSync(path.join(dataDir, 'chat_in.flag'), '');
  };
  send({ type: 'hello', editor_version: 'native-live-agent-edit', project: '' });
  send({ type: 'chat', action: 'new', chat_id: `live_edit_${Date.now()}` });
  if (!await waitUntil(() => ['ready', 'idle'].includes(String(brain(brainPath)?.state)), 20000)) {
    throw new Error('Packaged Agent did not become ready');
  }

  const turnId = `live-edit-${crypto.randomUUID()}`;
  const prompt = `Use only the render-llm actions. Read the scene, change material ${materialId} from ${originalColor} to #00FFFF with revision validation, read it back to verify, then undo and read again to verify the exact original color is restored. Do not render. Reply only after all steps.`;
  send({ type: 'user', turn_id: turnId, text: prompt, project_file: '' });
  const completedLog = await waitUntil(() => {
    if (!fs.existsSync(logPath)) return undefined;
    const tail = fs.readFileSync(logPath, 'utf8').slice(beforeLog);
    if (tail.includes(`ERROR en turno ${turnId}:`)) return { error: tail };
    return tail.includes('[codex usage]') && brain(brainPath)?.state === 'idle' ? { completed: tail } : undefined;
  }, 180000);
  if (!completedLog) throw new Error(`Live edit turn timed out: ${stderr.slice(0, 500)}`);
  if (completedLog.error) throw new Error(`Live edit turn failed: ${completedLog.error.slice(-1000)}`);
  const endedTools = [...completedLog.completed.matchAll(/tool result ([a-z_]+) ok=true:/g)].map((match) => match[1]);
  for (const required of ['scene_get', 'scene_validate', 'scene_apply', 'undo']) {
    if (!endedTools.includes(required)) throw new Error(`Model did not complete ${required}: ${JSON.stringify(endedTools)}`);
  }

  await stopOwned(agent);
  agent = undefined;
  client = await connectMcp('render-llm-live-agent-verify');
  const restored = value(await client.callTool({ name: 'scene_get', arguments: {} }));
  if (String(restored.document) !== baselineDocument) throw new Error('Agent undo did not restore exact original document bytes');
  if (Number(restored.revision) <= Number(baseline.revision)) throw new Error('Agent actions did not advance the native revision');
  const log = fs.readFileSync(logPath, 'utf8');
  const usageMatch = [...log.matchAll(/\[codex usage\] (\{[^\n]+\})/g)].at(-1);
  const result = {
    status: 'packaged_chatgpt_edit_observe_undo_passed',
    requested_user_turns: 1,
    model,
    effort: 'low',
    turn_id: turnId,
    prompt,
    answer: null,
    answer_capture: 'consumed by the running native editor; completion was observed through usage, idle heartbeat and action results',
    material_id: materialId,
    original_color: originalColor,
    temporary_color: '#00FFFF',
    revisions: { baseline: baseline.revision, restored: restored.revision },
    completed_tools: endedTools,
    exact_document_restored: true,
    usage: usageMatch ? JSON.parse(usageMatch[1]) : null,
    completion: { usage_logged: true, brain_state: brain(brainPath)?.state },
  };
  fs.writeFileSync(evidencePath, JSON.stringify(result, null, 2) + '\n');
  process.stdout.write(JSON.stringify(result, null, 2) + '\n');
} finally {
  if (client) await client.close().catch(() => {});
  await stopOwned(agent);
  await stopOwned(editor);
}
