import test from 'node:test';
import assert from 'node:assert/strict';
import { checkPathCage, resolveInDesignatedRoots } from '../dist/paths.js';

const roots = (project) => ({ projects: '', project, attachments: '', data: '', exports: '', particles: '' });

test('Windows drive and traversal rules do not depend on the host OS', () => {
  const designated = roots('C:\\Projects\\demo');
  for (const candidate of ['C:\\Windows\\win.ini', 'D:/private/file.txt', '..\\private.txt', '../private.txt', 'C:/Projects/demo-other/file.txt', '\\\\server\\share\\file.txt']) {
    assert.equal(checkPathCage(candidate, 'open the scene', designated).ok, false, candidate);
  }
  assert.equal(checkPathCage('assets\\mesh.obj', '', designated).resolved, 'C:\\Projects\\demo\\assets\\mesh.obj');
  assert.equal(checkPathCage('c:/projects/demo/mesh.obj', '', designated).ok, true);
  const literal = checkPathCage('D:\\models\\mesh.obj', 'open D:/models/mesh.obj', designated);
  assert.equal(literal.via, 'literal');
  assert.equal(literal.resolved, 'D:\\models\\mesh.obj');
});

test('POSIX roots preserve case and reject Windows drives and both traversal separators', () => {
  const designated = roots('/srv/projects/demo');
  assert.equal(checkPathCage('assets/mesh.obj', '', designated).resolved, '/srv/projects/demo/assets/mesh.obj');
  for (const candidate of ['C:/Windows/win.ini', '..\\private.txt', '../private.txt', '/SRV/projects/demo/file.txt', '/srv/projects/demo-other/file.txt']) {
    assert.equal(checkPathCage(candidate, '', designated).ok, false, candidate);
  }
  assert.equal(resolveInDesignatedRoots('Canon\\sample.json', { ...roots(''), data: 'C:\\App\\Data' })?.root, 'data');
  assert.equal(resolveInDesignatedRoots('assistant/token.json', { ...roots(''), data: 'C:\\App\\Data' }), null);
});
