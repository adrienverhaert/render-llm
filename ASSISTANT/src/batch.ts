import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { z } from 'zod';
import { Protocol } from './protocol.js';
import {
  ensureRenderInstance,
  pngDims,
  renderTimeoutMs,
  runHeadless,
  RENDER_EXE_NAME,
  RenderAborted,
  RenderError,
  type OnPhase,
} from './render.js';

const HEX_COLOR = /^#[0-9A-F]{6}$/;

const Material = z.object({
  color: z.string(),
}).passthrough();

const SceneObject = z.object({
  id: z.string().min(1),
  kind: z.string(),
  material_id: z.string().min(1).optional(),
}).passthrough();

const Document = z.object({
  version: z.literal(1),
  scene: z.object({ objects: z.array(SceneObject) }).passthrough(),
  materials: z.record(z.string(), Material),
  animation: z.object({
    tracks: z.array(z.object({ target: z.string(), property: z.string() }).passthrough()),
  }).passthrough(),
}).passthrough();

export interface ColorBatchVariant {
  index: number;
  color: string;
  document: string;
  document_sha256: string;
}

export interface ColorBatchPlan {
  base_document: string;
  base_sha256: string;
  object_id: string;
  material_id: string;
  variants: ColorBatchVariant[];
}

export interface ColorBatchRenderRequest {
  width: number;
  height: number;
  spp: number;
  denoise: boolean;
}

export interface ColorBatchOutput {
  index: number;
  color: string;
  document_sha256: string;
  image: string;
  image_sha256: string;
  width: number;
  height: number;
  render_status: string;
  backend: string;
  denoiser: string;
}

export interface ColorBatchResult {
  [key: string]: unknown;
  job_id: string;
  job_dir: string;
  base_sha256: string;
  object_id: string;
  material_id: string;
  outputs: ColorBatchOutput[];
}

