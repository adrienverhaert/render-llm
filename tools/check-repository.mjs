import assert from 'node:assert/strict';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const privatePath = /(^|\/)(AGENTS\.md|CLAUDE\.md|SKILL\.md|START_GOAL\.txt|PROJECT_BRIEF\.md|PLAN\.md|\.env(?:\..*)?|[^/]+\.(?:token|dmp|pdb))$|(^|\/)(?:\.agents|\.claude|\.codex|\.archive|node_modules)(?:\/|$)|^docs\/evidence\//i;
const privateContent = [
  ['personal home path', /C:[/\\]+Users[/\\]+PC(?:[/\\]|\b)/i],
  ['private key', /-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/],
  ['GitHub token', /\bgh[pousr]_[A-Za-z0-9]{30,}\b/],
  ['provider key', /\bsk-(?:ant-|proj-)?[A-Za-z0-9_-]{40,}\b/],
];

function inspect(name, bytes) {
  const failures = [];
  if ((privatePath.test(name) && name !== '.env.example') || /^build-.*\.json$/i.test(name)) failures.push('private artifact');
  if (!bytes.includes(0)) {
    const content = bytes.toString('utf8');
    for (const [label, pattern] of privateContent) if (pattern.test(content)) failures.push(label);
  }
  return failures;
}

if (process.argv.includes('--self-test')) {
  for (const name of ['AGENTS.md', 'a/CLAUDE.md', 'a/SKILL.md', 'x/.env', 'docs/evidence/transcript.json', '.archive/old.txt', 'state/auth.token', 'build-video-result.json']) {
    assert.ok(inspect(name, Buffer.from('fixture')).length, name);
  }
  assert.deepEqual(inspect('.env.example', Buffer.from('MODEL_NAME=example')), []);
  assert.deepEqual(inspect('ASSISTANT/prompts/render-llm-role.md', Buffer.from('Product instructions')), []);
  assert.ok(inspect('example.txt', Buffer.from(['C:', 'Users', 'PC', 'private'].join('/'))).length);
  assert.ok(inspect('example.txt', Buffer.from('ghp_' + 'x'.repeat(36))).length);
  console.log('Repository boundary self-checks passed.');
} else {
  const files = execFileSync('git', ['ls-files', '-z'], { cwd: root }).toString('utf8').split('\0').filter(Boolean);
  assert.ok(files.length, 'No tracked files to inspect. Stage the intended source first.');
  let failed = false;
  for (const name of files) {
    const failures = inspect(name, fs.readFileSync(new URL('../' + name, import.meta.url)));
    if (failures.length) { console.error(`${name}: ${failures.join(', ')}`); failed = true; }
  }
  if (failed) process.exitCode = 1;
  else console.log(`Checked ${files.length} tracked files for private artifacts and common credential patterns.`);
}
