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

## Status

- Editor/framework split phases 1-3 landed (see git log; 5+ local commits).
- Flavor mechanism: NOT STARTED (this document is the agreed plan).
