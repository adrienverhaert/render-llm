// ASSISTANT/test/run-e2e-smoke-screenlayout.mjs
//
// RONDA C: a second real-SDK smoke test (see run-e2e-smoke.mjs for the first
// one and its cost/quota rules -- same rules apply here: claude-haiku-4-5,
// one short turn, do not loop). This one exists to verify, against the REAL
// model and the REAL role.md + appended rig cheat sheet, that the new
// mi3_screen_layout tool is registered, schema-valid, and actually reachable
// end to end through the SDK -- not just through FakeEditor's direct op
// dispatch (covered separately and thoroughly in run-tests.mjs).
//
//   node test/run-e2e-smoke-screenlayout.mjs

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
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mi3-assistant-e2e-sl-'));
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
      'Use your screen-layout tool to check how the object named Cubo is framed by the scene\'s ' +
        'active camera. Call the tool once, then reply with exactly one short sentence saying ' +
        'whether Cubo is roughly centered or off to a side. Do not call any other tool.',
    );
    console.log('turn_id =', turnId);

    const done = await waitUntil(() => editor.getLog().some((r) => r.type === 'done' && r.turn_id === turnId), 90000);
    const elapsed = Date.now() - t0;
    assert(done, `turn completed within 90s (elapsed ${elapsed}ms)`);
    console.log(`OK: turn completed in ${elapsed}ms`);

    const turnEvents = editor.getLog().filter((r) => r.turn_id === turnId);
    const tokens = turnEvents.filter((r) => r.type === 'token');
    const texts = turnEvents.filter((r) => r.type === 'text');
    const errors = turnEvents.filter((r) => r.type === 'error');
    const doneEvent = turnEvents.find((r) => r.type === 'done');
    const toolEvents = turnEvents.filter((r) => r.type === 'tool');
    const toolNames = [...new Set(toolEvents.map((t) => t.name))];

    console.log(`tokens=${tokens.length} text_blocks=${texts.length} tool_events=${toolEvents.length} errors=${errors.length}`);
    console.log('tools called:', toolNames.join(', ') || '(none)');
    console.log('done event:', JSON.stringify(doneEvent));

    assert(errors.length === 0, `no error events (got ${errors.length}: ${JSON.stringify(errors)})`);
    assert(toolNames.includes('mi3_screen_layout'), `model called mi3_screen_layout (got: ${toolNames.join(', ') || '(none)'})`);
    const slEnd = toolEvents.find((t) => t.name === 'mi3_screen_layout' && t.phase === 'end');
    assert(slEnd && slEnd.ok === true, `mi3_screen_layout call succeeded (got: ${JSON.stringify(slEnd)})`);

    const fullText = tokens.map((t) => t.text).join('') + texts.map((t) => t.text).join('');
    console.log('assistant said:', JSON.stringify(fullText.slice(0, 300)));
    assert(fullText.trim().length > 0, 'model produced a text answer');

    assert(typeof doneEvent.cost_usd === 'number', 'done event carries cost_usd');
    console.log(`cost_usd = ${doneEvent.cost_usd}`);
    // The system prompt now carries the appended rig cheat sheet (RONDA C), so
    // this costs more in prompt tokens than the tool-less PONG test in
    // run-e2e-smoke.mjs -- $0.10 is still a few cents, not a real budget.
    assert(doneEvent.cost_usd < 0.15, `cost is negligible for a 1-tool haiku turn (got $${doneEvent.cost_usd})`);

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
