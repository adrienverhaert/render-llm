import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';

// Compare actual decoded baseline artifacts, not encoded PNG bytes.
const dir = path.resolve(process.argv[2]);
const source = JSON.parse(fs.readFileSync(path.join(dir, 'result.json'), 'utf8'));
const reference = source.runs.filter(run => run.kind === 'reference');
const native = source.runs.filter(run => run.kind === 'native');
assert.equal(reference.length, 3); assert.equal(native.length, 3);
for (const runs of [reference, native]) {
  assert.equal(new Set(runs.map(run => run.rgb_sha256)).size, 1, 'Within-build RGB is not repeatable');
}
function decode(file) {
  const result = spawnSync('ffmpeg', ['-v', 'error', '-i', path.join(dir, file), '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], { windowsHide: true, maxBuffer: 16 * 1024 * 1024 });
  assert.equal(result.status, 0, String(result.stderr));
  assert(result.stdout.length > 0);
  return result.stdout;
}
const a = decode('reference-1.png'), b = decode('native-1.png');
assert.equal(a.length, b.length);
let max = 0, sum = 0, changed = 0;
for (let i = 0; i < a.length; i++) {
  const difference = Math.abs(a[i] - b[i]);
  max = Math.max(max, difference); sum += difference; changed += Number(difference !== 0);
}
const median = runs => runs.map(run => run.render_ms).sort((x, y) => x - y)[1];
const result = {
  reference_exe_sha256: source.reference_exe_sha256,
  native_exe_sha256: source.native_exe_sha256,
  repeatable_rgb_within_each_build: true,
  compared_rgb_channels: a.length, differing_channels: changed,
  maximum_channel_difference: max, mean_channel_difference: sum / a.length,
  exact_rgb_parity: changed === 0,
  reference_median_ms: median(reference), native_median_ms: median(native),
  native_median_delta_percent: (median(native) / median(reference) - 1) * 100,
  scope: 'Three sequential runs per build; one scene, resolution and hardware. Not a general performance guarantee.'
};
fs.writeFileSync(path.join(dir, 'comparison.json'), JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify(result));
