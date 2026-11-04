import fs from 'node:fs';
import path from 'node:path';
import { Protocol } from '../dist/protocol.js';
import { runVideoExport } from '../dist/video.js';

const dirIndex = process.argv.indexOf('--dir');
const documentIndex = process.argv.indexOf('--document');
const dataDir = dirIndex >= 0 ? process.argv[dirIndex + 1] : undefined;
const documentPath = documentIndex >= 0 ? process.argv[documentIndex + 1] : undefined;
if (!dataDir || !documentPath) {
  throw new Error('Usage: node test/run-video-smoke.mjs --dir <Data/assistant> --document <scene.renderllm>');
}

const protocol = new Protocol(path.resolve(dataDir), { actionOnly: true });
const phases = [];
try {
  const result = await runVideoExport(
    protocol,
    fs.readFileSync(path.resolve(documentPath), 'utf8'),
    { width: 384, height: 216, samples: 4, denoise: true, fps: 24, frames: 96 },
    (phase, detail) => phases.push({ phase, detail }),
  );
  if (result.frames !== 96 || result.fps !== 24 || result.duration_s !== 4) {
    throw new Error('Video metadata is not exactly 96 frames at 24 fps for four seconds');
  }
  if (!result.denoise || result.denoiser === 'off') throw new Error('Video did not execute its requested denoiser');
  process.stdout.write(`${JSON.stringify({ ...result, phases }, null, 2)}\n`);
} finally {
  protocol.close();
}
