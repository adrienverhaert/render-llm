import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { z } from 'zod';
import { Protocol } from './protocol.js';
import {
  ensureRenderInstance,
  looksLikeMp4,
  runHeadless,
  RENDER_EXE_NAME,
  RenderAborted,
  RenderError,
  requireCompletedRenderResult,
  pngDims,
  type OnPhase,
} from './render.js';

export interface VideoRequest {
  format?: 'mp4' | 'png-sequence';
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
  fps: number;
  frames: number;
}

export interface VideoResult {
  [key: string]: unknown;
  job_id: string;
  job_dir: string;
  document_sha256: string;
  format: 'mp4' | 'png-sequence';
  video?: string;
  video_sha256?: string;
  sequence?: string;
  images?: Array<{index: number; image: string; image_sha256: string; bytes: number}>;
  bytes: number;
  width: number;
  height: number;
  frames: number;
  fps: number;
  duration_s: number;
  samples: number;
  denoise: boolean;
  render_status: string;
  backend: string;
  denoiser: string;
}

function sha256(data: string | Buffer): string {
  return crypto.createHash('sha256').update(data).digest('hex').toUpperCase();
}

export async function runVideoExport(
  protocol: Protocol,
  document: string,
  request: VideoRequest,
  onPhase: OnPhase,
  signal?: AbortSignal,
): Promise<VideoResult> {
  if (signal?.aborted) throw new RenderAborted('video export cancelled');
  const format = request.format ?? 'mp4';
  if (format !== 'mp4' && format !== 'png-sequence') throw new RenderError('unsupported animation output format');
  if (!Number.isInteger(request.width) || request.width < 320 || request.width > 1920
      || !Number.isInteger(request.height) || request.height < 180 || request.height > 1080
      || request.width % 2 !== 0 || request.height % 2 !== 0) {
    throw new RenderError('video dimensions must be even integers within 320x180 to 1920x1080');
  }
  if (!Number.isInteger(request.samples) || request.samples < 1 || request.samples > 256) {
    throw new RenderError('video samples must be an integer from 1 to 256');
  }
  if (!Number.isInteger(request.fps) || request.fps < 1 || request.fps > 60) {
    throw new RenderError('video fps must be an integer from 1 to 60');
  }
  if (!Number.isInteger(request.frames) || request.frames < 1 || request.frames > 240) {
    throw new RenderError('video frames must be an integer from 1 to 240');
  }
  if (request.width * request.height * request.samples * request.frames > 8_000_000_000) {
    throw new RenderError('video render budget exceeds the hard limit');
  }
  try {
    const root = z.object({
      version: z.literal(1),
      animation: z.object({ fps: z.number().positive(), frame_count: z.number().int().positive() }).passthrough(),
    }).passthrough().parse(JSON.parse(document));
    const lastTimelineFrame = (request.frames - 1) * root.animation.fps / request.fps;
    if (lastTimelineFrame > root.animation.frame_count - 1 + 1e-6) {
      throw new RenderError(
        `document ends at frame ${root.animation.frame_count - 1} but video needs timeline frame ${lastTimelineFrame}`,
      );
    }
  } catch (error) {
    if (error instanceof RenderError) throw error;
    throw new RenderError('video base must be a valid render-llm document');
  }

  const installDir = path.resolve(protocol.dir, '..', '..');
  if (!fs.existsSync(path.join(installDir, RENDER_EXE_NAME))) throw new RenderError('video install executable is missing');
  const renderInst = ensureRenderInstance(protocol, installDir, onPhase);
  const jobId = `video-${crypto.randomUUID()}`;
  const jobDir = path.join(protocol.dir, 'video', jobId);
  fs.mkdirSync(jobDir, { recursive: true });
  const documentPath = path.join(jobDir, 'scene.renderllm');
  const videoPath = path.join(jobDir, format === 'mp4' ? 'video.mp4' : 'frames');
  const warmPath = path.join(jobDir, 'warmup.png');
  const scriptPath = path.join(jobDir, 'job.json');
  fs.writeFileSync(documentPath, document, 'utf8');
  fs.writeFileSync(scriptPath, JSON.stringify({
    stop_on_error: 1,
    ops: [
      { id: 'reset', op: 'project.reset', args: {} },
      { id: 'apply', op: 'document.apply', args: { document, expected_revision: 0 } },
      { id: 'warm', op: 'render.final', args: { width: 160, height: 90, samples: 1, denoise: 0, transparent: 0, out: warmPath.replace(/\\/g, '/') } },
      { id: 'video', op: 'export.movie', args: {
        out: videoPath.replace(/\\/g, '/'), width: request.width, height: request.height,
        fps: request.fps, from: 0, to: request.frames - 1, step: 1, unit: 'output',
        samples: request.samples, denoise: request.denoise ? 1 : 0,
        format,
      } },
    ],
  }, null, 1), 'utf8');

  onPhase('render', `video ${request.frames} frames at ${request.fps} fps`);
  const outcome = await runHeadless(
    protocol,
    renderInst,
    scriptPath,
    path.join(jobDir, 'render.pid'),
    1_800_000,
    signal,
    (elapsed) => onPhase('render', `video: ${Math.round(elapsed / 1000)} s`),
  );
  if (outcome.stdout.trim()) fs.writeFileSync(path.join(jobDir, 'result.json'), outcome.stdout, 'utf8');
  if (outcome.stderr.trim()) fs.writeFileSync(path.join(jobDir, 'stderr.log'), outcome.stderr, 'utf8');
  let parsed: unknown;
  try { parsed = JSON.parse(outcome.stdout); } catch { throw new RenderError('video renderer returned invalid JSON'); }
  const headless = z.object({
    ops_total: z.number().int(), ops_ok: z.number().int(),
    results: z.array(z.record(z.string(), z.unknown())),
  }).parse(parsed);
  const result = headless.results.find((entry) => entry.id === 'video');
  const warmResult = headless.results.find((entry) => entry.id === 'warm');
  if (!result || headless.ops_total !== headless.ops_ok || Number(result.ok) !== 1) {
    const failed = headless.results.find((entry) => Number(entry.ok) !== 1);
    throw new RenderError(`video renderer failed: ${String(result?.error ?? failed?.error ?? 'no completed video operation')}`);
  }
  if (!warmResult || Number(warmResult.ok) !== 1) throw new RenderError('video Flame warmup result is missing');
  requireCompletedRenderResult(warmResult, 'video renderer warmup');
  const status = requireCompletedRenderResult(result, 'video renderer');
  const backend = /(?:^| )backend=([^ ]+)/.exec(status)?.[1] ?? 'unknown';
  const denoiser = /(?:^| )denoiser=([^ ]+)/.exec(status)?.[1] ?? 'unknown';
  if (request.denoise && denoiser === 'off') throw new RenderError('video requested denoising but the executed denoiser was off');
  let bytes = 0;
  let media: Pick<VideoResult, 'video' | 'video_sha256' | 'sequence' | 'images'>;
  if (format === 'mp4') {
    bytes = fs.existsSync(videoPath) ? fs.statSync(videoPath).size : 0;
    if (bytes <= 0 || !looksLikeMp4(videoPath)) throw new RenderError('video output is missing or has no MP4 ftyp header');
    media = {video: path.relative(protocol.dir, videoPath).replace(/\\/g, '/'), video_sha256: sha256(fs.readFileSync(videoPath))};
  } else {
    if (result.format !== 'png-sequence' || !fs.existsSync(videoPath) || !fs.statSync(videoPath).isDirectory()) throw new RenderError('PNG sequence directory missing');
    if (fs.readdirSync(videoPath).length !== request.frames) throw new RenderError('PNG sequence contains missing or incomplete frames');
    const images = Array.from({length: request.frames}, (_, index) => {
      const file = path.join(videoPath, `frame-${String(index).padStart(6, '0')}.png`);
      const dims = pngDims(file);
      if (!dims || dims.w !== request.width || dims.h !== request.height) throw new RenderError(`Invalid PNG sequence frame ${index}`);
      const data = fs.readFileSync(file); bytes += data.length;
      return {index, image: path.relative(protocol.dir, file).replace(/\\/g, '/'), image_sha256: sha256(data), bytes: data.length};
    });
    media = {sequence: path.relative(protocol.dir, videoPath).replace(/\\/g, '/'), images};
  }
  if (Number(result.frames) !== request.frames || Number(result.fps) !== request.fps
      || Number(result.width) !== request.width || Number(result.height) !== request.height) {
    throw new RenderError('video encoder metadata does not match the request');
  }
  const final: VideoResult = {
    job_id: jobId,
    job_dir: path.relative(protocol.dir, jobDir).replace(/\\/g, '/'),
    document_sha256: sha256(document),
    format,
    ...media,
    bytes,
    width: request.width,
    height: request.height,
    frames: request.frames,
    fps: request.fps,
    duration_s: request.frames / request.fps,
    samples: request.samples,
    denoise: request.denoise,
    render_status: status,
    backend,
    denoiser,
  };
  fs.writeFileSync(path.join(jobDir, 'manifest.json'), `${JSON.stringify(final, null, 2)}\n`, 'utf8');
  onPhase('done', `${final.frames} frames, ${final.duration_s} s`);
  return final;
}
