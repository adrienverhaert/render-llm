import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const runtimeDir = process.argv[2] ? path.resolve(process.argv[2]) : undefined;
if (!runtimeDir) throw new Error('Usage: node test/verify-runtime-package.mjs <staged ASSISTANT directory>');

const expected = [
  'scene_get', 'scene_validate', 'scene_apply', 'undo', 'redo',
  'color_batch', 'batch_start', 'batch_status', 'batch_cancel',
  'render_start', 'render_status', 'render_cancel', 'render_image',
  'video_export', 'video_start', 'video_status', 'video_cancel',
];
const publicModule = path.join(runtimeDir, 'dist', 'renderllm-tools.js');
const exports = await import(pathToFileURL(publicModule).href);
if (JSON.stringify(exports.RENDER_LLM_SHORT_NAMES) !== JSON.stringify(expected)) {
  throw new Error(`Unexpected staged action catalog: ${JSON.stringify(exports.RENDER_LLM_SHORT_NAMES)}`);
}
const expectedQualified = expected.map((name) => `mcp__render-llm__${name}`);
if (JSON.stringify(exports.RENDER_LLM_TOOL_NAMES) !== JSON.stringify(expectedQualified)) {
  throw new Error('Staged qualified MCP names do not match the public action catalog');
}

const forbiddenModules = ['tools', 'gap', 'projectFile', 'fake', 'trimEffects'];
for (const name of forbiddenModules) {
  for (const suffix of ['.js', '.js.map']) {
    const candidate = path.join(runtimeDir, 'dist', `${name}${suffix}`);
    if (fs.existsSync(candidate)) throw new Error(`Retired module leaked into runtime: ${candidate}`);
  }
}

const prompt = fs.readFileSync(path.join(runtimeDir, 'prompts', 'render-llm-role.md'), 'utf8');
if (!prompt.includes('render-llm') || /Mine-imator|Minecraft|MI-3|rig cheat/i.test(prompt)) {
  throw new Error('Staged Agent role is missing render-llm identity or contains retired product language');
}
if (!prompt.includes('right-handed Z-up') || !prompt.includes('local +Y axis') || !prompt.includes('[45,0,225]')) {
  throw new Error('Staged Agent role is missing the retained scene/camera coordinate contract');
}

process.stdout.write(`Verified staged Agent: ${expected.length} actions, ${forbiddenModules.length} retired modules absent.\n`);
