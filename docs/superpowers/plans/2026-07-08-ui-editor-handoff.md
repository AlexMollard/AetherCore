# UI Editor Feature — Handoff

Date: 2026-07-08
Branch: `master` (commit `4e8be345`, working tree clean)
Tests: 121/121 passing (`build-vs2022-msvc/tests/Debug/EngineTests.exe`, MSVC/Debug)

**Governing docs — read these first, in order:**
1. Design: [`docs/superpowers/specs/2026-07-07-ui-editor-design.md`](../specs/2026-07-07-ui-editor-design.md)
2. Plan (task-by-task, still the source of truth for remaining work): [`docs/superpowers/plans/2026-07-07-ui-editor.md`](2026-07-07-ui-editor.md)

This handoff exists because the working process changed mid-plan (dispatch model hit a weekly subagent limit; execution moved inline; the branch was also rebased onto `master` partway through, which silently dropped some already-completed work that had to be recovered — see "Gotchas" below). Everything here is a snapshot **on top of** the plan doc, not a replacement for it — the plan's remaining task descriptions (file lists, exact code, test snippets) are still accurate and should be followed as-is.

## Status vs. the plan

| Phase | Task | State |
|---|---|---|
| 0 | 0.1 Default scene template | ✅ Done |
| 0 | 0.2 SceneWorkflow (NewScene/QuickSave) | ✅ Done |
| 0 | 0.3 File menu + Ctrl+S | ✅ Done |
| 1 | 1.1 UI components | ✅ Done |
| 1 | 1.2 UiLayoutSystem (anchor resolution) | ✅ Done |
| 1 | 1.3 UiDrawCommand + UiDrawBuilder (image quads) | ✅ Done |
| 1 | 1.4 Serialization (v6 format) | ✅ Done |
| 2 | 2.1 ui_shapes.slang compiles | ✅ Done (already worked, no action needed) |
| 2 | 2.2 UiRenderer (buffer + pipeline + pass) | ✅ Done |
| 2 | 2.3 Wire into frame + texture-release hook | ✅ Done — **live-verified**: loaded a real scene, quads resolved to exact expected pixel rects, draw commands produced, `$UiOverlay` registered, zero Vulkan validation errors |
| 2 | (bonus, not in plan) Entity-0/null-entity ECS bug | ✅ Fixed at `World` construction — see Gotchas |
| 3 | 3.1 Offline SDF font baker (assetpack) | ✅ Done |
| 3 | 3.2 FontRegistry + ShapeText (runtime, CPU side) | ✅ Done — 5 shaping tests passing |
| 3 | **3.3 Emit glyph commands for UIText** | 🔶 **IN PROGRESS — stopped before writing any code.** Was mid-research into the GPU texture-upload path for the font atlas. **Nothing committed or edited for this task yet.** |
| 4 | 4.1–4.4 Editor (inspector, create menu, canvas panel, viewport handles) | ⬜ Not started |

**No uncommitted changes exist right now.** The next session can start Task 3.3 completely fresh from the plan doc.

## What Task 3.3 needs (per the plan, `2026-07-07-ui-editor.md` lines ~1188–1232)

Two independent halves:

1. **`UiDrawBuilder` glyph emission (pure, TDD-able, no GPU).** Change `BuildDrawCommands(World&, std::vector<UiDrawCommand>&, FontRegistry* fonts = nullptr)`. In the tree walk, when an entity has `UIText` and `fonts != nullptr`: `fonts->Load(text.fontName)` → `ShapeText(*font, text.text, text.pixelSize, rect.resolvedRect, text.wrap, (int)text.hAlign, (int)text.vAlign)` → for each `ShapedGlyph`, push a `kShapeSdfGlyph` command (`data0=glyph.rect`, `data1=glyph.uv`, `color=text.color`, `textureSlot=font->atlasBindlessSlot`, `layer=layer++`). The default-`nullptr` param keeps the existing image-only test/call site source-compatible. This half is straightforward — the exact test to add is written out in full in the plan doc.

