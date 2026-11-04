import crypto from 'node:crypto';
import http, { type IncomingMessage, type ServerResponse } from 'node:http';
import { z } from 'zod';
import type {
  ColorBatchActionArgs,
  MediaJobArgs,
  RenderLlmActionArgs,
  RenderLlmActionName,
  StillActionArgs,
  VideoActionArgs,
} from './renderllm-actions.js';

const EMPTY = z.object({}).strict();
const RENDER_JOB = z.object({ job_id: z.string().regex(/^render-[0-9a-f-]{36}$/i) }).strict();
const BATCH_JOB = z.object({ job_id: z.string().regex(/^batch-[0-9a-f-]{36}$/i) }).strict();
const VIDEO_JOB = z.object({ job_id: z.string().regex(/^video-[0-9a-f-]{36}$/i) }).strict();
const VALIDATE = z.object({ document: z.string().min(1) }).strict();
const APPLY = z.object({
  document: z.string().min(1),
  expected_revision: z.number().int().nonnegative(),
}).strict();
const COLOR_BATCH = z.object({
  object_id: z.string().min(1),
  colors: z.array(z.string().regex(/^#[0-9A-Fa-f]{6}$/)).length(4),
  expected_revision: z.number().int().nonnegative(),
  width: z.number().int().min(320).max(1920),
  height: z.number().int().min(180).max(1080),
  samples: z.number().int().min(1).max(256),
  denoise: z.boolean(),
}).strict();
const STILL = z.object({
  expected_revision: z.number().int().nonnegative(),
  width: z.number().int().min(320).max(1920),
  height: z.number().int().min(180).max(1080),
  samples: z.number().int().min(1).max(256),
  denoise: z.boolean(),
}).strict();
const VIDEO = z.object({
  format: z.enum(['mp4', 'png-sequence']).optional(),
  expected_revision: z.number().int().nonnegative(),
  width: z.number().int().min(320).max(1920).refine((value) => value % 2 === 0),
  height: z.number().int().min(180).max(1080).refine((value) => value % 2 === 0),
  samples: z.number().int().min(1).max(256),
  denoise: z.boolean().default(true),
  fps: z.number().int().min(1).max(60),
  frames: z.number().int().min(1).max(240),
}).strict();

export type HttpActionName = RenderLlmActionName
  | 'color_batch' | 'batch_start' | 'batch_status' | 'batch_cancel'
  | 'render_start' | 'render_status' | 'render_cancel' | 'render_image'
  | 'video_export' | 'video_start' | 'video_status' | 'video_cancel';
export type HttpActionArgs = RenderLlmActionArgs & {
  color_batch: ColorBatchActionArgs;
  batch_start: ColorBatchActionArgs;
  batch_status: MediaJobArgs;
  batch_cancel: MediaJobArgs;
  render_start: StillActionArgs;
  render_status: MediaJobArgs;
  render_cancel: MediaJobArgs;
  render_image: StillActionArgs;
  video_export: VideoActionArgs;
  video_start: VideoActionArgs;
  video_status: MediaJobArgs;
  video_cancel: MediaJobArgs;
};

export type HttpActionExecutor = (
  name: HttpActionName,
  args: HttpActionArgs[HttpActionName],
  signal: AbortSignal,
) => Promise<Record<string, unknown>>;

export interface RenderLlmHttpOptions {
  token: string;
  execute: HttpActionExecutor;
  maxBodyBytes?: number;
}

function sendJson(response: ServerResponse, status: number, body: Record<string, unknown>): void {
  const payload = Buffer.from(JSON.stringify(body), 'utf8');
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': payload.byteLength,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  });
  response.end(payload);
}

function tokenMatches(header: string | undefined, expected: string): boolean {
  if (!header?.startsWith('Bearer ')) return false;
  const supplied = Buffer.from(header.slice(7), 'utf8');
  const wanted = Buffer.from(expected, 'utf8');
  return supplied.byteLength === wanted.byteLength && crypto.timingSafeEqual(supplied, wanted);
}

async function readJson(request: IncomingMessage, limit: number): Promise<unknown> {
  const contentType = String(request.headers['content-type'] ?? '').split(';', 1)[0].trim().toLowerCase();
  if (contentType !== 'application/json') throw new Error('content-type must be application/json');
  const declared = Number(request.headers['content-length'] ?? 0);
  if (Number.isFinite(declared) && declared > limit) throw new Error('request body exceeds byte limit');

  const chunks: Buffer[] = [];
  let bytes = 0;
  for await (const chunk of request) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
    bytes += buffer.byteLength;
    if (bytes > limit) throw new Error('request body exceeds byte limit');
    chunks.push(buffer);
  }
  if (bytes === 0) return {};
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8'));
  } catch {
    throw new Error('request body must be valid JSON');
  }
}

export function createRenderLlmHttpServer(options: RenderLlmHttpOptions): http.Server {
  if (Buffer.byteLength(options.token, 'utf8') < 32) throw new Error('HTTP bearer token must be at least 32 bytes');
  const maxBodyBytes = options.maxBodyBytes ?? 17 * 1024 * 1024;

  return http.createServer(async (request, response) => {
    response.setHeader('Connection', 'close');
    if (!tokenMatches(request.headers.authorization, options.token)) {
      sendJson(response, 401, { success: false, error: 'unauthorized' });
      return;
    }

    const controller = new AbortController();
    request.once('aborted', () => controller.abort());
    try {
      const url = new URL(request.url ?? '/', 'http://127.0.0.1');
      let name: HttpActionName;
      let args: HttpActionArgs[HttpActionName];

      if (request.method === 'GET' && url.pathname === '/scene') {
        name = 'scene_get';
        args = {};
      } else if (request.method === 'POST' && url.pathname === '/scene/validate') {
        name = 'scene_validate';
        args = VALIDATE.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'PUT' && url.pathname === '/scene') {
        name = 'scene_apply';
        args = APPLY.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/undo') {
        name = 'undo';
        args = EMPTY.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/redo') {
        name = 'redo';
        args = EMPTY.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/batch/color') {
        name = 'color_batch';
        args = COLOR_BATCH.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/batch/start') {
        name = 'batch_start';
        args = COLOR_BATCH.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/batch/status') {
        name = 'batch_status';
        args = BATCH_JOB.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/batch/cancel') {
        name = 'batch_cancel';
        args = BATCH_JOB.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/render/start') {
        name = 'render_start';
        args = STILL.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/render/status') {
        name = 'render_status';
        args = RENDER_JOB.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/render/cancel') {
        name = 'render_cancel';
        args = RENDER_JOB.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/render/image') {
        name = 'render_image';
        args = STILL.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/video') {
        name = 'video_export';
        args = VIDEO.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/video/start') {
        name = 'video_start';
        args = VIDEO.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/video/status') {
        name = 'video_status';
        args = VIDEO_JOB.parse(await readJson(request, maxBodyBytes));
      } else if (request.method === 'POST' && url.pathname === '/video/cancel') {
        name = 'video_cancel';
        args = VIDEO_JOB.parse(await readJson(request, maxBodyBytes));
      } else {
        sendJson(response, 404, { success: false, error: 'route not found' });
        return;
      }

      const result = await options.execute(name, args, controller.signal);
      sendJson(response, result.success === true ? 200 : 409, result);
    } catch (error) {
      if (error instanceof z.ZodError) {
        sendJson(response, 400, { success: false, error: 'invalid request', issues: error.issues });
        return;
      }
      sendJson(response, 400, {
        success: false,
        error: error instanceof Error ? error.message : String(error),
      });
    }
  });
}
