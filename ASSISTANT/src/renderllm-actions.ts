import { Protocol, type OpsRes } from './protocol.js';
import { planColorBatch, runColorBatch, type ColorBatchResult } from './batch.js';
import { mediaJobsFor, type MediaJobSnapshot } from './media-jobs.js';
import type { OnPhase } from './render.js';
import { runStillRender, type StillResult } from './still.js';
import { runVideoExport, type VideoResult } from './video.js';

export type RenderLlmActionName = 'scene_get' | 'scene_validate' | 'scene_apply' | 'undo' | 'redo';

export type RenderLlmActionArgs = {
  scene_get: Record<string, never>;
  scene_validate: { document: string };
  scene_apply: { document: string; expected_revision: number };
  undo: Record<string, never>;
  redo: Record<string, never>;
};

export interface RenderLlmActionContext {
  turnId: string;
  signal?: AbortSignal;
}

export interface ColorBatchActionArgs {
  object_id: string;
  colors: string[];
  expected_revision: number;
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
}

export interface VideoActionArgs {
  format?: 'mp4' | 'png-sequence';
  expected_revision: number;
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
  fps: number;
  frames: number;
}

export interface StillActionArgs {
  expected_revision: number;
  width: number;
  height: number;
  samples: number;
  denoise: boolean;
}

export interface MediaJobArgs {
  job_id: string;
}

async function freezeSceneAtRevision(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  expectedRevision: number,
): Promise<{ document: string; revision: number }> {
  const snapshot = renderLlmActionJson(await runRenderLlmAction(protocol, ctx, 'scene_get', {}));
  if (snapshot.success !== true) throw new Error(String(snapshot.error ?? 'could not snapshot the open scene'));
  const document = String(snapshot.document ?? '');
  const revision = Number(snapshot.revision);
  if (!Number.isInteger(revision) || revision < 0) throw new Error('scene snapshot returned an invalid revision');
  if (revision !== expectedRevision) {
    throw new Error(`stale scene revision: expected ${expectedRevision}, current ${revision}`);
  }
  return { document, revision };
}

export async function runRenderLlmAction<Name extends RenderLlmActionName>(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  name: Name,
  args: RenderLlmActionArgs[Name],
): Promise<OpsRes> {
  switch (name) {
    case 'scene_get':
      return protocol.runOps(
        [{ id: 'scene-get', op: 'document.snapshot', args: {} }],
        ctx.turnId,
        15000,
        ctx.signal,
      );
    case 'scene_validate':
      return protocol.runOps(
        [{ id: 'scene-validate', op: 'document.canonicalize', args }],
        ctx.turnId,
        15000,
        ctx.signal,
      );
    case 'scene_apply':
      return protocol.runOps(
        [{ id: 'scene-apply', op: 'document.apply', args }],
        ctx.turnId,
        30000,
        ctx.signal,
      );
    case 'undo':
      return protocol.runOps(
        [{ id: 'undo', op: 'document.undo', args: {} }],
        ctx.turnId,
        15000,
        ctx.signal,
      );
    case 'redo':
      return protocol.runOps(
        [{ id: 'redo', op: 'document.redo', args: {} }],
        ctx.turnId,
        15000,
        ctx.signal,
      );
  }
}

export function renderLlmActionJson(res: OpsRes): Record<string, unknown> {
  const result = res.results?.[0] ?? {};
  const success = !res.error && res.ops_total === res.ops_ok && res.results.every((entry) => !!entry.ok);
  return {
    success,
    ...(res.error ? { error: res.error } : {}),
    ...result,
  };
}

export async function runColorBatchAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: ColorBatchActionArgs,
  onPhase: OnPhase = () => {},
): Promise<{ success: true; ok: 1; base_revision: number } & ColorBatchResult> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const plan = planColorBatch(document, args.object_id, args.colors);
  const result = await runColorBatch(protocol, plan, {
    width: args.width,
    height: args.height,
    spp: args.samples,
    denoise: args.denoise,
  }, onPhase, ctx.signal);
  return { success: true, ok: 1, base_revision: revision, ...result };
}

export async function runVideoAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: VideoActionArgs,
  onPhase: OnPhase = () => {},
): Promise<{ success: true; ok: 1; base_revision: number } & VideoResult> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const result = await runVideoExport(protocol, document, {
    width: args.width,
    height: args.height,
    samples: args.samples,
    denoise: args.denoise,
    fps: args.fps,
    frames: args.frames,
    format: args.format,
  }, onPhase, ctx.signal);
  return { success: true, ok: 1, base_revision: revision, ...result };
}

export async function runStillAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: StillActionArgs,
  onPhase: OnPhase = () => {},
): Promise<{ success: true; ok: 1; base_revision: number } & StillResult> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const result = await runStillRender(protocol, document, {
    width: args.width,
    height: args.height,
    samples: args.samples,
    denoise: args.denoise,
  }, onPhase, ctx.signal);
  return { success: true, ok: 1, base_revision: revision, ...result };
}

export async function startStillAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: StillActionArgs,
): Promise<{ success: true; ok: 1; job: MediaJobSnapshot }> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const job = mediaJobsFor(protocol).start('render', revision, async (signal) => runStillRender(protocol, document, {
    width: args.width,
    height: args.height,
    samples: args.samples,
    denoise: args.denoise,
  }, () => {}, signal));
  return { success: true, ok: 1, job };
}

export async function startVideoAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: VideoActionArgs,
): Promise<{ success: true; ok: 1; job: MediaJobSnapshot }> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const job = mediaJobsFor(protocol).start('video', revision, async (signal) => runVideoExport(protocol, document, {
    width: args.width,
    height: args.height,
    samples: args.samples,
    denoise: args.denoise,
    fps: args.fps,
    frames: args.frames,
    format: args.format,
  }, () => {}, signal));
  return { success: true, ok: 1, job };
}

export async function startColorBatchAction(
  protocol: Protocol,
  ctx: RenderLlmActionContext,
  args: ColorBatchActionArgs,
): Promise<{ success: true; ok: 1; job: MediaJobSnapshot }> {
  const { document, revision } = await freezeSceneAtRevision(protocol, ctx, args.expected_revision);
  const plan = planColorBatch(document, args.object_id, args.colors);
  const job = mediaJobsFor(protocol).start('batch', revision, async (signal) => runColorBatch(protocol, plan, {
    width: args.width,
    height: args.height,
    spp: args.samples,
    denoise: args.denoise,
  }, () => {}, signal));
  return { success: true, ok: 1, job };
}

export function statusStillAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).status('render', args.job_id) };
}

export function cancelStillAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).cancel('render', args.job_id) };
}

export function statusVideoAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).status('video', args.job_id) };
}

export function cancelVideoAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).cancel('video', args.job_id) };
}

export function statusColorBatchAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).status('batch', args.job_id) };
}

export function cancelColorBatchAction(protocol: Protocol, args: MediaJobArgs): { success: true; ok: 1; job: MediaJobSnapshot } {
  return { success: true, ok: 1, job: mediaJobsFor(protocol).cancel('batch', args.job_id) };
}
