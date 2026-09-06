# Engine asset credits

Provenance for third-party assets shipped as part of the ENGINE itself (packed into
`data/engine.pak` via `EngineAssetsPak`, not a sample project's own content). Mirrors
the per-project convention (e.g. `projects/Sandbox/assets/CREDITS.md`) at the
engine-resources level - this file did not exist before; created for the first
asset under `resources/` that needed one.

All assets below are CC0 / Public Domain. Attribution is not legally required by
this licence but is recorded here per project convention.

## Kenney — Prototype Textures (v1.0)
- Source: https://kenney.nl/assets/prototype-textures
- Author: Kenney (www.kenney.nl)
- Licence: CC0 1.0 Universal (https://creativecommons.org/publicdomain/zero/1.0/)
- Files used (copied from `PNG/`), 3 of the pack's 78 PNGs - a neutral greybox
  handful, not the whole pack:
  - `Light/texture_02.png` -> `textures/prototype/grid_light.png` - clean grid
    lines on a light-grey field, closest match to the engine's existing flat
    default (`baseColorFactor` ~0.85/0.85/0.82) and the classic Unreal/Unity
    "prototype grid" look. **This is the one wired as the default material's
    albedo texture** (`ComponentCatalog.cpp`'s `AddMeshPrimitive`) - it reads
    scale, rotation and mirroring at a glance on any primitive shape.
  - `Light/texture_07.png` -> `textures/prototype/checker_light.png` - coarse
    checkerboard, light. Not wired anywhere yet; kept alongside the default so
    a material preset/inspector picker has an alternative greybox style
    (checkers make UV stretching and axis flips more obvious than grid lines
    do, at the cost of a less clean silhouette on curved surfaces).
  - `Dark/texture_02.png` -> `textures/prototype/grid_dark.png` - the same grid
    as the default, on a dark field. Not wired anywhere yet; a contrast
    variant for dark environments or objects that would otherwise wash out
    against the light default.
- Explicitly NOT imported: the pack's Green/Orange/Purple/Red colour variants,
  its `Stairs`/`Door`/`Window`/`Wall` reference-scale textures (indices 9-13 in
  each colour folder - useful for level greyboxing at a known real-world scale,
  not for a generic per-primitive default material), and the `Vector/` SVG/SWF
  originals.
