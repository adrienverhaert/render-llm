import fs from 'node:fs';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import { Protocol } from '../dist/protocol.js';

function option(name) {
  const at = process.argv.indexOf(name);
  return at >= 0 ? process.argv[at + 1] : undefined;
}

const exe = path.resolve(option('--exe') ?? '');
const outputDir = path.resolve(option('--output-dir') ?? '');
const model = option('--model') ?? 'gpt-5.6-luna';
const prompt = 'Un video de una esfera roja rebotando sobre un pedestal. Habitacion blanca infinita con luz de estudio. Camara siguiendo la esfera.';
if (!fs.statSync(exe).isFile()) throw new Error(`Missing executable: ${exe}`);
fs.mkdirSync(outputDir, { recursive: true });

const releaseDir = path.dirname(exe);
const dataDir = path.join(releaseDir, 'Data', 'assistant');
const agentHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'index.js');
const mcpHost = path.join(releaseDir, 'ASSISTANT', 'dist', 'mcp-stdio.js');
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitUntil(read, timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() <= end) {
    const value = read();
    if (value) return value;
    await sleep(200);
  }
  return undefined;
}

async function stopOwned(child) {
  if (!child || child.exitCode !== null) return;
  child.kill();
  await Promise.race([new Promise((resolve) => child.once('exit', resolve)), sleep(5000)]);
  if (child.exitCode === null) child.kill('SIGKILL');
}

function readJson(file) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return undefined; }
}

function value(result) {
  const v = result.structuredContent;
  if (!v || v.success !== true || v.ok !== 1) throw new Error(`Native action failed: ${JSON.stringify(v ?? result)}`);
  return v;
}

async function connectMcp() {
  const transport = new StdioClientTransport({ command: process.execPath, args: [mcpHost, '--dir', dataDir], stderr: 'pipe' });
  const client = new Client({ name: 'render-llm-agent-video-verifier', version: '0.1.0' });
  await client.connect(transport);
  return client;
}

function isRed(hex) {
  const match = /^#([0-9A-F]{2})([0-9A-F]{2})([0-9A-F]{2})$/i.exec(String(hex));
  if (!match) return false;
  const [r, g, b] = match.slice(1).map((part) => Number.parseInt(part, 16));
  return r >= 160 && r >= g * 1.5 && r >= b * 1.5;
}

function visibleRedPixels(videoPath, width, height, frames) {
  const selected = frames.map((frame) => `eq(n\\,${frame})`).join('+');
  const decoded = spawnSync('ffmpeg', [
    '-v', 'error', '-i', videoPath, '-vf', `select=${selected},format=rgb24`,
    '-vsync', '0', '-f', 'rawvideo', '-',
  ], { encoding: null, maxBuffer: width * height * 3 * (frames.length + 1) });
  if (decoded.status !== 0) throw new Error(`Could not decode sampled video pixels: ${decoded.stderr?.toString().slice(0, 500)}`);
  const frameBytes = width * height * 3;
  if (decoded.stdout.length !== frameBytes * frames.length) {
    throw new Error(`Expected ${frames.length} sampled frames, got ${decoded.stdout.length / frameBytes}`);
  }
  return frames.map((frame, frameIndex) => {
    let red = 0;
    const start = frameIndex * frameBytes;
    for (let offset = start; offset < start + frameBytes; offset += 3) {
      const r = decoded.stdout[offset];
      const g = decoded.stdout[offset + 1];
      const b = decoded.stdout[offset + 2];
      if (r >= 120 && r >= g * 1.35 && r >= b * 1.35) red += 1;
    }
    return { frame, red_pixels: red };
  });
}

