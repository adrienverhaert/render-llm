import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { planColorBatch, runColorBatch } from '../dist/batch.js';
import { Protocol } from '../dist/protocol.js';

function samplePngRgb(file, x, y) {
  const png = fs.readFileSync(file);
  if (!png.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) {
    throw new Error(`Not a PNG: ${file}`);
  }
  let offset = 8;
  let width = 0;
  let height = 0;
  let channels = 0;
  const idat = [];
  while (offset < png.length) {
    const length = png.readUInt32BE(offset);
    const type = png.toString('ascii', offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      if (data[8] !== 8 || data[12] !== 0 || ![2, 6].includes(data[9])) {
        throw new Error('Smoke test requires a non-interlaced 8-bit RGB/RGBA PNG');
      }
      channels = data[9] === 2 ? 3 : 4;
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    offset += 12 + length;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const decoded = Buffer.alloc(stride * height);
  let source = 0;
  for (let row = 0; row < height; row += 1) {
    const filter = raw[source++];
    for (let column = 0; column < stride; column += 1) {
      const value = raw[source++];
      const left = column >= channels ? decoded[row * stride + column - channels] : 0;
      const up = row > 0 ? decoded[(row - 1) * stride + column] : 0;
      const upperLeft = row > 0 && column >= channels ? decoded[(row - 1) * stride + column - channels] : 0;
      let predictor = 0;
      if (filter === 1) predictor = left;
      else if (filter === 2) predictor = up;
      else if (filter === 3) predictor = Math.floor((left + up) / 2);
      else if (filter === 4) {
        const p = left + up - upperLeft;
        const pa = Math.abs(p - left);
        const pb = Math.abs(p - up);
        const pc = Math.abs(p - upperLeft);
        predictor = pa <= pb && pa <= pc ? left : pb <= pc ? up : upperLeft;
      } else if (filter !== 0) throw new Error(`Unsupported PNG filter ${filter}`);
      decoded[row * stride + column] = (value + predictor) & 255;
    }
  }
  const pixel = (y * width + x) * channels;
  return [decoded[pixel], decoded[pixel + 1], decoded[pixel + 2]];
}

function assertColorSignatures(samples) {
  const [[rr, rg, rb], [br, bg, bb], [yr, yg, yb], [gr, gg, gb]] = samples;
  if (!(rr > rg * 4 && rr > rb * 4)) throw new Error('Red variant lacks a red pixel signature');
  if (!(bb > br * 4 && bb > bg * 1.5)) throw new Error('Blue variant lacks a blue pixel signature');
  if (!(yr > yb * 4 && yg > yb * 4 && Math.abs(yr - yg) < 64)) throw new Error('Gold variant lacks a yellow pixel signature');
  if (!(gg > gr * 4 && gg > gb * 1.25)) throw new Error('Green variant lacks a green pixel signature');
}

const dirIndex = process.argv.indexOf('--dir');
const documentIndex = process.argv.indexOf('--document');
const dataDir = dirIndex >= 0 ? process.argv[dirIndex + 1] : undefined;
const documentPath = documentIndex >= 0 ? process.argv[documentIndex + 1] : undefined;
if (!dataDir || !documentPath) {
  throw new Error('Usage: node test/run-batch-smoke.mjs --dir <Data/assistant> --document <base.renderllm>');
}

const base = fs.readFileSync(path.resolve(documentPath), 'utf8');
const colors = ['#FF0000', '#0066FF', '#FFD700', '#00CC88'];
const plan = planColorBatch(base, 'cube', colors);
const protocol = new Protocol(path.resolve(dataDir), { actionOnly: true });
const phases = [];
try {
  const result = await runColorBatch(
    protocol,
    plan,
    { width: 384, height: 216, spp: 4, denoise: true },
    (phase, detail, ms) => phases.push({ phase, detail, ...(ms === undefined ? {} : { ms }) }),
  );
  if (result.outputs.length !== 4) throw new Error(`Expected four outputs, got ${result.outputs.length}`);
  if (new Set(result.outputs.map((entry) => entry.image_sha256)).size !== 4) {
    throw new Error('Four batch outputs were not pixel-distinct files');
  }
  const pixel_samples = result.outputs.map((entry) => samplePngRgb(
    path.resolve(dataDir, entry.image),
    Math.floor(entry.width / 2),
    Math.floor(entry.height / 3),
  ));
  assertColorSignatures(pixel_samples);
  process.stdout.write(`${JSON.stringify({ ...result, pixel_samples, phases }, null, 2)}\n`);
} finally {
  protocol.close();
}
