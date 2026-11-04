import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';

function option(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const exe = path.resolve(option('--exe') ?? '');
const seedPath = path.resolve(option('--seed') ?? '');
const evidenceDir = path.resolve(option('--evidence') ?? '');
const model = option('--model') ?? 'gpt-5.6-luna';
const resultName = option('--result-name') ?? 'result.json';
if (!fs.statSync(exe).isFile() || !fs.statSync(seedPath).isFile() || !evidenceDir) {
  throw new Error('Usage: node test/run-live-visual-agent.mjs --exe <render-llm.exe> --seed <scene.renderllm> --evidence <dir> [--model <id>]');
}

const releaseDir = path.dirname(exe);
const dataDir = path.join(releaseDir, 'Data', 'assistant');
const mcpHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'mcp-stdio.js');
const agentHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'index.js');
for (const required of [mcpHost, agentHost]) {
  if (!fs.statSync(required).isFile()) throw new Error(`Missing packaged Agent file: ${required}`);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function waitUntil(fn, timeoutMs, stepMs = 100) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() <= deadline) {
    const value = await fn();
    if (value) return value;
    await sleep(stepMs);
  }
  return undefined;
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').toUpperCase();
}

function pngDimensions(file) {
  const png = fs.readFileSync(file);
  if (png.length < 24 || png.subarray(1, 4).toString('ascii') !== 'PNG') throw new Error(`Invalid PNG: ${file}`);
  return { width: png.readUInt32BE(16), height: png.readUInt32BE(20) };
}

function structured(result) {
  const value = result.structuredContent;
  if (!value || typeof value !== 'object' || value.success !== true || value.ok !== 1) {
    throw new Error(`Native action failed: ${JSON.stringify(value ?? result)}`);
  }
  return value;
}

function readRecords(file) {
  try {
    return fs.readFileSync(file, 'utf8').split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
  } catch (error) {
    if (error?.code === 'ENOENT') return [];
    throw error;
  }
}

async function stopOwned(child) {
  if (!child || child.exitCode !== null) return;
  child.kill();
  await Promise.race([
    new Promise((resolve) => child.once('exit', resolve)),
    sleep(5000),
  ]);
  if (child.exitCode === null) child.kill('SIGKILL');
}

fs.mkdirSync(evidenceDir, { recursive: true });
const editor = spawn(exe, [], { cwd: releaseDir, windowsHide: true, stdio: 'ignore' });
let agent;
let client;
let transport;

