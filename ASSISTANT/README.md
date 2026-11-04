# Agent and local integrations

This Node.js/TypeScript runtime connects the native editor to chat, MCP and HTTP.
The product role is in `prompts/render-llm-role.md`; it is part of the application,
not a set of development instructions for repository contributors.

```sh
npm ci --ignore-scripts
npm test
npm run build:runtime
```

The free regression suite uses a controlled editor and provider. Real rendering
requires the native application and staged resources. Start the editor and open a
scene before connecting an external host.

```sh
node dist/mcp-stdio.js --dir "<runtime>/Data/assistant"
```

For HTTP:

```sh
node dist/http-api.js --dir "<runtime>/Data/assistant" --port 8765
```

The HTTP host binds to loopback and creates its own token file. Do not commit,
print or share that file. Both hosts require exclusive access to the editor's
action channel; stop native chat before starting an external host.

See [the action contract](../docs/MCP.md) for all 17 tools, routes, request schemas
and revision handling. `scene_apply` requires the revision returned by `scene_get`;
refresh it after every edit, undo or redo.

The default native-chat adapter uses the user's own supported Codex installation.
This preparation does not run paid/live model calls. Retained legacy protocol and
Claude-adapter code is covered by regression tests and is not a promise that every
historical feature is exposed by the current native product.
