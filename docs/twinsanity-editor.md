# Twinsanity editor workbench

Decisions and plan for the Crash Twinsanity (PAL, PS2) recreation project built on
AetherCore. This document is the source of truth for the project's structure and
sequencing; the machine-local agent skill `aethercore-editor-framework-split`
carries the same decisions for agent sessions.

## Decisions (2026-09-24)

1. **Flavored editor, not a second editor binary.** One editor executable; a
   per-project flavor (`ProjectSettings.toml` → `[editor] flavor = "twinsanity"`)
   gates which panels register. Twinsanity-specific code lives in
   `src/editor/twinsanity/`.
2. **No DevGame target.** Content is authored in the editor; gameplay runs
   through the plain GameRuntime (`AetherGame.exe`). Profiling comes from Tracy
   (already compiled into Dev builds) plus game-side instrumentation added in
   gameplay code. The editor's localhost control endpoint (`capture_texture`,
   render-graph queries) stays available without the editor.
3. **Monorepo, not a submodule.** Editor flavor code, the PS2 extractor and the
   Twinsanity project's original code all live in this repo. A future split
   follows the phase 4 boundary (framework repo + game repo) when there is a
   second consumer of the framework - not before.
4. **ISO-derived content never enters git.** This repo is public. Extracted
   models/textures/audio/level data from the PS2 ISO live under
   `projects/Twinsanity/assets/` (gitignored) or local-only folders. Studying
   the original happens in PCSX2 (memory view, GS dumps); mechanics are
   reimplemented clean against the AetherCore C# scripting API.

## Repo layout when active

    AetherCore/
      src/editor/twinsanity/      flavor panels (original code, committed)
      tools/tw-extract/           PS2 asset extractor -> GLTF (committed)
      projects/Twinsanity/
        ProjectSettings.toml      [editor] flavor = "twinsanity" (committed)
        scripts/, scenes/         original work (committed)
        assets/                   EXTRACTED content (gitignored, never commit)

## Sequencing

1. Flavor mechanism: parse `editor_flavor`, expose it to the editor, gate panel
   registration on it, swap the ImGui theme per flavor (Twinsanity theme:
   saturated orange/violet palette, chunky rounded panels).
2. `src/editor/twinsanity/` skeleton with a first panel.
3. `tools/tw-extract/`: PS2 archive ripping -> GLTF/images, validated against
   PCSX2 GS dumps.
4. Panels grow with content. Priority order: reference overlay (PCSX2 capture
   beside `capture_texture` output) -> PS2 asset browser -> level rebuild canvas
   -> entity spawn sheet -> spline/set-piece editor.

## Extracting assets (`tools/tw-extract`)

`tw-extract` reads the untouched PAL disc directly (it refuses any image whose
PCSX2 CRC is not `1510E1D1`) and writes everything into the gitignored
`projects/Twinsanity/assets/`. It is a .NET Framework 4.8 console tool built on
the MIT `Twinsanity` library from a local CrashModded checkout
(`tools/twinsanity-editor`), which is referenced by path and never copied here.
It is not part of the CMake build.

    dotnet build tools/tw-extract -c Release -p:TwinsanityDll=<CrashModded>/tools/twinsanity-editor/Twinsanity/bin/Release/Twinsanity.dll
    tools/tw-extract/bin/Release/net48/tw-extract.exe --iso "<original PAL .iso>"

Run from the repo root. Options: `--out <assets dir>`, `--only <substring>`
(e.g. `Levels/Earth/Hub`) and `--cache <dir>` (unpacked archive files, default
`%TEMP%/tw-extract/<crc>`). A full run takes about a minute.

|Output|Contents|
|---|---|
|`textures/<hash>.png`|Every decoded texture, named by content, shared by all models|
|`scenery/<Area>/<Level>/<chunk>/<chunk>.gltf`|A chunk's static scenery in world space, one primitive per material|
|`scenery/.../<chunk>_sky.gltf`|The chunk's skydome|
|`scenery/.../<chunk>_dynamic.gltf`|Animated scenery pieces at their initial transforms|
|`objects/<Object>/<Object>[_<n>].gltf`|A game object's graphics: skeleton, skin, joint-attached rigid parts, and every animation the object's OGI slots reference as clips named `aNNN` by slot (25 fps, sampled with the Twinsanity editor's `AnimationController` maths). `_<n>` is one per graphics set; `@<chunk>` marks a differing model under a name another chunk already used|
|`objects/<Object>/<Object>.states.json`|Characters only: the behaviour scripts' `DoAnim` commands per state, giving the clip slot and blend-in time the game uses|
|`collision/<Area>/<Level>/<chunk>*.gltf`|The chunk's collision triangles, split by surface (deadly surfaces separate)|
|`levels/<Area>/<Level>/<chunk>.level.json`|Scenery, sky, collision pieces, object instances (position, rotation, the instance's float parameters) and the player spawn|
|`images/<path>/<stem>_NN.png`|Gallery / loading-screen pictures, as the tiles the disc stores them in|

Load any of them with the editor's Add to Scene or the MCP `add_model` tool.
Conversion notes:

- Game space is mirrored on X (as the Twinsanity editor does), so glTF is
  right-handed Y-up; scenery instance matrices are applied row-vector
  (`p * M`).
- Texture and vertex alpha is re-decoded from the raw GS data: the library
  stores `(byte)(a << 1)`, which turns opaque `0x80` into transparent `0`.
- Vertex colour goes out as `COLOR_0 = min(byte / 128, 1)` (the GS reads `0x80`
  as 1.0). Scenery typically sits around `0xB0`, so the overbright part is
  clamped.
- The particle texture pages (Startup/Default.rm2 ParticleData) ARE extracted, to
  `particles/particle_page_<0..2>.png` (128x128 each, point-sampled). The definitions
  themselves are not exported as data; the crate effects hard-code the disc values
  (from `logs/cratebreak/particles.json`) in `scripts/CrateFx.cs`. The rest of the
  frontend is not extracted yet. Dynamic-scenery rotation assumes an `(x, y, z, w)`
  quaternion and has not been checked against the game.

## Status

- Editor/framework split phases 1-3 landed.
- Flavor mechanism and the Reference Images panel landed (sequencing 1-2).
- `tools/tw-extract` landed (sequencing 3): the whole disc extracts with no
  failures; beach scenery, Crash, Aku Aku and a crab were checked in the editor.
