import crypto from 'node:crypto';
import { Protocol } from './protocol.js';

export type MediaJobKind = 'render' | 'video' | 'batch';
export type MediaJobState = 'queued' | 'running' | 'cancelling' | 'succeeded' | 'failed' | 'cancelled';

export interface MediaJobSnapshot {
  [key: string]: unknown;
  job_id: string;
  kind: MediaJobKind;
  state: MediaJobState;
  base_revision: number;
  created_at: string;
  updated_at: string;
  result?: Record<string, unknown>;
  error?: string;
}

interface MediaJob extends MediaJobSnapshot {
  controller: AbortController;
}

type MediaJobRunner = (signal: AbortSignal) => Promise<Record<string, unknown>>;

const MAX_JOBS = 32;
const registries = new WeakMap<Protocol, MediaJobRegistry>();

function publicSnapshot(job: MediaJob): MediaJobSnapshot {
  return {
    job_id: job.job_id,
    kind: job.kind,
    state: job.state,
    base_revision: job.base_revision,
    created_at: job.created_at,
    updated_at: job.updated_at,
    ...(job.result ? { result: job.result } : {}),
    ...(job.error ? { error: job.error } : {}),
  };
}

export class MediaJobRegistry {
  private readonly jobs = new Map<string, MediaJob>();

  start(kind: MediaJobKind, baseRevision: number, runner: MediaJobRunner): MediaJobSnapshot {
    this.prune();
    if (this.jobs.size >= MAX_JOBS) throw new Error('media job limit reached');
    const now = new Date().toISOString();
    const job: MediaJob = {
      job_id: `${kind}-${crypto.randomUUID()}`,
      kind,
      state: 'queued',
      base_revision: baseRevision,
      created_at: now,
      updated_at: now,
      controller: new AbortController(),
    };
    this.jobs.set(job.job_id, job);
    queueMicrotask(async () => {
      if (job.controller.signal.aborted) {
        job.state = 'cancelled';
        job.updated_at = new Date().toISOString();
        return;
      }
      job.state = 'running';
      job.updated_at = new Date().toISOString();
      try {
        const result = await runner(job.controller.signal);
        if (job.controller.signal.aborted) {
          job.state = 'cancelled';
        } else {
          job.result = result;
          job.state = 'succeeded';
        }
      } catch (error) {
        const aborted = job.controller.signal.aborted || (error instanceof Error && error.name === 'RenderAborted');
        job.state = aborted ? 'cancelled' : 'failed';
        job.error = error instanceof Error ? error.message : String(error);
      }
      job.updated_at = new Date().toISOString();
    });
    return publicSnapshot(job);
  }

  status(kind: MediaJobKind, jobId: string): MediaJobSnapshot {
    const job = this.require(kind, jobId);
    return publicSnapshot(job);
  }

  cancel(kind: MediaJobKind, jobId: string): MediaJobSnapshot {
    const job = this.require(kind, jobId);
    if (job.state === 'queued' || job.state === 'running') {
      job.controller.abort();
      job.state = 'cancelling';
      job.updated_at = new Date().toISOString();
    }
    return publicSnapshot(job);
  }

  private require(kind: MediaJobKind, jobId: string): MediaJob {
    const job = this.jobs.get(jobId);
    if (!job || job.kind !== kind) throw new Error(`${kind} job not found`);
    return job;
  }

  private prune(): void {
    if (this.jobs.size < MAX_JOBS) return;
    for (const [jobId, job] of this.jobs) {
      if (job.state === 'succeeded' || job.state === 'failed' || job.state === 'cancelled') {
        this.jobs.delete(jobId);
        if (this.jobs.size < MAX_JOBS) return;
      }
    }
  }
}

export function mediaJobsFor(protocol: Protocol): MediaJobRegistry {
  let registry = registries.get(protocol);
  if (!registry) {
    registry = new MediaJobRegistry();
    registries.set(protocol, registry);
  }
  return registry;
}
