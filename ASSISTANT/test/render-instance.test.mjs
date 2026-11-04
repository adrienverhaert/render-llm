import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { ensureRenderInstance } from '../dist/render.js';

test('render staging omits mutable editor state and replaces old cache layouts', () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'renderllm-staging-'));
  const put = (name, content = 'fixture') => {
    const file = path.join(root, name);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content);
  };
  try {
    for (const name of ['render-llm.exe', 'Qt5Core.dll', 'platforms/qwindows.dll', 'Data/Fonts/font.ttf', 'Data/Libraries/file.dll', 'Data/legacy.midata',
      'Data/renderllm-ui.png.temporary', 'Data/session.midata', 'Data/log.txt', 'Data/headless.json', 'Data/renderllm-projects/private.renderllm',
      'Data/assistant/chat_in.jsonl', 'Data/assistant/renderllm-http.token', 'Projects/private/file.txt', 'ASSISTANT/private.txt']) put(name);
    const protocol = { dir: path.join(root, 'Data/assistant'), log() {} };
    const instance = ensureRenderInstance(protocol, root, () => {});
    for (const name of ['render-llm.exe', 'Qt5Core.dll', 'platforms/qwindows.dll', 'Data/Fonts/font.ttf', 'Data/Libraries/file.dll', 'Data/legacy.midata']) {
      assert.ok(fs.existsSync(path.join(instance, name)), name);
    }
    for (const name of ['Data/renderllm-ui.png.temporary', 'Data/session.midata', 'Data/log.txt', 'Data/headless.json', 'Data/renderllm-projects', 'Data/assistant', 'Projects', 'ASSISTANT']) {
      assert.equal(fs.existsSync(path.join(instance, name)), false, name);
    }
    put('Data/assistant/render_inst/Data/old-private-state.json');
    const marker = path.join(instance, '.renderllm_render_inst.json');
    const original = JSON.parse(fs.readFileSync(marker));
    fs.writeFileSync(marker, JSON.stringify({ ...original, layout: 1 }));
    ensureRenderInstance(protocol, root, () => {});
    assert.equal(fs.existsSync(path.join(instance, 'Data/old-private-state.json')), false);
    fs.unlinkSync(path.join(root, 'render-llm.exe'));
    assert.throws(() => ensureRenderInstance(protocol, root, () => {}), /renderer is unavailable/);
    assert.ok(fs.existsSync(path.join(root, 'Projects/private/file.txt')), 'source data preserved');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
