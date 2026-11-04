# Preview status

The repository is being prepared for external testing. It is not a stable release.

| Check | Current evidence |
| --- | --- |
| TypeScript agent, protocol, MCP and HTTP regression suite | 170/170 inherited cases plus the staging regression passed on Windows, 2026-11-03 |
| Provider calls | Not executed during this preparation; tests use controlled providers |
| Native build from pinned source dependencies and official shared Qt | Passed; document CTest passed |
| Native rendering and GPU diagnostics | 1280×720 stills decoded with actual DXR/oidn-cuda |
| Native MCP/HTTP integration | Edit/render/undo restored exact decoded RGB; both transports exported decoded PNG sequences |
| Native MP4 export | H.264, 384×216, 96 frames, 24 fps, 4 seconds; full decode passed |
| Native startup stability | A previously recorded intermittent startup failure remains unresolved |
| Source and binary redistribution | License/provenance inventory incomplete; no public binary release |

Earlier native tests are useful development evidence, but are not substituted for
testing this checkout and the executable built from it.

## Release requirements

1. Keep clean build, document validation, rendered-pixel and transport checks passing.
2. Resolve the recorded startup fault; a successful preview does not establish its root cause.
3. Verify upstream and dependency notices, plus required redistribution materials.

Generated output, dumps, chats, model transcripts and local work instructions stay
outside the repository. Test fixtures use fictional data.
