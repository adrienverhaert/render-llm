# The `.renderllm` scene format

`CppProject/Native/RenderLlmDocument` validates the canonical document;
`RenderLlmDocumentBridge` maps it to the native editor and timeline. There is no
second renderer behind the document model.

## Structure

Version 1 requires exactly six top-level fields:

```json
{
  "version": 1,
  "agent": {},
  "animation": {"schema": 1, "fps": 24, "frame_count": 96, "tracks": [], "constraints": []},
  "materials": {},
  "render": {"schema": 2, "renderer": "UltraFlame", "samples": 24},
  "scene": {"objects": [], "world": {"color": "#000000", "strength": 1}}
}
```

Files must end in `.renderllm` and be at most 16 MiB. Unknown versions, unsupported
fields, invalid IDs or references, parent cycles and credential-shaped fields are
rejected before mutation. Numeric ranges and per-object properties are validated.
Unsupported non-empty animation constraints are rejected instead of ignored.

Saved documents use deterministic, recursively key-sorted JSON ending in one LF.
`QSaveFile` provides atomic replacement. The native document CTest exercises
canonicalization, semantic validation, credential rejection and disk round trips.

## Renderers and animation

`render.schema: 2` names the retained D3D11 renderer **Flame** and the retained DXR
path tracer **UltraFlame**. Older documents without that schema field interpret
`Flame` as the former Cinema path and normalize to schema 2 / `UltraFlame`.
Execution results report the backend and denoiser that actually ran.

Animation tracks address stable object/material IDs. Supported tracks include
transforms, visibility, material color/metallic/roughness/emission, camera
properties and light color/power/size. World tracks use the `$world` target.
Frames must be integral, ordered and within the declared frame count; retained
interpolation names and all four Bézier handles are preserved.

World settings include ambient/sun colors, environment orientation and fog.
Render settings include bounces, noise threshold, denoise, exposure, overscan and
tiled output. Missing optional fields use the retained defaults.

Area lights are oriented squares: `light.size` is the side length and local +Y is
the one-sided emitting normal. The retained environment path supports one sun.
Replacing the document invalidates the native scene-generation cache.

## Revisions, history and files

`scene_get` returns the canonical document and current revision. `scene_apply`
requires `expected_revision`; a stale value fails before mutation. The complete
document is validated before replacing the scene. If post-apply verification
fails, the bridge restores the previous canonical snapshot.

Undo and redo use a bounded 64-revision history. Each successful transition
advances the exposed revision. Render, batch and video jobs operate on frozen
snapshots rather than following subsequent live edits.

Native save/open operations accept a filename rather than an arbitrary path.
Names are bounded, require `.renderllm`, and reject path components, drive
separators and leading dots. Files stay under `Data/renderllm-projects`;
symbolic-link roots and targets are rejected. The native project dialog uses the
same boundary.

See [MCP.md](MCP.md) for the action and HTTP contracts, [DEMO.md](DEMO.md) for a
walkthrough, and [STATUS.md](STATUS.md) for executed checks and remaining limits.
