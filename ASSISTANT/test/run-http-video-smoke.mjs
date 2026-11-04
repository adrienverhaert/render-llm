import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Protocol } from '../dist/protocol.js';

const here = path.dirname(fileURLToPath(import.meta.url));
function option(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}
const dataDir = option('--dir');
const documentPath = option('--document');
const hostPath = option('--host')
  ? path.resolve(option('--host'))
  : path.resolve(here, '..', 'dist', 'http-api.js');
const imageOnly = process.argv.includes('--image-only');
const asyncImage = process.argv.includes('--async-image');
const cancelImage = process.argv.includes('--cancel-image');
const skipImage = process.argv.includes('--skip-image');
const asyncVideo = process.argv.includes('--async-video');
const asyncBatch = process.argv.includes('--async-batch');
if (!dataDir || !documentPath) {
  throw new Error('Usage: node test/run-http-video-smoke.mjs --dir <Data/assistant> --document <scene.renderllm>');
}
const document = fs.readFileSync(path.resolve(documentPath), 'utf8');
const batchObjectId = JSON.parse(document).scene?.objects?.find(
  (object) => typeof object?.material_id === 'string',
)?.id;

const setup = new Protocol(path.resolve(dataDir));
try {
  const initialized = await setup.runOps([{ id: 'new', op: 'project.new', args: {} }], '', 15000);
  if (initialized.ops_ok !== 1) throw new Error(`Could not initialize blank editor project: ${JSON.stringify(initialized)}`);
} finally {
  setup.close();
}

const probe = net.createServer();
await new Promise((resolve, reject) => {
  probe.once('error', reject);
  probe.listen(0, '127.0.0.1', resolve);
});
const address = probe.address();
if (!address || typeof address !== 'object') throw new Error('Could not allocate loopback port');
await new Promise((resolve) => probe.close(resolve));

const child = spawn(process.execPath, [
  hostPath,
  '--dir', path.resolve(dataDir),
  '--port', String(address.port),
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

const base = `http://127.0.0.1:${address.port}`;
let headers = {};
async function request(route, options = {}) {
  const response = await fetch(`${base}${route}`, { ...options, headers: { ...headers, ...options.headers } });
  const result = await response.json();
  if (!response.ok || result.success !== true || result.ok !== 1) {
    throw new Error(`HTTP action failed (${response.status}): ${JSON.stringify(result)}`);
  }
  return result;
}

async function waitForJob(prefix, jobId, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const status = await request(`/${prefix}/status`, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ job_id: jobId }),
    });
    if (status.job?.state === 'succeeded') return status.job.result;
    if (status.job?.state === 'failed' || status.job?.state === 'cancelled') {
      throw new Error(`${prefix} job ended ${status.job.state}: ${String(status.job.error ?? '')}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`${prefix} job did not finish within ${timeoutMs} ms`);
}

try {
  const token = fs.readFileSync(path.resolve(dataDir, 'renderllm-http.token'), 'utf8').trim();
  headers = { Authorization: `Bearer ${token}` };
  const before = await request('/scene');
  const applied = await request('/scene', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ document, expected_revision: Number(before.revision) }),
  });
  let image;
  if (!skipImage && asyncImage) {
    const started = await request('/render/start', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        expected_revision: Number(applied.revision), width: 384, height: 216,
        samples: cancelImage ? 256 : 4, denoise: true,
      }),
    });
    const jobId = String(started.job?.job_id ?? '');
    if (cancelImage) {
      await request('/render/cancel', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ job_id: jobId }),
      });
    }
    const deadline = Date.now() + 60000;
    while (Date.now() < deadline) {
      const status = await request('/render/status', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ job_id: jobId }),
      });
      if (status.job?.state === 'succeeded') {
        if (cancelImage) throw new Error('Cancelled image job unexpectedly succeeded');
        image = status.job.result;
        break;
      }
      if (status.job?.state === 'cancelled') {
        if (!cancelImage) throw new Error('Async image job was unexpectedly cancelled');
        image = { job_id: jobId, state: 'cancelled' };
        break;
      }
      if (status.job?.state === 'failed') {
        throw new Error(`Async image job ended ${status.job.state}: ${String(status.job.error ?? '')}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    if (!image) throw new Error('Async image job did not finish within 60 seconds');
  } else if (!skipImage) {
    image = await request('/render/image', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        expected_revision: Number(applied.revision), width: 384, height: 216, samples: 4, denoise: true,
      }),
    });
  }
  let video;
  if (!imageOnly) {
    const videoBody = {
      expected_revision: Number(applied.revision), width: 384, height: 216, samples: 4,
      denoise: true, fps: 24, frames: 96,
    };
    if (asyncVideo) {
      const started = await request('/video/start', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(videoBody),
      });
      video = await waitForJob('video', String(started.job?.job_id ?? ''), 120000);
    } else {
      video = await request('/video', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(videoBody),
      });
    }
  }
  let batch;
  if (asyncBatch) {
    if (typeof batchObjectId !== 'string') throw new Error('Smoke document has no colorable object');
    const started = await request('/batch/start', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        object_id: batchObjectId, colors: ['#FF0000', '#0066FF', '#FFD700', '#00CC88'],
        expected_revision: Number(applied.revision), width: 384, height: 216, samples: 4, denoise: true,
      }),
    });
    batch = await waitForJob('batch', String(started.job?.job_id ?? ''), 120000);
  }
  process.stdout.write(JSON.stringify({
    applied_revision: applied.revision,
    ...(image ? { image } : {}),
    ...(video ? { video } : {}),
    ...(batch ? { batch } : {}),
  }) + '\n');
} finally {
  child.stdin.end();
  await new Promise((resolve) => child.once('exit', resolve));
}
