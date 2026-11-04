# Verification

Windows x64 technical preview. The checks below distinguish source tests, native
execution and package integrity.

| Check | Current evidence |
| --- | --- |
| TypeScript agent, protocol, MCP and HTTP regression suite | 170 existing cases and three focused regressions passed on Windows |
| Provider calls | Regression tests use controlled providers; live provider calls require separate verification |
| Native build from pinned source dependencies and official shared Qt | Passed; document CTest passed |
| Native rendering and GPU diagnostics | 1280×720 stills decoded with actual DXR/oidn-cuda |
| Native MCP/HTTP integration | Edit/render/undo restored exact decoded RGB; both transports exported decoded PNG sequences |
| Native MP4 export | H.264, 384×216, 96 frames, 24 fps, 4 seconds; full decode passed |
| Extracted Windows package | 5,250 files matched their recorded hashes; startup capture, 1280×720 GPU still and 96-frame MP4 passed |
| Native startup stability | A previously recorded intermittent startup failure remains unresolved |
| Source and binary redistribution | License/provenance inventory incomplete; no public binary release |

## Reproduce the extracted package checks

After following [BUILD.md](BUILD.md), run the same checks against the extracted
runtime rather than the build directory:

```powershell
./tools/test-native-startup.ps1 -Runtime <extracted-runtime> -Runs 1
./tools/test-still-export.ps1 -Runtime <extracted-runtime>
./tools/test-video-export.ps1 -Runtime <extracted-runtime>
```

The tested executable's SHA-256 is
`68f4d551b711676e2bba108211f78c7464f5a72936db389ee6398acce2240298`.
The tested ZIP's SHA-256 is
`a68874740cb38630ebf6ac6e242c774189d8613f5d7f3b11706cc50e73316e3e`.
The still reported DXR and OIDN-CUDA. Startup completed with exit code zero and a
decoded capture; this single run does not resolve the intermittent fault below.

## Release requirements

1. Keep clean build, document validation, rendered-pixel and transport checks passing.
2. Resolve the recorded startup fault; a successful preview does not establish its root cause.
3. Verify upstream and dependency notices, plus required redistribution materials.

See [SECURITY.md](../SECURITY.md) for private vulnerability reporting and
[LICENSING.md](../LICENSING.md) for redistribution terms.
