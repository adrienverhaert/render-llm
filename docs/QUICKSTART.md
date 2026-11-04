# Run Render LLM

## Requirements

- Windows x64 with a Direct3D-compatible graphics driver.
- [Microsoft Visual C++ x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- [Node.js 22 or newer](https://nodejs.org/) for MCP, HTTP, export jobs and native chat.
- A supported, signed-in Codex installation for native chat.

## Open the editor

1. Extract the whole ZIP into a writable directory. Keep the DLLs, `Data`,
   `platforms` and `ASSISTANT` folders beside `render-llm.exe`.
2. Run `render-llm.exe`.
3. Open `Data/renderllm-projects/studio-bounce.renderllm` with the project picker.
4. Use **Look**, **Objects** and **Motion** to edit the scene. Save changes with
   **Ctrl+S** and export through **Output**.

## Connect a client

Stop native chat before connecting an external client. For MCP, launch
`node ASSISTANT/dist/mcp-stdio.js --dir <extracted-directory>/Data/assistant`
from the extracted directory. The editor must remain open.

For HTTP, launch
`node ASSISTANT/dist/http-api.js --dir <extracted-directory>/Data/assistant`.
The host binds to loopback and prints its listening address. Use the generated
`Data/assistant/renderllm-http.token` as a bearer token; keep it private.

Scene files contain authored scene data. Provider credentials are configured
separately and must never be placed inside a project or shared ZIP.

## Notices

Read `LICENSE`, `LICENSING.md` and `UPSTREAM-NOTICE.txt` before redistribution.