const editor = spawn(exe, [], { cwd: releaseDir, windowsHide: true, stdio: 'ignore' });
let agent;
let client;
try {
  if (!await waitUntil(() => fs.existsSync(dataDir), 30000)) throw new Error('Native editor did not create Data/assistant');
  await sleep(3000);
  const reset = new Protocol(dataDir);
  try {
    const initialized = await reset.runOps([{ id: 'new', op: 'project.new', args: {} }], '', 15000);
    if (initialized.ops_ok !== 1) throw new Error(`Could not initialize blank editor project: ${JSON.stringify(initialized)}`);
  } finally {
    reset.close();
  }
  const logPath = path.join(dataDir, 'assistant.log');
  const brainPath = path.join(dataDir, 'brain.json');
  const beforeAgentLog = fs.existsSync(logPath) ? fs.statSync(logPath).size : 0;
  agent = spawn(process.execPath, [agentHost, '--dir', dataDir, '--model', model, '--effort', 'low'], {
    cwd: path.join(releaseDir, 'ASSISTANT'), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stderr = '';
  agent.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
  if (!await waitUntil(() => fs.existsSync(logPath) && fs.statSync(logPath).size > beforeAgentLog, 10000)) {
    throw new Error(`Packaged Agent did not start: ${stderr.slice(0, 500)}`);
  }

  let seq = 0;
  const send = (record) => {
    fs.appendFileSync(path.join(dataDir, 'chat_in.jsonl'), `${JSON.stringify({ seq: ++seq, t: Date.now(), ...record })}\n`);
    fs.writeFileSync(path.join(dataDir, 'chat_in.flag'), '');
  };
  const chatId = `user_video_${Date.now()}`;
  send({ type: 'hello', editor_version: 'native-user-video', project: '' });
  send({ type: 'chat', action: 'new', chat_id: chatId });
  if (!await waitUntil(() => ['ready', 'idle'].includes(String(readJson(brainPath)?.state)), 20000)) {
    throw new Error('Packaged Agent did not become ready');
  }

  const beforeTurnLog = fs.statSync(logPath).size;
  const turnId = `user-video-${Date.now()}`;
  const turnStarted = Date.now();
  send({ type: 'user', turn_id: turnId, text: prompt, project_file: '' });
  const outcome = await waitUntil(() => {
    const tail = fs.readFileSync(logPath, 'utf8').slice(beforeTurnLog);
    if (tail.includes(`ERROR en turno ${turnId}:`)) return { error: tail };
    if (tail.includes('[codex usage]') && readJson(brainPath)?.state === 'idle') return { completed: tail };
    return undefined;
  }, 1_900_000);
  if (!outcome) throw new Error(`Agent video turn timed out: ${stderr.slice(0, 500)}`);
  if (outcome.error) throw new Error(`Agent video turn failed: ${outcome.error.slice(-1500)}`);

  const completedTools = [...outcome.completed.matchAll(/tool result ([a-z_]+) ok=true:/g)].map((match) => match[1]);
  if (!completedTools.includes('scene_get') || !completedTools.includes('scene_validate') || !completedTools.includes('scene_apply')) {
    throw new Error(`Agent did not complete the scene edit contract: ${JSON.stringify(completedTools)}`);
  }
  if (!completedTools.includes('video_export')) throw new Error(`Agent did not complete video_export: ${JSON.stringify(completedTools)}`);

  await stopOwned(agent);
  agent = undefined;
  client = await connectMcp();
  const sceneResult = value(await client.callTool({ name: 'scene_get', arguments: {} }));
  const sceneDocument = JSON.parse(String(sceneResult.document));
  const objects = sceneDocument.scene?.objects ?? [];
  const sphere = objects.find((object) => object.kind === 'sphere');
  const camera = objects.find((object) => object.kind === 'camera');
  const sphereMaterial = sphere ? sceneDocument.materials?.[sphere.material_id] : undefined;
  const tracks = sceneDocument.animation?.tracks ?? [];
  const sphereMotion = sphere && tracks.find((track) => track.target === sphere.id && track.property === 'position');
  const cameraMotion = camera && tracks.find((track) => track.target === camera.id && track.property === 'position');
  if (!sphere || !isRed(sphereMaterial?.color)) throw new Error('Final scene does not contain a red sphere');
  if (!sphereMotion || sphereMotion.keys.length < 3) throw new Error('Final scene does not contain a multi-key sphere bounce');
  if (!cameraMotion || cameraMotion.keys.length < 2) throw new Error('Final scene does not contain an animated following camera');

  const manifests = fs.existsSync(path.join(dataDir, 'video'))
    ? fs.readdirSync(path.join(dataDir, 'video'), { withFileTypes: true })
      .filter((entry) => entry.isDirectory())
      .map((entry) => path.join(dataDir, 'video', entry.name, 'manifest.json'))
      .filter((file) => fs.existsSync(file) && fs.statSync(file).mtimeMs >= turnStarted)
      .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)
    : [];
  if (manifests.length === 0) throw new Error('Agent completed without a new video manifest');
  const manifest = readJson(manifests[0]);
  const sourceVideo = path.join(dataDir, String(manifest.video));
  const stat = fs.statSync(sourceVideo);
  const header = fs.readFileSync(sourceVideo).subarray(0, 32).toString('latin1');
  if (!stat.isFile() || stat.size <= 0 || !header.includes('ftyp')) throw new Error('Agent video is not a non-empty MP4');

  const videoOut = path.join(outputDir, 'esfera-roja-pedestal.mp4');
  const sceneOut = path.join(outputDir, 'esfera-roja-pedestal.renderllm');
  const resultOut = path.join(outputDir, 'resultado-agente.json');
  fs.copyFileSync(sourceVideo, videoOut);
  const sampledPixels = visibleRedPixels(videoOut, Number(manifest.width), Number(manifest.height), [0, 47, 95]);
  if (sampledPixels.some((sample) => sample.red_pixels < 50)) {
    throw new Error(`Agent video failed visual red-sphere acceptance: ${JSON.stringify(sampledPixels)}`);
  }
  fs.writeFileSync(sceneOut, `${String(sceneResult.document).replace(/\r?\n?$/, '')}\n`, 'utf8');
  const usageMatch = [...outcome.completed.matchAll(/\[codex usage\] (\{[^\n]+\})/g)].at(-1);
  const result = {
    status: 'agent_video_pixel_verified', prompt, model, effort: 'low', turn_id: turnId,
    completed_tools: completedTools, scene_revision: sceneResult.revision,
    scene_checks: { red_sphere: true, bounce_keys: sphereMotion.keys.length, following_camera_keys: cameraMotion.keys.length },
    video: { path: videoOut, bytes: stat.size, sampled_pixels: sampledPixels, ...manifest },
    usage: usageMatch ? JSON.parse(usageMatch[1]) : null,
  };
  fs.writeFileSync(resultOut, `${JSON.stringify(result, null, 2)}\n`, 'utf8');
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
} finally {
  if (client) await client.close().catch(() => {});
  await stopOwned(agent);
  await stopOwned(editor);
}
