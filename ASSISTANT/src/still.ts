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
  requireCompletedRenderResult,
  type OnPhase,
} from './render.js';

export interface StillRequest {
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
}

export interface StillResult {
  [key: string]: unknown;
  job_id: string;
  job_dir: string;
  document_sha256: string;
  image: string;
  image_sha256: string;
  bytes: number;
  width: number;
  height: number;
  samples: number;
  render_status: string;
  backend: string;
  denoiser: string;
}

function sha256(data: string | Buffer): string {
  return crypto.createHash('sha256').update(data).digest('hex').toUpperCase();
}

export async function runStillRender(
  protocol: Protocol,
  document: string,
  request: StillRequest,
  onPhase: OnPhase,
  signal?: AbortSignal,
): Promise<StillResult> {
  if (signal?.aborted) throw new RenderAborted('still render cancelled');
  if (!Number.isInteger(request.width) || request.width < 320 || request.width > 1920
      || !Number.isInteger(request.height) || request.height < 180 || request.height > 1080) {
    throw new RenderError('still dimensions must be integers within 320x180 to 1920x1080');
  }
  if (!Number.isInteger(request.samples) || request.samples < 1 || request.samples > 256) {
    throw new RenderError('still samples must be an integer from 1 to 256');
  }
  if (request.width * request.height * request.samples > 130_000_000) {
    throw new RenderError('still render budget exceeds the hard limit');
  }
  try {
    z.object({ version: z.literal(1) }).passthrough().parse(JSON.parse(document));
  } catch {
    throw new RenderError('still base must be a valid render-llm document');
  }

  const installDir = path.resolve(protocol.dir, '..', '..');
  if (!fs.existsSync(path.join(installDir, RENDER_EXE_NAME))) throw new RenderError('still install executable is missing');
  const renderInst = ensureRenderInstance(protocol, installDir, onPhase);
  const jobId = `still-${crypto.randomUUID()}`;
  const jobDir = path.join(protocol.dir, 'still', jobId);
  fs.mkdirSync(jobDir, { recursive: true });
  const documentPath = path.join(jobDir, 'scene.renderllm');
  const imagePath = path.join(jobDir, 'image.png');
  const warmPath = path.join(jobDir, 'warmup.png');
  const scriptPath = path.join(jobDir, 'job.json');
  fs.writeFileSync(documentPath, document, 'utf8');
  fs.writeFileSync(scriptPath, JSON.stringify({
    stop_on_error: 1,
    ops: [
      { id: 'reset', op: 'project.reset', args: {} },
      { id: 'apply', op: 'document.apply', args: { document, expected_revision: 0 } },
      { id: 'warm', op: 'render.final', args: { width: 160, height: 90, samples: 1, denoise: 0, transparent: 0, out: warmPath.replace(/\\/g, '/') } },
      { id: 'image', op: 'render.final', args: {
        width: request.width, height: request.height, samples: request.samples,
        denoise: request.denoise ? 1 : 0, transparent: 0, out: imagePath.replace(/\\/g, '/'),
      } },
    ],
  }, null, 1), 'utf8');

  onPhase('render', `still ${request.width}x${request.height}, ${request.samples} spp`);
  const outcome = await runHeadless(
    protocol,
    renderInst,
    scriptPath,
    path.join(jobDir, 'render.pid'),
    renderTimeoutMs(request.width, request.height, request.samples),
    signal,
    (elapsed) => onPhase('render', `still: ${Math.round(elapsed / 1000)} s`),
  );
  if (outcome.stdout.trim()) fs.writeFileSync(path.join(jobDir, 'result.json'), outcome.stdout, 'utf8');
  if (outcome.stderr.trim()) fs.writeFileSync(path.join(jobDir, 'stderr.log'), outcome.stderr, 'utf8');
  let parsed: unknown;
  try { parsed = JSON.parse(outcome.stdout); } catch { throw new RenderError('still renderer returned invalid JSON'); }
  const headless = z.object({
    ops_total: z.number().int(), ops_ok: z.number().int(),
    results: z.array(z.record(z.string(), z.unknown())),
  }).parse(parsed);
  const imageResult = headless.results.find((entry) => entry.id === 'image');
  if (!imageResult || headless.ops_total !== headless.ops_ok || Number(imageResult.ok) !== 1) {
    const failed = headless.results.find((entry) => Number(entry.ok) !== 1);
    throw new RenderError(`still renderer failed: ${String(imageResult?.error ?? failed?.error ?? 'no completed image operation')}`);
  }
  const status = requireCompletedRenderResult(imageResult, 'still renderer');
  const dims = pngDims(imagePath);
  if (!dims || dims.w !== request.width || dims.h !== request.height) {
    throw new RenderError('still output is missing or has invalid dimensions');
  }
  const final: StillResult = {
    job_id: jobId,
    job_dir: path.relative(protocol.dir, jobDir).replace(/\\/g, '/'),
    document_sha256: sha256(document),
    image: path.relative(protocol.dir, imagePath).replace(/\\/g, '/'),
    image_sha256: sha256(fs.readFileSync(imagePath)),
    bytes: fs.statSync(imagePath).size,
    width: dims.w,
    height: dims.h,
    samples: request.samples,
    render_status: status,
    backend: /(?:^| )backend=([^ ]+)/.exec(status)?.[1] ?? 'unknown',
    denoiser: /(?:^| )denoiser=([^ ]+)/.exec(status)?.[1] ?? 'unknown',
  };
  fs.writeFileSync(path.join(jobDir, 'manifest.json'), `${JSON.stringify(final, null, 2)}\n`, 'utf8');
  onPhase('done', `${final.width}x${final.height}`);
  return final;
}
