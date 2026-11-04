import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dirIndex = process.argv.indexOf('--dir');
const dataDir = dirIndex >= 0 ? process.argv[dirIndex + 1] : undefined;
if (!dataDir) throw new Error('Usage: node test/run-http-smoke.mjs --dir <Data/assistant>');
const seedIndex = process.argv.indexOf('--seed');
const seedPath = seedIndex >= 0 ? process.argv[seedIndex + 1] : undefined;
const hostIndex = process.argv.indexOf('--host');
const hostPath = hostIndex >= 0
  ? path.resolve(process.argv[hostIndex + 1])
  : path.resolve(here, '..', 'dist', 'http-api.js');

const probe = net.createServer();
await new Promise((resolve, reject) => {
  probe.once('error', reject);
  probe.listen(0, '127.0.0.1', resolve);
});
const probeAddress = probe.address();
if (!probeAddress || typeof probeAddress !== 'object') throw new Error('Could not allocate loopback port');
const port = probeAddress.port;
await new Promise((resolve) => probe.close(resolve));

const child = spawn(process.execPath, [
  hostPath,
  '--dir', path.resolve(dataDir),
  '--port', String(port),
  '--shutdown-on-stdin-close',
], { stdio: ['pipe', 'pipe', 'pipe'] });
let stdout = '';
let stderr = '';
child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });

const deadline = Date.now() + 5000;
while (!stdout.includes('HTTP API listening') && child.exitCode === null && Date.now() < deadline) {
  await new Promise((resolve) => setTimeout(resolve, 25));
}
if (!stdout.includes('HTTP API listening')) {
  child.stdin.end();
  await new Promise((resolve) => child.once('exit', resolve));
  throw new Error(`HTTP host did not start: ${stderr}`);
}

const base = `http://127.0.0.1:${port}`;
let headers = {};
async function request(route, options = {}) {
  const response = await fetch(`${base}${route}`, { ...options, headers: { ...headers, ...options.headers } });
  const result = await response.json();
  if (!response.ok || result.success !== true || result.ok !== 1) {
    throw new Error(`HTTP action failed (${response.status}): ${JSON.stringify(result)}`);
  }
  return result;
}

try {
  const token = fs.readFileSync(path.resolve(dataDir, 'renderllm-http.token'), 'utf8').trim();
  headers = { Authorization: `Bearer ${token}` };
  let before = await request('/scene');
  if (seedPath) {
    const seed = fs.readFileSync(path.resolve(seedPath), 'utf8');
    await request('/scene/validate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ document: seed }),
    });
    await request('/scene', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ document: seed, expected_revision: Number(before.revision) }),
    });
    before = await request('/scene');
  }
  const originalText = String(before.document ?? '');
  const changed = JSON.parse(originalText);
  const materialId = Object.keys(changed.materials ?? {})[0];
  if (!materialId) throw new Error('Smoke scene has no editable material');
  const oldColor = String(changed.materials[materialId].color);
  const newColor = oldColor.toUpperCase() === '#2A9D8F' ? '#E76F51' : '#2A9D8F';
  changed.materials[materialId].color = newColor;
  const document = JSON.stringify(changed);

  await request('/scene/validate', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ document }),
  });
  const applied = await request('/scene', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ document, expected_revision: Number(before.revision) }),
  });
  const observed = await request('/scene');
  if (JSON.parse(String(observed.document)).materials[materialId].color !== newColor) {
    throw new Error('Observed scene does not contain the HTTP color edit');
  }
  const undone = await request('/undo', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{}',
  });
  const restored = await request('/scene');
  if (String(restored.document) !== originalText) throw new Error('HTTP undo did not restore exact document bytes');
  const restoredDocument = JSON.parse(String(restored.document));
  const batchObject = restoredDocument.scene.objects.find((entry) => entry.material_id === materialId);
  if (!batchObject) throw new Error('Smoke scene has no shape for the editable material');
  const batch = await request('/batch/color', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      object_id: batchObject.id,
      colors: ['#FF0000', '#0066FF', '#FFD700', '#00CC88'],
      expected_revision: Number(restored.revision),
      width: 384,
      height: 216,
      samples: 4,
      denoise: true,
    }),
  });
  if (batch.outputs?.length !== 4 || new Set(batch.outputs.map((entry) => entry.image_sha256)).size !== 4) {
    throw new Error('HTTP batch did not return four distinct outputs');
  }

  process.stdout.write(JSON.stringify({
    material_id: materialId,
    old_color: oldColor,
    new_color: newColor,
    revisions: {
      before: before.revision,
      applied: applied.revision,
      observed: observed.revision,
      undone: undone.revision,
      restored: restored.revision,
    },
    changed_observed: true,
    undo_byte_equal: true,
    batch: {
      job_id: batch.job_id,
      base_revision: batch.base_revision,
      outputs: batch.outputs,
    },
  }) + '\n');
} finally {
  child.stdin.end();
  await new Promise((resolve) => child.once('exit', resolve));
}
