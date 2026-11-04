// ASSISTANT/test/run-e2e-smoke.mjs
//
// The ONE real-SDK smoke test (spec section 9, Sonnet-B step 4): a single
// turn, no tools, against the user's own Claude Code login, using
// claude-haiku-4-5 to keep cost negligible. This spends real API quota --
// do not loop this script. Run it deliberately, at most twice.
//
//   node test/run-e2e-smoke.mjs

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { FakeEditor } from './fake-editor.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ASSISTANT_DIR = path.resolve(__dirname, '..');
const MODEL = 'claude-haiku-4-5';

function assert(cond, msg) {
  if (!cond) throw new Error('assertion failed: ' + msg);
}

async function waitUntil(fn, timeoutMs, stepMs = 100) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    if (await fn()) return true;
    if (Date.now() > deadline) return false;
    await new Promise((r) => setTimeout(r, stepMs));
  }
}

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mi3-assistant-e2e-'));
  console.log('dir =', dir);
  console.log('model =', MODEL);

  const editor = new FakeEditor(dir);
  editor.start();

  const child = spawn(process.execPath, ['dist/index.js', '--dir', dir, '--model', MODEL], {
    cwd: ASSISTANT_DIR,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (d) => (stdout += d.toString()));
  child.stderr.on('data', (d) => (stderr += d.toString()));

  let ok = true;
  try {
    editor.sendHello();
    const gotReady = await waitUntil(() => editor.getLog().some((r) => r.type === 'status' && r.state === 'ready'), 15000);
    assert(gotReady, `status:ready received (stdout: ${stdout.slice(0, 500)} | stderr: ${stderr.slice(0, 500)})`);
    console.log('OK: status:ready received');

    const t0 = Date.now();
    const turnId = editor.sendUser(
      'Reply with exactly the single word PONG and nothing else. Do not call any tools -- just answer directly.',
    );
    console.log('turn_id =', turnId);

    const done = await waitUntil(() => editor.getLog().some((r) => r.type === 'done' && r.turn_id === turnId), 60000);
    const elapsed = Date.now() - t0;
    assert(done, `turn completed within 60s (elapsed ${elapsed}ms)`);
    console.log(`OK: turn completed in ${elapsed}ms`);

    const turnEvents = editor.getLog().filter((r) => r.turn_id === turnId);
    const tokens = turnEvents.filter((r) => r.type === 'token');
    const texts = turnEvents.filter((r) => r.type === 'text');
    const errors = turnEvents.filter((r) => r.type === 'error');
    const doneEvent = turnEvents.find((r) => r.type === 'done');
    const toolEvents = turnEvents.filter((r) => r.type === 'tool');

    console.log(`tokens=${tokens.length} text_blocks=${texts.length} tool_events=${toolEvents.length} errors=${errors.length}`);
    console.log('done event:', JSON.stringify(doneEvent));

    assert(errors.length === 0, `no error events (got ${errors.length}: ${JSON.stringify(errors)})`);
    assert(tokens.length > 0 || texts.length > 0, 'streaming produced at least one token or fallback text block');
    assert(toolEvents.length === 0, `no tool calls for a no-tools prompt (got ${toolEvents.length})`);

    const fullText = tokens.map((t) => t.text).join('') + texts.map((t) => t.text).join('');
    console.log('assistant said:', JSON.stringify(fullText.slice(0, 200)));
    assert(/pong/i.test(fullText), `response mentions PONG (got: ${JSON.stringify(fullText.slice(0, 200))})`);

    assert(typeof doneEvent.cost_usd === 'number', 'done event carries cost_usd');
    console.log(`cost_usd = ${doneEvent.cost_usd}`);
    assert(doneEvent.cost_usd < 0.01, `cost is negligible for a 1-turn haiku call (got $${doneEvent.cost_usd})`);

    const logPath = path.join(dir, 'assistant.log');
    const logText = fs.existsSync(logPath) ? fs.readFileSync(logPath, 'utf8') : '';
    for (const banned of ['Bash', 'WebSearch', 'WebFetch']) {
      assert(!logText.includes(`"${banned}"`), `assistant.log does not mention the disallowed tool ${banned}`);
    }

    console.log('');
    console.log('ALL CHECKS PASSED');
  } catch (e) {
    ok = false;
    console.error('FAIL:', e && e.stack ? e.stack : e);
    console.error('--- child stdout ---');
    console.error(stdout);
    console.error('--- child stderr ---');
    console.error(stderr);
    console.error('--- assistant.log ---');
    try {
      console.error(fs.readFileSync(path.join(dir, 'assistant.log'), 'utf8'));
    } catch {
      /* nothing written */
    }
  } finally {
    child.kill();
    editor.stop();
    await new Promise((r) => setTimeout(r, 100));
  }

  process.exitCode = ok ? 0 : 1;
}

main();