2. **GPU atlas upload in `UiRenderer` (the harder half — this is where research stopped).** `FontRegistry::Load` (already implemented, `src/engine/ui/FontRegistry.cpp`) only parses `.fontmeta` metrics; `FontAsset::atlasBindlessSlot` is left `0xFFFFFFFF` by design (see the comment at the end of `FontRegistry::Load`). `UiRenderer` (which owns GPU resources) needs to additionally:
   - Read `engine://fonts/<name>-Regular.fontatlas` via `io::FileSystem::ReadFile` (same VFS call `FontRegistry::Load` already uses for `.fontmeta` — mirror it).
   - Parse the `FontAtlasHeader` (`magic/width/height`, defined in both `tools/assetpack/FontProcessor.hpp` and `src/engine/ui/FontAsset.hpp` — already written, already `static_assert`-matched, no new format work needed) and get a pointer to the trailing `width*height` R8 bytes.
   - Create a single-channel GPU texture and upload those bytes, then register it bindless to get a real slot.
   - Write that slot back into the cached `FontAsset` so `atlasBindlessSlot` stops being `0xFFFFFFFF`. **`FontRegistry` currently only exposes `const FontAsset* Get/Load`** — it will need a small mutable accessor added (e.g. `FontAsset* GetMutable(std::string_view name)`) for `UiRenderer` to poke the slot in after upload. This is a small, safe addition to `FontRegistry.hpp/.cpp` — do it as part of this task.

   **Grounding already done, ready to use:** the exact single-channel-texture upload pattern doesn't have a direct precedent (the codebase's existing texture loaders — `src/engine/material/Texture.cpp` — all use RGBA8 or BC-compressed formats), but the *building blocks* are confirmed and copy-paste-ready:
   - Texture create + bindless registration pattern: `Texture.cpp`'s `UploadRgbaToGpuImage` (lines ~39–79) — swap `gpu::Format::R8G8B8A8Srgb` for the single-channel equivalent (confirm the exact enum name, e.g. `gpu::Format::R8Unorm`, in `src/engine/gpu/GpuFormat.hpp` / `GpuEnums.hpp` — not yet confirmed).
   - The actual byte upload: `vkutil::HostCopyToImage(device, image, hostData, width, height)` (`src/engine/vulkan/VulkanUtils.hpp:54-95`) — **this was the exact point research stopped.** It was traced down to the Vulkan level and appears to be a raw `vkCopyMemoryToImage` (host-image-copy extension) that takes `hostData` + `width`/`height` and internally handles the layout transition — it does **not** take a bytes-per-pixel or format parameter explicitly at this call site, but the underlying image's own format (set at `CreateTexture` time via `TextureDesc.format`) governs how Vulkan interprets `hostData`'s layout, so it should work unchanged for an R8-format image with `width*height` (not `width*height*4`) bytes of source data — **this assumption is what needs verifying** (by reading a few more lines of context around `VkMemoryToImageCopy`/`vkCopyMemoryToImage` if the extension has any per-format stride assumptions, or just by trying it and checking for a Vulkan validation error on first run).
   - Then the same `OneShotCmd` + barrier + `RegisterTextureBindless` sequence as `UploadRgbaToGpuImage` lines 64–78, unchanged.
   - Where to trigger it: `UiRenderer::Init` (pre-warm "Roboto" once) is simplest for v1, matching the plan's "load 'Roboto' once" note — no need to build a lazy multi-font-upload path yet.

**If the R8 host-copy assumption doesn't hold** (i.e. `HostCopyToImage` turns out to assume 4-bytes-per-texel somewhere), the fallback is the staging-buffer + `CopyBufferToImage` path instead (`src/engine/gpu/CommandList.hpp:102`, used by `LocalShadowService.cpp:709-723` for its atlas — also grounded, also copy-paste-ready, just more code).

## Gotchas discovered this session (don't re-discover these)

