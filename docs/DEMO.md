# Reproduce the scene workflow

The demo uses the actual native editor. There is no browser-based renderer or
hosted AI endpoint in this repository.

1. Build the Windows application following [BUILD.md](BUILD.md).
2. Open `examples/studio-bounce.renderllm` in the editor.
3. Render a still and inspect the image and execution diagnostics.
4. Change the sphere's material, render again, then undo the change.
5. Keep the editor open and stop native chat before connecting MCP or HTTP.

## MCP

Register a local stdio server in your MCP client's configuration. Use absolute
paths for both the script and runtime directory; JSON paths use `/` or escaped
backslashes.

```json
{
  "mcpServers": {
    "render-llm": {
      "command": "node",
      "args": [
        "<checkout>/ASSISTANT/dist/mcp-stdio.js",
        "--dir",
        "<runtime>/Data/assistant"
      ]
    }
  }
}
```

Call `scene_get`, copy its `document`, and call `scene_validate` with the changed
document. Apply it with `scene_apply` and the returned `revision` as
`expected_revision`. Then call `render_image` with the new revision. The result
includes actual PNG image content, not just a file path for the model to infer.

Call `undo`, read the scene again, and compare it with the original document.
The [action contract](MCP.md) lists image dimensions, sampling bounds and media-job
status/cancel methods.

## HTTP

Run the local host described in [ASSISTANT/README.md](../ASSISTANT/README.md). Read
its generated token locally and send it as a bearer token; do not paste it into a
README, issue or screenshot. `GET /scene` returns the current document and revision.
`PUT /scene` accepts `document` and `expected_revision`; `POST /undo` accepts `{}`.
The full route table is in [MCP.md](MCP.md).

The HTTP and MCP entry points use the same validated actions as native chat. A
successful simulated test proves the transport contract; a native render is a
separate check.
