# MCP action contract

## Implemented transport

Native chat registers the tools below in its in-process MCP server. The same tool
definitions are exposed by `ASSISTANT/dist/mcp-stdio.js --dir <Data/assistant>` over
stdio. Scene/history handlers submit `document.*` through the existing editor protocol
and `Native/RenderLlmActionLayer`; color batches freeze that same canonical document
and use isolated native render instances; video freezes it and uses the retained
native H.264 movie path.

The external host takes an exclusive PID/token lock and refuses to start while native
chat owns the channel. Its action-only protocol mode does not delete preview/render
jobs, emit chat records, or publish a brain heartbeat. The stdio handshake and exact
seventeen-tool catalog are tested. A real stdio client also completed read, color edit,
observe and byte-exact undo against the native editor in its existing test mode; normal
launch still requires a valid open project. See [STATUS.md](STATUS.md).

All tools return MCP text content and structured content derived from the editor
operation result, including `success` and any top-level editor `error`. They set
`isError` on validation, transport, or action failure. Scene documents are canonical
version-1 `.renderllm` JSON. The native boundary enforces its 16 MiB byte limit and
recursive credential rejection.

| Tool | Arguments | Result and side effects |
|---|---|---|
| `scene_get` | none | Read-only canonical `document` plus current `revision`. |
| `scene_validate` | `document: string` | Read-only canonical document; rejects invalid schema or credentials. |
| `scene_apply` | `document: string`, `expected_revision: nonnegative integer` | Transactional scene replacement; records undo state and returns document/object count/new revision. A stale revision fails before mutation. |
| `undo` | none | Restores the preceding native document revision and advances revision; fails unchanged when history is empty. |
| `redo` | none | Restores the next native document revision and advances revision; fails unchanged when history is empty. |
| `color_batch` | `object_id`, exactly four `colors`, `expected_revision`, bounded dimensions/samples, `denoise` | Freezes the current canonical scene at the requested revision and renders each color in an isolated native Flame process. Returns a manifest and all PNGs. |
| `batch_start`, `batch_status`, `batch_cancel` | Start uses the `color_batch` arguments; status/cancel use `job_id` | Background lifecycle for the same frozen four-variant batch. Successful status returns all four actual PNGs. |
| `render_start` | `expected_revision`, bounded dimensions/samples, `denoise` | Freezes the canonical scene, starts a bounded background Flame render, and returns a job snapshot immediately. |
| `render_status` | `job_id` | Returns queued/running/cancelling/succeeded/failed/cancelled state. On success, returns metadata and the actual PNG. |
| `render_cancel` | `job_id` | Signals only that job's isolated renderer and returns its current state. It does not mutate the scene. |
| `render_image` | `expected_revision`, bounded dimensions/samples, `denoise` | Freezes the canonical scene and renders one PNG in an isolated native Flame process. Returns the actual image plus hash, dimensions, executed backend and denoiser. |
| `video_export` | `expected_revision`, bounded even dimensions, samples, `denoise` (default `true`), fps, frame count, optional `format` (`mp4` default or `png-sequence`) | Freezes the scene and uses the retained native frame loop. MP4 returns `video` and `video_sha256`; PNG sequence returns `sequence` and an `images` array with index, relative path, SHA-256 and bytes per frame. Both include timing and executed backend/denoiser. |
| `video_start`, `video_status`, `video_cancel` | Start uses the `video_export` arguments; status/cancel use `job_id` | Background lifecycle for the same frozen MP4 or PNG-sequence export. Completed numbered PNGs survive cancellation; incomplete temporary frames are not reported as completed. |

These tools do not accept shell commands or filesystem paths. `scene_apply`, `undo`,
and `redo` mutate only the open supported scene through the validated transaction
boundary. Authorization follows native-chat tool permission or explicit local stdio
process launch. `color_batch` and `render_image` return rendered PNGs as MCP image
content so a native visual agent receives actual pixels. `video_export`
returns metadata and a confined relative path because MCP has no video content block.
For numbered PNGs, pass `format: "png-sequence"`; the same optional field is accepted
by HTTP `POST /video` and `/video/start`. File paths remain host-generated, not inputs.

## Local HTTP API

`ASSISTANT/dist/http-api.js --dir <Data/assistant> [--port 8765]` binds only to
`127.0.0.1`. It generates a 256-bit bearer token at
`Data/assistant/renderllm-http.token`, does not print the token, disables response
caching, and shares the external action-host lock with stdio MCP. Every request must
send `Authorization: Bearer <token>`.

| Method and route | JSON body | Shared action |
|---|---|---|
| `GET /scene` | none | `scene_get` |
| `POST /scene/validate` | `{ "document": "..." }` | `scene_validate` |
| `PUT /scene` | `{ "document": "...", "expected_revision": 7 }` | `scene_apply` |
| `POST /undo` | `{}` | `undo` |
| `POST /redo` | `{}` | `redo` |
| `POST /batch/color` | `{ "object_id": "cube", "colors": ["#FF0000", "#0066FF", "#FFD700", "#00CC88"], "expected_revision": 7, "width": 384, "height": 216, "samples": 4, "denoise": true }` | `color_batch` |
| `POST /batch/start`, `/batch/status`, `/batch/cancel` | Start uses the color body; status/cancel use `{ "job_id": "batch-<uuid>" }` | Matching batch lifecycle action |
| `POST /render/start` | Same body as `/render/image` | `render_start` |
| `POST /render/status` | `{ "job_id": "render-<uuid>" }` | `render_status` |
| `POST /render/cancel` | `{ "job_id": "render-<uuid>" }` | `render_cancel` |
| `POST /render/image` | `{ "expected_revision": 7, "width": 384, "height": 216, "samples": 4, "denoise": true }` | `render_image` |
| `POST /video` | `{ "expected_revision": 7, "width": 384, "height": 216, "samples": 4, "denoise": true, "fps": 24, "frames": 96 }` | `video_export` |
| `POST /video/start`, `/video/status`, `/video/cancel` | Start uses the video body; status/cancel use `{ "job_id": "video-<uuid>" }` | Matching video lifecycle action |

Bodies must be strict JSON and are bounded before the native 16 MiB document check.
Unknown routes return `404`. Editor action failures return their structured result
with a non-2xx status.

## Verification

The current Windows preview passed the native document CTest, the inherited 170-case agent/protocol suite and a staging regression that excludes mutable editor state. A real native MCP client changed rendered pixels and undo restored the original decoded RGB hash. MCP and authenticated loopback HTTP each exported and decoded a three-frame PNG sequence without changing the live document. Native MP4 export also produced 96 H.264 frames at 24 fps and passed a full decode.

These checks do not exercise a live provider or establish the cause of the previously recorded intermittent startup fault. See [STATUS.md](STATUS.md) and the source of the [native integration check](../ASSISTANT/test/run-studio-native.mjs).
