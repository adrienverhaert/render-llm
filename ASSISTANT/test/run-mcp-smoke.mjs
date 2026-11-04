import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const dirIndex = process.argv.indexOf('--dir');
const dataDir = dirIndex >= 0 ? process.argv[dirIndex + 1] : undefined;
if (!dataDir) throw new Error('Usage: node test/run-mcp-smoke.mjs --dir <Data/assistant>');
const seedIndex = process.argv.indexOf('--seed');
const seedPath = seedIndex >= 0 ? process.argv[seedIndex + 1] : undefined;
const hostIndex = process.argv.indexOf('--host');
const hostPath = hostIndex >= 0
  ? path.resolve(process.argv[hostIndex + 1])
  : path.resolve(here, '..', 'dist', 'mcp-stdio.js');
const renderRequested = process.argv.includes('--render');

function structured(result) {
  const value = result.structuredContent;
  if (!value || typeof value !== 'object') throw new Error('MCP result has no structuredContent');
  if (value.success !== true || value.ok !== 1) {
    throw new Error(`MCP action failed: ${JSON.stringify(value)}`);
  }
  return value;
}

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [hostPath, '--dir', path.resolve(dataDir)],
  stderr: 'pipe',
});
const client = new Client({ name: 'render-llm-smoke', version: '0.1.0' });

try {
  await client.connect(transport);
  const listed = await client.listTools();
  const names = listed.tools.map((entry) => entry.name).sort();
  const expected = [
    'batch_cancel', 'batch_start', 'batch_status', 'color_batch',
    'redo', 'render_cancel', 'render_image', 'render_start', 'render_status',
    'scene_apply', 'scene_get', 'scene_validate', 'undo',
    'video_cancel', 'video_export', 'video_start', 'video_status',
  ];
  if (names.join(',') !== expected.join(',')) {
    throw new Error(`Unexpected public MCP catalog: ${names.join(',')}`);
  }

  const initial = structured(await client.callTool({ name: 'scene_get', arguments: {} }));
  const initialDocument = JSON.parse(String(initial.document ?? ''));
  if (initialDocument.render?.renderer !== 'Flame') {
    throw new Error(`Normal editor did not start with Flame: ${initialDocument.render?.renderer}`);
  }

  if (seedPath) {
    const seed = fs.readFileSync(path.resolve(seedPath), 'utf8');
    structured(await client.callTool({ name: 'scene_validate', arguments: { document: seed } }));
    structured(await client.callTool({
      name: 'scene_apply',
      arguments: { document: seed, expected_revision: Number(initial.revision) },
    }));
  }

  const before = structured(await client.callTool({ name: 'scene_get', arguments: {} }));
  const originalText = String(before.document ?? '');
  const changed = JSON.parse(originalText);
  const materialIds = Object.keys(changed.materials ?? {});
  if (materialIds.length === 0) throw new Error('Smoke scene has no editable material');
  const materialId = materialIds[0];
  const oldColor = String(changed.materials[materialId].color);
  const newColor = oldColor.toUpperCase() === '#E63946' ? '#2563EB' : '#E63946';
  changed.materials[materialId].color = newColor;
  const changedText = JSON.stringify(changed);

  structured(await client.callTool({
    name: 'scene_validate',
    arguments: { document: changedText },
  }));
  const applied = structured(await client.callTool({
    name: 'scene_apply',
    arguments: { document: changedText, expected_revision: Number(before.revision) },
  }));
  const observed = structured(await client.callTool({ name: 'scene_get', arguments: {} }));
  const observedDocument = JSON.parse(String(observed.document));
  if (observedDocument.materials[materialId].color !== newColor) {
    throw new Error('Observed scene does not contain the MCP color edit');
  }

  const undone = structured(await client.callTool({ name: 'undo', arguments: {} }));
  const restored = structured(await client.callTool({ name: 'scene_get', arguments: {} }));
  if (String(restored.document) !== originalText) throw new Error('Undo did not restore exact document bytes');

  const rendered = renderRequested
    ? structured(await client.callTool({
        name: 'render_image',
        arguments: {
          expected_revision: Number(restored.revision),
          width: 384,
          height: 216,
          samples: 4,
          denoise: true,
        },
      }))
    : undefined;

  process.stdout.write(JSON.stringify({
    tools: names,
    initial_renderer: initialDocument.render.renderer,
    initial_revision: initial.revision,
    material_id: materialId,
    old_color: oldColor,
    new_color: newColor,
    revisions: {
      before: before.revision,
      applied: applied.revision,
      observed: observed.revision,
      undone: undone.revision,
      restored: restored.revision,
    },
    changed_observed: true,
    undo_byte_equal: true,
    ...(rendered ? { render: rendered } : {}),
  }) + '\n');
} finally {
  await client.close();
}
