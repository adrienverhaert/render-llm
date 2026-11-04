import fs from 'node:fs';
import path from 'node:path';
import { readExistingBrain } from './protocol.js';

export interface ActionHostLock {
  path: string;
  token: string;
}

function pidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

export function assertNativeChatIdle(dir: string): void {
  const brain = readExistingBrain(dir);
  if (brain && brain.pid !== process.pid && pidAlive(brain.pid)) {
    throw new Error(
      `Native chat already owns the editor action channel (pid ${brain.pid}). ` +
        'Stop native chat before starting an external action host.',
    );
  }
}

export function acquireActionHostLock(dir: string): ActionHostLock {
  fs.mkdirSync(dir, { recursive: true });
  const lockPath = path.join(dir, 'renderllm-action-host.lock');
  const token = `${process.pid}:${Date.now()}`;
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const fd = fs.openSync(lockPath, 'wx');
      try {
        fs.writeFileSync(fd, JSON.stringify({ pid: process.pid, token, started_at: Date.now() }), 'utf8');
      } finally {
        fs.closeSync(fd);
      }
      return { path: lockPath, token };
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (code !== 'EEXIST') throw error;
      let owner = 0;
      try {
        owner = Number((JSON.parse(fs.readFileSync(lockPath, 'utf8')) as { pid?: unknown }).pid ?? 0);
      } catch {
        owner = 0;
      }
      if (owner > 0 && pidAlive(owner)) {
        throw new Error(`Another render-llm action host is active (pid ${owner}).`);
      }
      fs.rmSync(lockPath, { force: true });
    }
  }
  throw new Error('Could not acquire the render-llm action host lock.');
}

export function releaseActionHostLock(lock: ActionHostLock): void {
  try {
    const current = JSON.parse(fs.readFileSync(lock.path, 'utf8')) as { token?: unknown };
    if (current.token === lock.token) fs.rmSync(lock.path, { force: true });
  } catch {
    // A missing or replaced lock is not ours to remove.
  }
}
