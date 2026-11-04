import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';

// Read real native batch artifacts; no editor mutation or model request.
const [dataDir, evidenceDir, sourceDocumentPath] = process.argv.slice(2).map(value => path.resolve(value));
if (!dataDir || !evidenceDir) throw new Error('Usage: verify-native-variants <Data/assistant> <evidence>');
const report = JSON.parse(fs.readFileSync(path.join(evidenceDir, 'complete.json'), 'utf8'));
assert.equal(report.passed, true); assert.equal(report.scene_unchanged, true);
const result = report.result;
assert.equal(result.outputs.length, 4);
const sourceDocument = sourceDocumentPath ? JSON.parse(fs.readFileSync(sourceDocumentPath, 'utf8')) : undefined;
if (sourceDocument) sourceDocument.materials[result.material_id].color = '#000000';
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex').toUpperCase();
let reference;
const checks = result.outputs.map(output => {
  const file = path.resolve(dataDir, output.image);
  assert(file.startsWith(dataDir + path.sep), 'output escaped native data');
  const documentFile = file.replace(/\.png$/, '.renderllm');
  const bytes = fs.readFileSync(file), documentBytes = fs.readFileSync(documentFile);
  assert.equal(hash(bytes), output.image_sha256); assert.equal(hash(documentBytes), output.document_sha256);
  const doc = JSON.parse(documentBytes);
  assert.equal(doc.materials[result.material_id].color, output.color);
  assert.equal(doc.scene.objects.filter(object => object.material_id === result.material_id).length, 1);
  doc.materials[result.material_id].color = '#000000';
  if (sourceDocument) assert.deepEqual(doc, sourceDocument, 'variant differs from source beyond selected material color');
  if (reference) assert.deepEqual(doc, reference, 'variant differs beyond selected material color'); else reference = doc;
  const decoded = spawnSync('ffmpeg', ['-v', 'error', '-i', file, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], { windowsHide: true, maxBuffer: 8 * 1024 * 1024 });
  assert.equal(decoded.status, 0, String(decoded.stderr));
  assert.equal(decoded.stdout.length, output.width * output.height * 3);
  fs.copyFileSync(documentFile, path.join(evidenceDir, path.basename(documentFile)));
  return { index: output.index, color: output.color, decoded_bytes: decoded.stdout.length, decoded_sha256: hash(decoded.stdout), document_sha256: output.document_sha256 };
});
assert.equal(new Set(checks.map(check => check.decoded_sha256)).size, 4, 'decoded variants are not distinct');
const evidence = { passed: true, only_selected_color_changed: true, four_decoded_pixel_hashes: true,
  ...(sourceDocument ? {source_document: sourceDocumentPath, source_preserved_except_color: true,
    retained_groups: sourceDocument.scene.objects.filter(object => object.kind === 'group').length} : {}), checks };
fs.writeFileSync(path.join(evidenceDir, 'isolation.json'), JSON.stringify(evidence, null, 2) + '\n');
console.log('PASS four real native variants: full RGB decode, hashes, and color-only document isolation');