1. **`core.autocrlf=true` silently corrupts committed binary files that don't match git's binary-content heuristic.** The baked `.fontmeta` was inflated 7676→10292 bytes on `git add` before this was caught. Fixed via a new root `.gitattributes` (`*.fontatlas binary`, `*.fontmeta binary`). **If you bake/commit any new binary asset format, add its extension to `.gitattributes` too**, or it will silently corrupt on commit.
2. **The `feature/ui-editor`→`master` branch move dropped uncommitted/orphaned work.** Task 3.1's actual commit (assetpack `bake-font` CLI wiring + FreeType CMake link) ended up unreachable from `master` — only some working-tree files survived. It was recovered via `git log --oneline --all` (found the orphaned commit `0f08bf51`), confirmed `master` never independently touched the same files (`git diff <merge-base> HEAD -- <files>` was empty), then `git checkout 0f08bf51 -- <files>`. **If anything else from the plan seems mysteriously "already partially done but not quite right" or missing wiring you'd expect, check `git log --oneline --all` for an orphaned commit before redoing the work from scratch.**
3. **`default.scene.toml` is a *live, editor-editable* file, not a frozen fixture.** Someone (organically, via the working New-Scene/Save-As feature from Phase 0) has already re-saved it with extra lights + a script attached to the Cube. `tests/scene/SceneWorkflowTests.cpp` originally hard-coded `entities.size() == 2`, which broke. It's been rewritten to search for the static-platform/dynamic-cube invariant instead of assuming a fixed count/index — **don't re-introduce a fixed-count assertion against this file.**
4. **The classic entt-id-0 gotcha is fixed at the source now** — `World`'s constructor (`src/engine/scene/World.cpp`) retires entt's raw-0 null slot via a create+destroy at construction, so `world.Create()` always returns a valid entity, even the very first one. The old per-test "burn entity 0 first" workarounds are gone from the test suite. **Do not re-add them** — if you see the old advice anywhere (this handoff included, in case it goes stale), check `WorldTests.cpp` first to see if it's already handled.
5. **Model/agent notes:** subagent dispatch via the `Agent` tool hit a **weekly limit** mid-task once (an account-level quota, not a per-task timeout). If it recurs, the fallback that worked well was finishing the interrupted task **inline** (direct Read/Edit/Bash tool calls in the main session) rather than re-dispatching — the user explicitly chose this mode ("continue inline") for the remainder of the plan after the first cutoff.

## Working conventions in force for this branch (per explicit user instruction this session)

- **Work happens on `master` directly.** Not `feature/ui-editor` (that branch's history is now orphaned/abandoned — don't merge it back, its useful commits have already been cherry-picked/recovered where needed).
- **Commit locally only. Do NOT push.** (Earlier in this feature's history, before the branch move, pushes to `origin/feature/ui-editor` were happening after every verified task — that convention no longer applies post-move. Confirm with the user before pushing anything.)
- Build/test command: `cmake --build build-vs2022-msvc --target EngineTests --config Debug` then `./build-vs2022-msvc/tests/Debug/EngineTests.exe`. (`build-ninja-clang`/clang-cl has a separate pre-existing unrelated breakage — a fix was spawned as a background task early in this feature's history; check if it landed before assuming clang-cl works.)
- Subagent-driven execution (fresh implementer + spec-compliance reviewer + code-quality reviewer per task, per `superpowers:subagent-driven-development`) was the original mode for this whole plan and produced consistently high-quality, well-grounded, thoroughly-tested results — including catching several real bugs (a stale-frame-slot race, a silent SDF-vs-plain-coverage FreeType mis-render, the entt id-0 collision, a texture refcount leak) that would have been easy to miss. **Prefer resuming that mode for Phase 4** (the remaining editor UI work) if the subagent quota allows — it's a good fit for that phase's more independent, less GPU-research-heavy tasks.

## Immediate next step

Resume Task 3.3 exactly as scoped in the plan doc (`docs/superpowers/plans/2026-07-07-ui-editor.md`, "Task 3.3: Emit glyph commands from UIText"), using the grounding notes above to shortcut the GPU-upload research. Suggested order: implement + TDD the `UiDrawBuilder` half first (fast, safe, no GPU risk), commit, then tackle the `UiRenderer` atlas-upload half, verify live in the editor (a `UIText` "Hello" should render as crisp SDF text over the viewport — the plan's own manual-verification bar for this task), then commit and continue to Phase 4.