function stable(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stable).join(',')}]`;
  if (value !== null && typeof value === 'object') {
    return `{${Object.entries(value as Record<string, unknown>)
      .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
      .map(([key, entry]) => `${JSON.stringify(key)}:${stable(entry)}`)
      .join(',')}}`;
  }
  return JSON.stringify(value);
}

function canonical(value: unknown): string {
  return `${stable(value)}\n`;
}

function sha256(value: string): string {
  return crypto.createHash('sha256').update(value, 'utf8').digest('hex').toUpperCase();
}

export function planColorBatch(documentText: string, objectId: string, colors: string[]): ColorBatchPlan {
  if (colors.length !== 4) throw new Error('color batch requires exactly four colors');
  const normalized = colors.map((color) => color.toUpperCase());
  if (normalized.some((color) => !HEX_COLOR.test(color))) throw new Error('batch colors must use #RRGGBB');
  if (new Set(normalized).size !== normalized.length) throw new Error('batch colors must be distinct');

  let parsed: unknown;
  try {
    parsed = JSON.parse(documentText);
  } catch {
    throw new Error('base document must be valid JSON');
  }
  const base = Document.parse(parsed);
  const object = base.scene.objects.find((entry) => entry.id === objectId);
  if (!object) throw new Error(`batch object does not exist: ${objectId}`);
  if (!['cube', 'sphere', 'plane', 'cylinder', 'cone'].includes(object.kind)) throw new Error('color batch target must be a retained shape');
  if (!object.material_id || !base.materials[object.material_id]) throw new Error('batch object has no valid material');
  const materialId = object.material_id;
  if (!HEX_COLOR.test(base.materials[materialId].color.toUpperCase())) throw new Error('batch material has an invalid base color');
  const shared = base.scene.objects.filter((entry) => entry.material_id === materialId);
  if (shared.length !== 1) throw new Error('batch target material must be unique to the selected shape');
  if (base.animation.tracks.some((track) => track.target === objectId && track.property === 'material.color')) {
    throw new Error('batch target cannot have an animated material.color track');
  }

  const baseDocument = canonical(base);
  const variants = normalized.map((color, index) => {
    const variant = structuredClone(base);
    variant.materials[materialId].color = color;
    const text = canonical(variant);
    return { index, color, document: text, document_sha256: sha256(text) };
  });
  return {
    base_document: baseDocument,
    base_sha256: sha256(baseDocument),
    object_id: objectId,
    material_id: materialId,
    variants,
  };
}

function parseHeadless(stdout: string): { ops_total: number; ops_ok: number; results: Array<Record<string, unknown>> } {
  let parsed: unknown;
  try {
    parsed = JSON.parse(stdout);
  } catch {
    throw new RenderError('batch renderer returned invalid JSON');
  }
  const result = z.object({
    ops_total: z.number().int().nonnegative(),
    ops_ok: z.number().int().nonnegative(),
    results: z.array(z.record(z.string(), z.unknown())),
  }).parse(parsed);
  if (result.ops_total !== result.ops_ok) {
    const failure = result.results.find((entry) => Number(entry.ok) !== 1);
    throw new RenderError(`batch renderer operation failed: ${String(failure?.error ?? 'unknown error')}`);
  }
  return result;
}

function executedRenderer(status: string): { backend: string; denoiser: string } {
  const backend = /(?:^| )backend=([^ ]+)/.exec(status)?.[1] ?? 'unknown';
  const denoiser = /(?:^| )denoiser=([^ ]+)/.exec(status)?.[1] ?? 'unknown';
  return { backend, denoiser };
}

export async function runColorBatch(
  protocol: Protocol,
  plan: ColorBatchPlan,
  request: ColorBatchRenderRequest,
  onPhase: OnPhase,
  signal?: AbortSignal,
): Promise<ColorBatchResult> {
  if (signal?.aborted) throw new RenderAborted('color batch cancelled');
  if (request.width < 320 || request.width > 1920 || request.height < 180 || request.height > 1080) {
    throw new RenderError('batch dimensions are outside 320x180 to 1920x1080');
  }
  if (!Number.isInteger(request.spp) || request.spp < 1 || request.spp > 256) {
    throw new RenderError('batch samples must be an integer from 1 to 256');
  }
  if (request.width * request.height * request.spp > 130_000_000) {
    throw new RenderError('batch render budget exceeds the hard limit');
  }
  if (sha256(plan.base_document) !== plan.base_sha256) throw new RenderError('frozen batch base hash mismatch');

  const installDir = path.resolve(protocol.dir, '..', '..');
  if (!fs.existsSync(path.join(installDir, RENDER_EXE_NAME))) throw new RenderError('batch install executable is missing');
  const renderInst = ensureRenderInstance(protocol, installDir, onPhase);
  const jobId = `color-${crypto.randomUUID()}`;
  const jobDir = path.join(protocol.dir, 'batch', jobId);
  fs.mkdirSync(jobDir, { recursive: true });
  const outputs: ColorBatchOutput[] = [];

  for (const variant of plan.variants) {
    if (signal?.aborted) throw new RenderAborted('color batch cancelled');
    if (sha256(variant.document) !== variant.document_sha256) throw new RenderError(`variant ${variant.index} hash mismatch`);
    const stem = `variant-${String(variant.index).padStart(2, '0')}`;
    const documentPath = path.join(jobDir, `${stem}.renderllm`);
    const outputPath = path.join(jobDir, `${stem}.png`);
    const warmPath = path.join(jobDir, `${stem}-warmup.png`);
    const scriptPath = path.join(jobDir, `${stem}.json`);
    fs.writeFileSync(documentPath, variant.document, 'utf8');
    fs.writeFileSync(scriptPath, JSON.stringify({
      stop_on_error: 1,
      ops: [
        { id: 'reset', op: 'project.reset', args: {} },
        { id: 'apply', op: 'document.apply', args: { document: variant.document, expected_revision: 0 } },
        { id: 'warm', op: 'render.final', args: { width: 160, height: 90, samples: 1, denoise: 0, transparent: 0, out: warmPath.replace(/\\/g, '/') } },
        { id: 'shot', op: 'render.final', args: { width: request.width, height: request.height, samples: request.spp, denoise: request.denoise ? 1 : 0, transparent: 0, out: outputPath.replace(/\\/g, '/') } },
      ],
    }, null, 1), 'utf8');

    onPhase('render', `color ${variant.index + 1}/${plan.variants.length}: ${variant.color}`);
    const outcome = await runHeadless(
      protocol,
      renderInst,
      scriptPath,
      path.join(jobDir, `${stem}.pid`),
      renderTimeoutMs(request.width, request.height, request.spp),
      signal,
      (elapsed) => onPhase('render', `color ${variant.index + 1}/${plan.variants.length}: ${Math.round(elapsed / 1000)} s`),
    );
    const headless = parseHeadless(outcome.stdout);
    const shot = headless.results.find((entry) => entry.id === 'shot');
    const status = String(shot?.render_status ?? '');
    if (!status) throw new RenderError(`variant ${variant.index} has no executed renderer status`);
    const dims = pngDims(outputPath);
    if (!dims || dims.w !== request.width || dims.h !== request.height) {
      throw new RenderError(`variant ${variant.index} output is missing or has invalid dimensions`);
    }
    const actual = executedRenderer(status);
    outputs.push({
      index: variant.index,
      color: variant.color,
      document_sha256: variant.document_sha256,
      image: path.relative(protocol.dir, outputPath).replace(/\\/g, '/'),
      image_sha256: crypto.createHash('sha256').update(fs.readFileSync(outputPath)).digest('hex').toUpperCase(),
      width: dims.w,
      height: dims.h,
      render_status: status,
      backend: actual.backend,
      denoiser: actual.denoiser,
    });
  }

  const result: ColorBatchResult = {
    job_id: jobId,
    job_dir: path.relative(protocol.dir, jobDir).replace(/\\/g, '/'),
    base_sha256: plan.base_sha256,
    object_id: plan.object_id,
    material_id: plan.material_id,
    outputs,
  };
  fs.writeFileSync(path.join(jobDir, 'manifest.json'), `${JSON.stringify(result, null, 2)}\n`, 'utf8');
  onPhase('done', `${outputs.length} color variants`);
  return result;
}
