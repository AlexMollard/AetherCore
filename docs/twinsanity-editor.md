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
(e.g. `Levels/Earth/Hub`), `--cache <dir>` (unpacked archive files, default
`%TEMP%/tw-extract/<crc>`), `--music <n,n,...|all>` (extra `MUSIC.MH`
tracks), `--voice <n,n,...>` (`ENGLISH.MB` speech tracks), `--movies <NAME,...>`
with `--ffmpeg <exe>` (FMV frames + audio; ffmpeg runs at extract time only) and
`--hd-pack <zip>` (CRASHARKI's `ctwin-tp` PCSX2 replacement pack: every disc
texture, UI sprite and font page with a matching pack image is written as the HD
art instead). A full run takes about a minute. After extracting, bake with
`AssetPacker bake-all projects/Twinsanity`.

|Output|Contents|
|---|---|
|`textures/<hash>.png`|Every decoded texture, named by content, shared by all models|
|`scenery/<Area>/<Level>/<chunk>/<chunk>.gltf`|A chunk's static scenery in world space, one primitive per material|
|`scenery/.../<chunk>_sky.gltf`|The chunk's skydome|
|`scenery/.../<chunk>_dynamic.gltf`|Animated scenery pieces at their initial transforms|
|`objects/<Object>/<Object>[_<n>].gltf`|A game object's graphics: skeleton, skin (rigid joint-attached parts are merged into the skin, weighted fully to their joint, so GPU skinning carries them), and every animation the object's OGI slots reference as clips named `aNNN` by slot (25 fps, sampled with the Twinsanity editor's `AnimationController` maths). `_<n>` is one per graphics set; `@<chunk>` marks a differing model under a name another chunk already used|
|`objects/<Object>/<Object>.states.json`|Characters only: the behaviour scripts' `DoAnim` commands per state, giving the clip slot and blend-in time the game uses|
|`collision/<Area>/<Level>/<chunk>*.gltf`|The chunk's collision triangles, split by surface (deadly surfaces separate)|
|`levels/<Area>/<Level>/<chunk>.level.json`|Scenery, sky, collision pieces, object instances (position, rotation, the instance's float parameters) and the player spawn|
|`images/<path>/<stem>_NN.png`|Gallery / loading-screen pictures, as the tiles the disc stores them in|
|`audio/sfx/<Area>/<Level>/<id>.wav`, `sounds.json`|A level's sound bank (SPU ADPCM decoded to 16-bit WAV at its own rate) plus `sounds.json`: every clip, each object's sound slots, every sound/music script command with raw arguments, and the level's streams. `Startup/Default` is the shared crate/pickup bank, `Startup/Frontend` the menu sounds|
|`audio/music/track_<n>.wav`|The `MUSIC.MH` streams the extracted levels' `act_DJ` (music) and `act_GLOBAL_AMBIENT_SOUND_*` (ambience bed) actors name in their instance params. Beach: 27 (title theme) and 89 (surf)|
|`audio/voice/track_<n>.wav`|`ENGLISH.MB` speech the cutscene scripts play (command 185)|
|`movies/<NAME>.wav`, `movies/<NAME>/fNNNNN.jpg`|Pre-rendered FMVs (`/FMV/<NAME>.PSS`): English audio track and 25 fps frames, played by `TwinsanityMovie.cs`|

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
  (from `logs/cratebreak/particles.json`) in `scripts/CrateFx.cs`. Disc conventions, shared
  through `CrateFx.CK` / `CrateFx.DiscUv`: colour and alpha 0x80 = 1.0; the texture rect's
  V runs up the page (the PNG is top-down); GenSort Radial / ImprovedRadial map to the
  emitter's `emit_shape`. The rest of the
  frontend is not extracted yet. Dynamic-scenery rotation assumes an `(x, y, z, w)`
  quaternion and has not been checked against the game.
- HD pack matching is perceptual, not by PCSX2's file-name hash: that hash covers the
  texture's GS memory at runtime (DBW/CSA/TBW as the game sets them), which the disc
  does not reproduce. `HdPack.cs` compares 32x32 alpha-premultiplied thumbnails (pack
  images flipped upright, GS alpha widened) and accepts a pair only when it is close
  (<= 8.5 per channel) and clearly ahead of the next candidate.

## Level bake (being retired)

Replaced by **Level convert** below; the bake and its `Twinsanity Marker` binding are deleted when
the converted scenes land.

The beach is pre-built in the editor rather than at Play. The flavor's **Level Bake**
panel (or the `twinsanity.bake_level` control method) runs the same builder
`TwinsanityLevel` runs at Play and saves the result as the gitignored
`assets/prefabs/beach.prefab.toml`. Every baked entity carries a `Twinsanity Marker`
component (its role and disc identity), and at Play the scripts bind to those
entities instead of spawning them, so a hand-placed copy behaves like a baked one.

`scenes/Beach.scene.toml` holds a linked instance of that prefab, so edits to baked
entities save as per-entity overrides in the committed scene. A re-bake keeps them;
an override whose entity no longer exists is dropped with a warning. Without the
prefab (a fresh checkout), `TwinsanityLevel` builds the level from the extracted
JSON as before. Re-bake after any re-extraction that changes level content.
After editing the bake scripts, press Play then Stop once before baking: the bake
runs the last-loaded script assembly, not the scripts on disk.

## Level convert

The hub becomes ordinary AetherCore content: one prefab per object family, one scene per area,
and a world scene that stitches the areas together. It is a **one-time migration**. After it,
the editor scenes and prefabs are the source of truth, and a re-extract refreshes only models,
textures and audio (prefabs and scenes reference them by path).

Run it from the flavor's **Level Convert** panel or the `twinsanity.convert` control method
(`{mode: "write"|"report", areas?: [scene...], overwrite?: [name...|"*"]}`). Save your scene first:
the command replaces the live world, and it refuses to run with unsaved edits or during Play. It
reloads the scene that was open when it finishes. After editing `TwinsanityConvert.cs`, press Play
then Stop once so the command runs the new assembly.

What it writes:

|Output|Contents|
|---|---|
|`assets/prefabs/tw_*.prefab.toml`|One per family and model variant: `tw_crate_*`, `tw_wumpa*`, `tw_gem_*`, `tw_push_*`, `tw_prop_*`, `tw_critter_*`, `tw_sled*`, `tw_spawner_*`, `tw_agent_*`, `tw_trigger`, `tw_spawn`. The root holds the model and a descriptor script (`TwCrate`, `TwActor`, `TwSpawner`, `TwAgent`, `TwTrigger` or `TwSpawn`, see `scripts/TwinsanityObjects.cs`), whose defaults are the family's own data, so a placed copy behaves as that family. Committed.|
|`scenes/HubBeach` `HubA` `HubB` `HubC` `HubD` `Pier` `HighPath` `BossArea` `AlwaysOn`|One per hub chunk: its scenery and `Collision` pieces (tags `tw_collision`, `tw_deadly`, `tw_drown`) under `Area <chunk>`, plus one linked prefab instance per disc instance. The instance's disc data (area, layer, id, flags, floats, params...) is a root override of its descriptor. Its links and trigger targets are entity references to other instance roots, and its `points`/`path` are `Point N`/`Path N` children, so moving the instance moves them. Committed.|
|`scenes/Beach`|The world scene: environment, `Level`, `Crash`, the `Sky` (tag `tw_sky`), the primary `tw_spawn`, and `[[includes]]` of every area scene, which keeps the hub one seamless Play.|
|`.aether/convert/manifest.json`|The C# half's output, read by the C++ half. Machine-local.|

- **Where the families come from.** `TwinsanityConvert.cs` classifies every instance exactly as
  the old bake did, and maps each family to a prefab through its checked-in `Catalogue` table. A
  family that is missing from the table fails the conversion with its name. Add a row; there is
  no generic fallback.
- **Write mode.** An existing prefab or scene is kept unless it is named in `overwrite` ("Overwrite
  all" in the panel). Re-running is byte-stable: instance node ids are hashes of
  `<chunk>#<layer>#<id>`, prefab guids are carried over by entity name path, and chunk and instance
  order are fixed.
- **Report mode.** Nothing is written but `.aether/convert/drift-<scene>.md`. These files list what
  the current extract would change: missing or extra instances, moves over 1 cm or 0.5°,
  descriptor property and link differences, points, static pieces, and prefabs that differ from a
  fresh template. Apply the changes you want by hand in the editor.
- **Editing.** Edit an area in its own scene. Entities included into `Beach` are transient there and
  are never saved into it.
- **Texture paths are stable.** Committed prefabs and scenes name
  `assets/textures/<hash>.png`. tw-extract derives the hash from the disc texture's own
  size and decoded pixels (`tools/tw-extract/Convert.cs`, `TextureStore.Name`). `--hd-pack` only
  writes the HD art under that same name, so a re-extract with or without the pack keeps every
  reference valid.
- **Lighting notes.** The world scene is saved by the editor now, and the editor does not keep
  comments. The notes that explained its `[environment]` values are in `scenes/Beach.scene.toml` as of
  commit `cc05231e`.

## Status

- Editor/framework split phases 1-3 landed.
- Flavor mechanism and the Reference Images panel landed (sequencing 1-2).
- `tools/tw-extract` landed (sequencing 3): the whole disc extracts with no
  failures; beach scenery, Crash, Aku Aku and a crab were checked in the editor.
- Beach playable end to end: crates and creatures, cutscenes (director scenes and
  FMVs, with speech and a hold-to-skip prompt), audio, Aku Aku, cannon and sled,
  pre-baked level, HD textures.
