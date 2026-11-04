import fs from 'node:fs';
import path from 'node:path';
import { createSdkMcpServer, tool } from '@anthropic-ai/claude-agent-sdk';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import { z } from 'zod';
import { Protocol, type OpResultEntry, type OpsRes } from './protocol.js';
import {
  cancelColorBatchAction,
  cancelStillAction,
  cancelVideoAction,
  renderLlmActionJson,
  runColorBatchAction,
  runRenderLlmAction,
  runStillAction,
  runVideoAction,
  startColorBatchAction,
  startStillAction,
  startVideoAction,
  statusColorBatchAction,
  statusStillAction,
  statusVideoAction,
} from './renderllm-actions.js';

/** The complete per-turn state required by the public render-llm action surface. */
export interface ActionTurnCtx {
  turnId: string;
  signal?: AbortSignal;
}

export const RENDER_LLM_SHORT_NAMES = [
  'scene_get', 'scene_validate', 'scene_apply', 'undo', 'redo',
  'color_batch', 'batch_start', 'batch_status', 'batch_cancel',
  'render_start', 'render_status', 'render_cancel', 'render_image',
  'video_export', 'video_start', 'video_status', 'video_cancel',
] as const;

export const RENDER_LLM_TOOL_NAMES: string[] = RENDER_LLM_SHORT_NAMES.map(
  (name) => `mcp__render-llm__${name}`,
);

/** Native chat owns non-empty turn IDs; every nested/external action host uses empty. */
export const EXTERNAL_ACTION_TURN_ID = '';

function summarizeAction(res: OpsRes) {
  const parts: string[] = [];
  if (res.error) parts.push(`Editor error: ${res.error}`);
  if (res.checkpoint?.created) {
    parts.push(`Checkpoint created: ${res.checkpoint.cp_id ?? '?'} (${res.checkpoint.name ?? 'assistant'})`);
  } else if (res.checkpoint?.error) {
    const hint = res.checkpoint.error === 'nofolder'
      ? 'the project has never been saved, so there is nothing to roll back to yet. Tell the user to press Ctrl+S before you make changes.'
      : 'the undo safety net is degraded for this turn -- proceed carefully and say so.';
    parts.push(`Checkpoint NOT created (${res.checkpoint.error}): ${hint}`);
  }
  const results = res.results ?? [];
  for (const result of results) {
    const entry = result as OpResultEntry;
    const label = String(entry.op ?? entry.id ?? '?');
    if (!entry.ok) {
      parts.push(`${label}: FAILED -- ${String(entry.error ?? 'unknown error')}`);
      continue;
    }
    const fields = Object.entries(entry)
      .filter(([key, value]) => !['id', 'op', 'ok'].includes(key) && value !== undefined)
      .map(([key, value]) => `${key}: ${typeof value === 'object' && value !== null ? JSON.stringify(value) : String(value)}`);
    parts.push(fields.length ? `${label}:\n${fields.join('\n')}` : `${label}: ok`);
  }
  const ok = !res.error && res.ops_total === res.ops_ok && results.every((result) => !!(result as OpResultEntry).ok);
  return {
    ok,
    text: parts.join('\n\n') || '(empty result)',
    structuredContent: renderLlmActionJson(res),
  };
}

function readPngForModel(p: Protocol, absPath: string) {
  const maxBytes = 4 * 1024 * 1024;
  try {
    const stat = fs.statSync(absPath);
    if (!stat.isFile() || stat.size > maxBytes) {
      p.log(`image not attached (${stat.size} bytes > ${maxBytes}): ${absPath}`);
      return undefined;
    }
    p.log(`image attached to tool result (${stat.size} bytes): ${absPath}`);
    return { type: 'image' as const, data: fs.readFileSync(absPath).toString('base64'), mimeType: 'image/png' };
  } catch (error) {
    p.log(`WARN could not attach image ${absPath}: ${String(error)}`);
    return undefined;
  }
}

type ToolBody = () => Promise<{
  ok: boolean;
  text: string;
  imagePaths?: string[];
  structuredContent?: Record<string, unknown>;
}>;

