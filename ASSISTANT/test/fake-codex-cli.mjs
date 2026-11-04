import fs from 'node:fs';

let input = '';
for await (const chunk of process.stdin) input += chunk;
const images = [];
for (let i = 0; i < process.argv.length - 1; i++) {
  if (process.argv[i] === '-i') images.push(process.argv[i + 1]);
}
if (!process.argv.includes('--ignore-user-config')) process.exit(21);
if (!process.argv.includes('--ignore-rules')) process.exit(22);
if (!process.argv.includes('--disable')) process.exit(23);
if (!process.argv.some((arg) => arg.startsWith('mcp_servers.render-llm.command='))) process.exit(24);
if (images.some((image) => !fs.statSync(image).isFile())) process.exit(25);
if (!process.argv.includes('--strict-config')) process.exit(26);
if (!process.argv.includes('mcp_servers.render-llm.default_tools_approval_mode="approve"')) process.exit(27);
if (!process.argv.includes('mcp_servers.render-llm.required=true')) process.exit(28);
if (!process.argv.includes('mcp_servers.render-llm.tool_timeout_sec=1800')) process.exit(30);
const roleFile = process.argv.find(arg => arg.startsWith('model_instructions_file='));
if (!roleFile || !fs.statSync(JSON.parse(roleFile.slice('model_instructions_file='.length))).isFile()) process.exit(32);
for (const name of ['CODEX_APP_TOOLS_PIPE_PATH', 'CODEX_MCP_NODE_PATH', 'CODEX_PERMISSION_PROFILE', 'CODEX_SESSION_ID', 'CODEX_THREAD_ID']) {
  if (process.env[name]) process.exit(31);
}
const enabled = process.argv.find((arg) => arg.startsWith('mcp_servers.render-llm.enabled_tools='));
for (const tool of ['scene_get', 'scene_validate', 'scene_apply', 'undo', 'redo', 'color_batch', 'batch_start', 'batch_status', 'batch_cancel', 'render_start', 'render_status', 'render_cancel', 'render_image', 'video_export', 'video_start', 'video_status', 'video_cancel']) {
  if (!enabled?.includes(`"${tool}"`)) process.exit(29);
}

process.stderr.write(
  `FAKE_CODEX images=${images.length} prompt=${input.length} role=${input.includes('render-llm fixture role') ? 1 : 0} memory=${input.includes('fixture memory') ? 1 : 0}\n`,
);
process.stdout.write(`${JSON.stringify({ type: 'thread.started', thread_id: '11111111-1111-4111-8111-111111111111' })}\n`);
process.stdout.write(`${JSON.stringify({ type: 'turn.started' })}\n`);
for (const tool of ['exec', 'wait']) {
  for (const phase of ['started', 'completed']) process.stdout.write(`${JSON.stringify({type:`item.${phase}`,item:{id:`dispatch-${tool}`,type:'mcp_tool_call',server:'codex',tool,status:'completed'}})}\n`);
}
if (process.env.RENDER_LLM_CODEX_TEST_SERVER) process.stdout.write(`${JSON.stringify({type:'item.started',item:{id:'unauthorized',type:'mcp_tool_call',server:process.env.RENDER_LLM_CODEX_TEST_SERVER,tool:'read_secret'}})}\n`);
process.stdout.write(`${JSON.stringify({ type: 'item.started', item: { id: 'tool-1', type: 'mcp_tool_call', server: 'render-llm', tool: 'scene_get', arguments: {} } })}\n`);
process.stdout.write(`${JSON.stringify({ type: 'item.completed', item: { id: 'tool-1', type: 'mcp_tool_call', server: 'render-llm', tool: 'scene_get', status: 'completed', result: { ok: 1 } } })}\n`);
process.stdout.write(`${JSON.stringify({ type: 'item.completed', item: { type: 'agent_message', text: 'fixture answer' } })}\n`);
process.stdout.write(`${JSON.stringify({ type: 'turn.completed', usage: { input_tokens: 12, cached_input_tokens: 3, output_tokens: 2 } })}\n`);