try {
  const dataReady = await waitUntil(() => fs.existsSync(dataDir), 30000);
  if (!dataReady || editor.exitCode !== null) throw new Error(`Native editor did not initialize (exit=${editor.exitCode})`);
  // Data/assistant can survive a previous run, so its existence is not a
  // readiness signal. Give the native event loop time to install its ops
  // consumer before the action-only MCP bridge sends seq=1.
  await sleep(3000);
  if (editor.exitCode !== null) throw new Error(`Native editor exited during startup (exit=${editor.exitCode})`);

  transport = new StdioClientTransport({
    command: process.execPath,
    args: [mcpHost, '--dir', dataDir],
    stderr: 'pipe',
  });
  client = new Client({ name: 'render-llm-live-visual-evidence', version: '0.1.0' });
  await client.connect(transport);

  const initial = structured(await client.callTool({ name: 'scene_get', arguments: {} }));
  const seed = fs.readFileSync(seedPath, 'utf8');
  structured(await client.callTool({ name: 'scene_validate', arguments: { document: seed } }));
  const seeded = structured(await client.callTool({
    name: 'scene_apply',
    arguments: { document: seed, expected_revision: Number(initial.revision) },
  }));
  const baseline = structured(await client.callTool({
    name: 'render_image',
    arguments: { expected_revision: Number(seeded.revision), width: 384, height: 216, samples: 4, denoise: true },
  }));

  const changedDocument = JSON.parse(seed);
  const materialId = Object.keys(changedDocument.materials ?? {})[0];
  if (!materialId) throw new Error('Seed has no material to change');
  const oldColor = changedDocument.materials[materialId].color;
  const newColor = '#FF00FF';
  changedDocument.materials[materialId].color = newColor;
  const changedText = JSON.stringify(changedDocument);
  structured(await client.callTool({ name: 'scene_validate', arguments: { document: changedText } }));
  const changed = structured(await client.callTool({
    name: 'scene_apply',
    arguments: { document: changedText, expected_revision: Number(seeded.revision) },
  }));
  const changedRender = structured(await client.callTool({
    name: 'render_image',
    arguments: { expected_revision: Number(changed.revision), width: 384, height: 216, samples: 4, denoise: true },
  }));

  const baselineSource = path.resolve(dataDir, String(baseline.image));
  const changedSource = path.resolve(dataDir, String(changedRender.image));
  const baselinePng = path.join(evidenceDir, 'baseline-gray.png');
  const changedPng = path.join(evidenceDir, 'changed-magenta.png');
  fs.copyFileSync(baselineSource, baselinePng);
  fs.copyFileSync(changedSource, changedPng);
  const baselineHash = sha256(baselinePng);
  const changedHash = sha256(changedPng);
  if (baselineHash === changedHash) throw new Error('Color edit did not change rendered PNG bytes');
  const dimensions = pngDimensions(changedPng);
  if (dimensions.width !== 384 || dimensions.height !== 216) throw new Error(`Unexpected changed render dimensions: ${JSON.stringify(dimensions)}`);

  await client.close();
  client = undefined;
  await stopOwned(editor);

  const turnId = `live-visual-${randomUUID()}`;
  const attachmentDir = path.join(dataDir, 'attachments', turnId);
  fs.mkdirSync(attachmentDir, { recursive: true });
  fs.copyFileSync(changedPng, path.join(attachmentDir, 'changed-render.png'));

  const logPath = path.join(dataDir, 'assistant.log');
  const logBytesBefore = fs.existsSync(logPath) ? fs.statSync(logPath).size : 0;
  agent = spawn(process.execPath, [agentHost, '--dir', dataDir, '--model', model, '--effort', 'low'], {
    cwd: path.join(releaseDir, 'ASSISTANT'),
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  agent.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
  agent.stderr.on('data', (chunk) => { stderr += chunk.toString(); });

  const chatOut = path.join(dataDir, 'chat_out.jsonl');
  let seq = 0;
  const writeIn = (record) => {
    const full = { seq: ++seq, t: Date.now(), ...record };
    fs.appendFileSync(path.join(dataDir, 'chat_in.jsonl'), JSON.stringify(full) + '\n', 'utf8');
    fs.writeFileSync(path.join(dataDir, 'chat_in.flag'), '', 'utf8');
  };
  const chatId = `live_visual_${Date.now()}`;
  const protocolStarted = await waitUntil(() => fs.existsSync(logPath) && fs.statSync(logPath).size > logBytesBefore, 10000);
  if (!protocolStarted || agent.exitCode !== null) throw new Error(`Packaged Agent protocol did not start (exit=${agent.exitCode}; stdout=${stdout.slice(0, 300)}; stderr=${stderr.slice(0, 300)})`);
  writeIn({ type: 'hello', editor_version: 'native-live-evidence', project: '' });
  writeIn({ type: 'chat', action: 'new', chat_id: chatId });
  const ready = await waitUntil(() => readRecords(chatOut).some((entry) => entry.type === 'status' && entry.state === 'ready'), 20000);
  if (!ready || agent.exitCode !== null) throw new Error(`Packaged Agent did not become ready (exit=${agent.exitCode}; stdout=${stdout.slice(0, 300)}; stderr=${stderr.slice(0, 300)})`);
  writeIn({
    type: 'user',
    turn_id: turnId,
    text: 'Inspect the attached native Flame render. Reply with only the dominant visible object color in plain English. Do not call tools.',
    project_file: '',
    attachments: 'changed-render.png',
  });

  const done = await waitUntil(() => readRecords(chatOut).find((entry) => entry.type === 'done' && entry.turn_id === turnId), 180000);
  if (!done) throw new Error(`Real provider turn did not finish (stdout=${stdout.slice(0, 500)}; stderr=${stderr.slice(0, 500)})`);
  const events = readRecords(chatOut).filter((entry) => entry.turn_id === turnId);
  const answer = events.filter((entry) => entry.type === 'token' || entry.type === 'text').map((entry) => entry.text).join('').trim();
  const tools = [...new Set(events.filter((entry) => entry.type === 'tool').map((entry) => entry.name))];
  if (!answer) throw new Error('Real model returned no text');
  if (tools.length !== 0) throw new Error(`Model unexpectedly called tools: ${tools.join(', ')}`);
  if (!/(magenta|pink|purple|violet)/i.test(answer)) throw new Error(`Model did not identify the changed object color: ${JSON.stringify(answer)}`);

  const logText = fs.readFileSync(logPath, 'utf8');
  const logLines = logText.split(/\r?\n/).filter((line) => line.includes(turnId) || line.includes('RONDA G:') || line.includes('tools de la sesion'));
  const result = {
    requested_user_turns: 1,
    model,
    effort: 'low',
    turn_id: turnId,
    prompt: 'Inspect the attached native Flame render. Reply with only the dominant visible object color in plain English. Do not call tools.',
    answer,
    done,
    tools_called: tools,
    scene: {
      material_id: materialId,
      old_color: oldColor,
      new_color: newColor,
      seeded_revision: seeded.revision,
      changed_revision: changed.revision,
    },
    baseline_render: {
      path: baselinePng,
      sha256: baselineHash,
      backend: baseline.backend,
      denoiser: baseline.denoiser,
    },
    changed_render: {
      path: changedPng,
      sha256: changedHash,
      backend: changedRender.backend,
      denoiser: changedRender.denoiser,
      ...dimensions,
    },
    attachment_bytes: fs.statSync(path.join(attachmentDir, 'changed-render.png')).size,
    diagnostic_excerpt: logLines.slice(-12),
  };
  fs.writeFileSync(path.join(evidenceDir, resultName), JSON.stringify(result, null, 2) + '\n', 'utf8');
  process.stdout.write(JSON.stringify(result, null, 2) + '\n');
} finally {
  if (client) await client.close().catch(() => {});
  await stopOwned(agent);
  await stopOwned(editor);
}