async function runTool(p: Protocol, ctx: ActionTurnCtx, name: string, summary: string, body: ToolBody): Promise<CallToolResult> {
  const started = Date.now();
  p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'start', summary });
  try {
    const result = await body();
    p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'end', ok: result.ok, ms: Date.now() - started, summary });
    p.log(`tool result ${name} ok=${result.ok}: ${result.text.slice(0, 500)}${result.text.length > 500 ? '...' : ''}`);
    const content: CallToolResult['content'] = [{ type: 'text', text: result.text }];
    for (const candidate of result.imagePaths ?? []) {
      const image = readPngForModel(p, candidate);
      if (image) content.push(image);
    }
    return { content, isError: !result.ok, ...(result.structuredContent ? { structuredContent: result.structuredContent } : {}) };
  } catch (error) {
    p.emit({ type: 'tool', turn_id: ctx.turnId, name, phase: 'end', ok: false, ms: Date.now() - started, summary });
    return { content: [{ type: 'text', text: `ERROR: ${error instanceof Error ? error.message : String(error)}` }], isError: true };
  }
}

const revisionSchema = { expected_revision: z.number().int().nonnegative() };
const renderSchema = {
  ...revisionSchema,
  width: z.number().int().min(320).max(1920),
  height: z.number().int().min(180).max(1080),
  samples: z.number().int().min(1).max(256),
  denoise: z.boolean(),
};
const videoSchema = {
  format: z.enum(['mp4', 'png-sequence']).optional(),
  ...revisionSchema,
  width: z.number().int().min(320).max(1920).refine((value) => value % 2 === 0),
  height: z.number().int().min(180).max(1080).refine((value) => value % 2 === 0),
  samples: z.number().int().min(1).max(256),
  denoise: z.boolean().default(true),
  fps: z.number().int().min(1).max(60),
  frames: z.number().int().min(1).max(240),
};
const batchSchema = {
  object_id: z.string().min(1),
  colors: z.array(z.string().regex(/^#[0-9A-Fa-f]{6}$/)).length(4),
  ...renderSchema,
};

export function buildRenderLlmTools(p: Protocol, ctx: ActionTurnCtx) {
  const action = (name: 'scene_get' | 'scene_validate' | 'scene_apply' | 'undo' | 'redo', description: string, schema: Record<string, z.ZodType>, summary: string) =>
    tool(name, description, schema, async (args) => runTool(p, ctx, name, summary, async () =>
      summarizeAction(await runRenderLlmAction(p, ctx, name, args as never))));
  return [
    action('scene_get', 'Return the complete canonical render-llm document and its current revision. This is read-only.', {}, 'reading the render-llm document'),
    action('scene_validate', 'Validate and canonicalize a complete render-llm document without changing the open scene.', { document: z.string().min(1) }, 'validating the render-llm document'),
    action('scene_apply', 'Transactionally replace the supported live scene with a complete validated render-llm document. The write is undoable and revision guarded.', { document: z.string().min(1), ...revisionSchema }, 'applying the render-llm document'),
    action('undo', 'Undo the most recent render-llm document transaction.', {}, 'undoing the render-llm transaction'),
    action('redo', 'Redo the most recently undone render-llm document transaction.', {}, 'redoing the render-llm transaction'),
    tool('batch_start', 'Start four isolated color variants from the exact requested scene revision and return a job ID.', batchSchema,
      async (args) => runTool(p, ctx, 'batch_start', 'starting isolated color batch', async () => ({ ok: true, text: 'Started revision-frozen color batch.', structuredContent: await startColorBatchAction(p, ctx, args) }))),
    tool('batch_status', 'Read one color-batch job. A completed job returns all four actual PNGs.', { job_id: z.string().regex(/^batch-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'batch_status', 'reading color batch status', async () => {
        const result = statusColorBatchAction(p, args);
        const outputs = result.job.state === 'succeeded' && Array.isArray(result.job.result?.outputs) ? result.job.result.outputs as Array<Record<string, unknown>> : [];
        return { ok: true, text: `Batch job is ${result.job.state}.`, imagePaths: outputs.map((entry) => String(entry.image ?? '')).filter(Boolean).map((image) => path.join(p.dir, image)), structuredContent: result };
      })),
    tool('batch_cancel', 'Cancel one queued or running color-batch job and its current isolated renderer.', { job_id: z.string().regex(/^batch-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'batch_cancel', 'cancelling color batch', async () => { const result = cancelColorBatchAction(p, args); return { ok: true, text: `Batch job is ${result.job.state}.`, structuredContent: result }; })),
    tool('color_batch', 'Render four isolated color variants of one retained shape from the exact current scene revision.', batchSchema,
      async (args) => runTool(p, ctx, 'color_batch', 'rendering isolated color variants', async () => { const result = await runColorBatchAction(p, ctx, args); return { ok: true, text: `Rendered ${result.outputs.length} isolated color variants from revision ${result.base_revision}.`, imagePaths: result.outputs.map((entry) => path.resolve(p.dir, entry.image)), structuredContent: result }; })),
    tool('render_start', 'Start a bounded background render from the exact requested scene revision and return a job ID.', renderSchema,
      async (args) => runTool(p, ctx, 'render_start', 'starting frozen image render', async () => ({ ok: true, text: 'Started revision-frozen image render.', structuredContent: await startStillAction(p, ctx, args) }))),
    tool('render_status', 'Read one render job without changing the scene. A completed job returns its actual PNG.', { job_id: z.string().regex(/^render-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'render_status', 'reading image render status', async () => { const result = statusStillAction(p, args); const image = result.job.state === 'succeeded' ? String(result.job.result?.image ?? '') : ''; return { ok: true, text: `Render job is ${result.job.state}.`, imagePaths: image ? [path.join(p.dir, image)] : [], structuredContent: result }; })),
    tool('render_cancel', 'Cancel one queued or running render job and only its isolated native process.', { job_id: z.string().regex(/^render-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'render_cancel', 'cancelling image render', async () => { const result = cancelStillAction(p, args); return { ok: true, text: `Render job is ${result.job.state}.`, structuredContent: result }; })),
    tool('render_image', 'Render the revision-frozen scene through native Flame and return the actual PNG plus executed backend diagnostics.', renderSchema,
      async (args) => runTool(p, ctx, 'render_image', 'rendering frozen image', async () => { const result = await runStillAction(p, ctx, args); return { ok: true, text: `Rendered ${result.width}x${result.height} at ${result.samples} spp using ${result.backend}/${result.denoiser}.`, imagePaths: [path.join(p.dir, result.image)], structuredContent: result }; })),
    tool('video_start', 'Start a bounded background MP4 or PNG-sequence export from the exact requested scene revision and return a job ID.', videoSchema,
      async (args) => runTool(p, ctx, 'video_start', 'starting frozen video', async () => ({ ok: true, text: 'Started revision-frozen video export.', structuredContent: await startVideoAction(p, ctx, args) }))),
    tool('video_status', 'Read one video-export job without changing the scene.', { job_id: z.string().regex(/^video-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'video_status', 'reading video status', async () => { const result = statusVideoAction(p, args); return { ok: true, text: `Video job is ${result.job.state}.`, structuredContent: result }; })),
    tool('video_cancel', 'Cancel one queued or running video export and only its isolated native process.', { job_id: z.string().regex(/^video-[0-9a-f-]{36}$/i) },
      async (args) => runTool(p, ctx, 'video_cancel', 'cancelling video', async () => { const result = cancelVideoAction(p, args); return { ok: true, text: `Video job is ${result.job.state}.`, structuredContent: result }; })),
    tool('video_export', 'Export a revision-frozen animated render-llm document through the retained native Flame movie path.', videoSchema,
      async (args) => runTool(p, ctx, 'video_export', 'exporting frozen video', async () => { const result = await runVideoAction(p, ctx, args); return { ok: true, text: `Exported ${result.frames} frames at ${result.fps} fps (${result.duration_s} s) using ${result.backend}/${result.denoiser}.`, structuredContent: result }; })),
  ];
}

export function buildRenderLlmMcp(p: Protocol, ctx: ActionTurnCtx) {
  return createSdkMcpServer({
    name: 'render-llm',
    version: '0.1.0',
    instructions: 'Validated, revision-aware control of the open render-llm scene.',
    tools: buildRenderLlmTools(p, ctx),
  });
}
