# INKBOUND UI Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build INKBOUND's Title, Level Select, and Settings screens as one MCP-authored, atmospheric menu system — after first enabling the engine's MCP to author UI entities.

**Architecture:** One `Menu` scene holds a shared `MenuShell` plus three screen-root subtrees toggled by a `MenuController` state machine. UI *structure* is authored as persisted scene entities (via the MCP, which Phase 0 makes possible); *behavior and animation* live in focused C# scripts that find authored entities by name and drive them. Instant state transitions keep the atmosphere continuous.

**Tech Stack:** C++20 engine (reflection/serde, Vulkan UI renderer), C#/CoreCLR game scripts (`AetherGame` assembly), TOML scenes, the AetherCore MCP for authoring + verification, Pillow for generated UI textures.

## Global Constraints

- Ink accent is cyan `#4DD9FF` → `[0.302, 0.851, 1.0, 1.0]` (linear-ish RGBA used across UI). Horror reds `#7a1f1f`, `#6a2a2a`.
- Headings/labels use font `PixelStorm` (already baked). The italic "voice" uses `IBMPlexMonoItalic` (baked in Phase 1).
- Settings persist to `%LOCALAPPDATA%/AetherCore/INKBOUND/settings.json`; never write settings into the repo.
- Level unlock: nodes 1–4 → `Level1`–`Level4` all unlocked; nodes 5–6 locked placeholders.
- Full-window composition (no device bezel); preserve the storyboard's off-center, left-aligned layout.
- Wire what's wireable: Ink Glow scales the cyan ink glow read by `InkBlock`/`AetherInk`; Screen Shake sets a flag; Music/SFX persist but stay inert (no audio system).
- Engine build: `cmake --build build/vs2022-msvc --target Editor` (Debug). Run the editor from the build-tree root `build/vs2022-msvc` with `AETHER_CONTROL_PORT=8787` (shaders are CWD-relative).
- The MCP editor is already the control endpoint at `127.0.0.1:8787`; verification uses `screenshot`, `send_input`, `get_console_log`, `save_scene`, `load_scene`.
- Do not modify the existing ink mechanic (`AetherInk.cs`, `InkBlock.cs`) except the one-line Ink-Glow read in Phase 4.
- Commit style: plain imperative subject, no prefixes/attribution (see repo CLAUDE.md).

---

## Phase 0 — Engine: make UI entities MCP-authorable

**Why:** The MCP can't author UI today. `UI Image` isn't reflected (so `add_component`/`set_component`/Inspector don't know it), and all four UI components are `AE_NOT_ADDABLE()`. Serialization already works via `serde/UiImageSerde.cpp`; only reflection + addability + a path→handle field are missing.

### Task 0.1: Give `UIImage` a texture path + lazy-resolve flag

**Files:**
- Modify: `src/engine/ui/UiComponents.hpp` (the `UIImage` struct, ~line 35)

**Interfaces:**
- Produces: `aether::ui::UIImage::texturePath` (std::string), `aether::ui::UIImage::textureDirty` (bool) — consumed by the draw builder, reflection, native setter, and serde.

- [ ] **Step 1: Add the fields to `UIImage`.** Change the struct to:

```cpp
struct UIImage
{
    glm::vec4 color{1.f};
    float cornerRadius = 0.f;
    TextureHandle texture{};
    // Authored texture path; resolved to `texture` lazily by the draw builder when
    // `textureDirty` is set. Every other texture-bearing component stores the path
    // (SpriteRenderer, TileMap); this brings UIImage in line so reflection/MCP can set it.
    std::string texturePath;
    bool textureDirty = false;
    // Pixel-art sampling: the UI shader snaps UVs to texel centres.
    bool pixelArt = false;
};
```

- [ ] **Step 2: Build the engine to confirm it still compiles.**

Run: `cmake --build build/vs2022-msvc --target Editor`
Expected: builds (a struct field add is source-compatible; the serde still copies `color/cornerRadius/pixelArt`).

- [ ] **Step 3: Commit.**

```bash
git add src/engine/ui/UiComponents.hpp
git commit -m "Add authored texturePath to UIImage"
```

### Task 0.2: Resolve `texturePath` → handle lazily in the draw builder

**Files:**
- Modify: `src/engine/ui/UiDrawBuilder.cpp` (`Walk`, ~line 60–90; `EmitImage`, ~line 12)

**Interfaces:**
- Consumes: `UIImage::texturePath`, `UIImage::textureDirty`, `TextureRegistry::Acquire(std::string)`, `TextureRegistry::Release(TextureHandle)`.

- [ ] **Step 1: Resolve before emitting.** In `Walk`, where it does `if (const auto* img = world.TryGet<UIImage>(entity))`, change to a mutable get and resolve first:

```cpp
if (auto* img = world.TryGet<UIImage>(entity))
{
    if (img->textureDirty && textures != nullptr)
    {
        if (img->texture.IsValid())
        {
            textures->Release(img->texture);
        }
        img->texture = img->texturePath.empty()
            ? TextureHandle{}
            : textures->Acquire(img->texturePath);
        img->textureDirty = false;
    }
    EmitImage(*rect, *img, textures, layer, out);
}
```

(Keep the existing `rect`/`layer` plumbing exactly as it was; only the image branch changes. If `Walk`'s signature has `const World&`, widen it to `World&` — it already gets a mutable `World&` from `BuildDrawCommands`.)

- [ ] **Step 2: Build.**

Run: `cmake --build build/vs2022-msvc --target Editor`
Expected: builds.

- [ ] **Step 3: Commit.**

```bash
git add src/engine/ui/UiDrawBuilder.cpp
git commit -m "Resolve UIImage texturePath to a handle at draw time"
```

### Task 0.3: Keep the native setter and serde in sync with `texturePath`

**Files:**
- Modify: `src/app/scripting/interop/UiExports.cpp` (`aether_ui_set_image_texture`, ~line 152)
- Modify: `src/app/scene/serde/UiImageSerde.cpp` (`CaptureUiImage`, `ApplyUiImage`)

**Interfaces:**
- Consumes: `UIImage::texturePath`, `UIImageRecord::texturePath`.

- [ ] **Step 1: Native setter also stores the path.** In `aether_ui_set_image_texture`, set the path alongside the handle:

```cpp
if (path == nullptr || path[0] == '\0')
{
    img->texture = aether::TextureHandle{};
    img->texturePath.clear();
    img->textureDirty = false;
    return;
}
img->texturePath = path;
img->texture = ctx.assets->GetTextureRegistry().Acquire(path);
img->textureDirty = false;
```

- [ ] **Step 2: Serde copies the member directly.** In `CaptureUiImage`, replace the `TryGetPath` block with a plain copy:

```cpp
ir.color = im->color;
ir.cornerRadius = im->cornerRadius;
ir.pixelArt = im->pixelArt;
ir.texturePath = im->texturePath;
```

In `ApplyUiImage`, set the path and let the draw builder resolve (drop the eager Acquire, keep the asset-DB registration so the bake pipeline still sees the dependency):

```cpp
ui::UIImage im;
im.color = c.rec.uiImage->color;
im.cornerRadius = c.rec.uiImage->cornerRadius;
im.pixelArt = c.rec.uiImage->pixelArt;
im.texturePath = c.rec.uiImage->texturePath;
im.textureDirty = !im.texturePath.empty();
if (!im.texturePath.empty() && c.deps.assetDatabase != nullptr)
{
    c.deps.assetDatabase->Register(MakeTextureSource(im.texturePath));
}
c.world.Emplace<ui::UIImage>(c.entity, im);
```

- [ ] **Step 3: Build.**

Run: `cmake --build build/vs2022-msvc --target Editor`
Expected: builds.

- [ ] **Step 4: Commit.**

```bash
git add src/app/scripting/interop/UiExports.cpp src/app/scene/serde/UiImageSerde.cpp
git commit -m "Route UIImage texture through texturePath in native setter and serde"
```

### Task 0.4: Reflect `UI Image` and make the UI components addable

**Files:**
- Modify: `src/app/scene/reflection/MoreComponents.reflect.cpp` (UI block, lines ~15–17 aliases and ~154–186)

**Interfaces:**
- Produces: a reflected `"UI Image"` component (fields `color:Color4`, `corner_radius:Float`, `texture:String`, `pixel_art:Bool`) that `list_component_types`/`add_component`/`set_component` recognize; UI components become `add_component`-able.

- [ ] **Step 1: Add the alias.** Under the existing UI aliases (~line 17):

```cpp
using UiImageComponent = aether::ui::UIImage;
```

- [ ] **Step 2: Remove `AE_NOT_ADDABLE()` from the four UI component blocks** (`UiCanvasComponent`, `UiRectComponent`, `UiTextComponent`, and the new image). Leave `AE_GENERIC_SERIALIZE()` on Canvas/Rect/Text unchanged.

- [ ] **Step 3: Add the `UI Image` reflection block** after the `UiTextComponent` block (before the file's end). Note: no `AE_GENERIC_SERIALIZE` — UIImage keeps its bespoke serde. The `texture` field is a CustomField so its setter can flag `textureDirty` without needing asset access:

```cpp
AE_COMPONENT(UiImageComponent, "UI Image", "UI", ICON_FA_IMAGE)
AE_FIELD_N("color", color, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
AE_FIELD_CUSTOM("texture", String,
    [](const void* c) -> ::aether::reflect::FieldValue
    {
        return ::aether::reflect::MakeValue(static_cast<const UiImageComponent*>(c)->texturePath);
    },
    [](void* c, const ::aether::reflect::FieldValue& v)
    {
        auto* img = static_cast<UiImageComponent*>(c);
        img->texturePath = v.str;
        img->textureDirty = true;
    })
AE_FIELD_N("pixel_art", pixelArt, Bool)
AE_COMPONENT_END()
```

- [ ] **Step 4: Build.**

Run: `cmake --build build/vs2022-msvc --target Editor`
Expected: builds.

- [ ] **Step 5: Restart the editor and verify the MCP now sees UI Image.**

Relaunch the editor (`AETHER_CONTROL_PORT=8787` from `build/vs2022-msvc`), `open_project D:/AetherCore/projects/INKBOUND`.
Run MCP `list_component_types`.
Expected: a `"UI Image"` entry with `color:color4, corner_radius:float, texture:string, pixel_art:bool` appears under category `UI`.

- [ ] **Step 6: Commit.**

```bash
git add src/app/scene/reflection/MoreComponents.reflect.cpp
git commit -m "Reflect UI Image and make UI components addable"
```

### Task 0.5: End-to-end MCP-authoring round-trip test

**Files:** none (live verification on a throwaway scene)

- [ ] **Step 1: Author a UI image via the MCP on a scratch scene.**

Via MCP: `new_scene` (kind `2d`) → `create_entity "TestCanvas"` → `add_component` `UI Canvas`, `UI Rect` → `create_entity "TestImage"`, `parent_entity` under TestCanvas → `add_component` `UI Rect`, `UI Image` → `set_component "UI Rect"` `{anchor_min:[0,0], anchor_max:[1,1], offset_min:[0,0], offset_max:[0,0]}` → `set_component "UI Image"` `{color:[0.3,0.85,1.0,1.0], corner_radius:12}`.

- [ ] **Step 2: Screenshot.** Run MCP `screenshot`. Expected: a cyan rounded rectangle fills the view.

- [ ] **Step 3: Save + reload round-trips.** `save_scene`, then `load_scene` the same scene, `screenshot` again. Expected: identical — the `ui_image` persisted and reloaded through reflection + serde.

- [ ] **Step 4: Cleanup.** Delete the scratch scene file if one was written to disk; no commit (throwaway).

---

## Phase 1 — Foundation: settings store, controller skeleton, shell, fonts

### Task 1.1: Bake the IBM Plex Mono italic font

**Files:**
- Create: `projects/INKBOUND/assets/fonts/IBMPlexMonoItalic.fontatlas`, `.fontmeta` (produced by the tool)
- Source: a temporary `IBMPlexMono-Italic.ttf` (fetch from Google Fonts, OFL)

- [ ] **Step 1: Fetch the TTF** into the scratchpad (OFL-licensed, embeddable):

```bash
curl -L -o "$TEMP/IBMPlexMono-Italic.ttf" \
  "https://github.com/google/fonts/raw/main/ofl/ibmplexmono/IBMPlexMono-Italic.ttf"
```

Expected: a non-empty `.ttf` (~90 KB). If the fetch fails, fall back to PixelStorm for the italic voice and note it (Global Constraints / spec Risk).

- [ ] **Step 2: Bake it** with AssetPacker to the project fonts dir (font name becomes `IBMPlexMonoItalic`):

```bash
./build/vs2022-msvc/tools/Release/AssetPacker.exe bake-font "$TEMP/IBMPlexMono-Italic.ttf" projects/INKBOUND/assets/fonts
```

Expected: `IBMPlexMonoItalic.fontatlas` + `.fontmeta` written under `projects/INKBOUND/assets/fonts/`.

- [ ] **Step 3: Verify in-engine.** Restart editor, author a throwaway `UI Text` with `font: "IBMPlexMonoItalic"`, `screenshot`. Expected: smooth italic mono glyphs (not the pixel font). Delete the throwaway entity.

- [ ] **Step 4: Commit.**

```bash
git add projects/INKBOUND/assets/fonts/IBMPlexMonoItalic.fontatlas projects/INKBOUND/assets/fonts/IBMPlexMonoItalic.fontmeta
git commit -m "Bake IBM Plex Mono italic font for INKBOUND UI"
```

### Task 1.2: `GameSettings` static store with JSON persistence

**Files:**
- Create: `projects/INKBOUND/scripts/GameSettings.cs`

**Interfaces:**
- Produces: `static float MusicVolume, SfxVolume, InkGlow`; `static bool ScreenShake`; `static readonly Vector4 Accent`; `static void Load()`; `static void Save()`; `static float ClampUnit(float)`.

- [ ] **Step 1: Write `GameSettings.cs`.** Uses `System.Text.Json` (available in CoreCLR) and `Environment.GetFolderPath` for LocalAppData:

```csharp
using System;
using System.IO;
using System.Numerics;
using System.Text.Json;

namespace AetherGame;

/// <summary>Player-facing game settings, persisted to LocalAppData. Read by the
/// Settings screen and by gameplay (Ink Glow, Screen Shake).</summary>
public static class GameSettings
{
    public static float MusicVolume = 0.70f;
    public static float SfxVolume = 0.55f;
    public static float InkGlow = 0.85f;      // 0..1, scales the ink accent glow
    public static bool ScreenShake = true;

    /// <summary>The cyan ink accent used across the UI and ink mechanic.</summary>
    public static readonly Vector4 Accent = new(0.302f, 0.851f, 1.0f, 1.0f);

    private static string PathOnDisk()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "INKBOUND");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "settings.json");
    }

    public static float ClampUnit(float v) => v < 0f ? 0f : (v > 1f ? 1f : v);

    private sealed class Dto
    {
        public float Music { get; set; }
        public float Sfx { get; set; }
        public float InkGlow { get; set; }
        public bool ScreenShake { get; set; }
    }

    public static void Load()
    {
        try
        {
            string p = PathOnDisk();
            if (!File.Exists(p)) return;
            Dto? d = JsonSerializer.Deserialize<Dto>(File.ReadAllText(p));
            if (d == null) return;
            MusicVolume = ClampUnit(d.Music);
            SfxVolume = ClampUnit(d.Sfx);
            InkGlow = ClampUnit(d.InkGlow);
            ScreenShake = d.ScreenShake;
        }
        catch (Exception e) { Log.Warn($"[INKBOUND] settings load failed: {e.Message}"); }
    }

    public static void Save()
    {
        try
        {
            var d = new Dto { Music = MusicVolume, Sfx = SfxVolume, InkGlow = InkGlow, ScreenShake = ScreenShake };
            File.WriteAllText(PathOnDisk(), JsonSerializer.Serialize(d));
        }
        catch (Exception e) { Log.Warn($"[INKBOUND] settings save failed: {e.Message}"); }
    }
}
```

- [ ] **Step 2: Verify it compiles** via a Play cycle (C# rebuilds on Play): MCP `play` then `stop`; check `get_console_log minimumLevel:error` is empty of compile errors.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/GameSettings.cs
git commit -m "Add GameSettings store with LocalAppData persistence"
```

### Task 1.3: Generate the vignette + scanline UI textures

**Files:**
- Create: `projects/INKBOUND/assets/textures/ui/vignette.png`, `scanlines.png` (via a scratch Pillow script)

- [ ] **Step 1: Generate the textures** with a Pillow script (matching the project's existing pixel-art asset workflow):

```python
# scratch: gen_ui_tex.py — run with: py -3 gen_ui_tex.py
from PIL import Image
import math, os
out = r"D:\AetherCore\projects\INKBOUND\assets\textures\ui"
os.makedirs(out, exist_ok=True)

# Vignette: transparent centre -> ~0.8 black at the corners.
S = 512
vig = Image.new("RGBA", (S, S), (0, 0, 0, 0))
px = vig.load()
cx = cy = (S - 1) / 2
maxd = math.hypot(cx, cy)
for y in range(S):
    for x in range(S):
        d = math.hypot(x - cx, y - cy) / maxd     # 0 centre .. 1 corner
        a = int(204 * max(0.0, (d - 0.35) / 0.65) ** 1.6)  # start ~35% out
        px[x, y] = (0, 0, 0, a)
vig.save(os.path.join(out, "vignette.png"))

# Scanlines: 2px tall tile, 1 dark row + 1 clear row, low alpha.
sc = Image.new("RGBA", (4, 2), (0, 0, 0, 0))
for x in range(4):
    sc.putpixel((x, 0), (0, 0, 0, 46))   # ~18% dark line
sc.save(os.path.join(out, "scanlines.png"))
print("wrote", out)
```

Run: `py -3 "$TEMP/gen_ui_tex.py"` (write the script to the scratchpad first).
Expected: both PNGs exist under `assets/textures/ui/`.

- [ ] **Step 2: Commit.**

```bash
git add projects/INKBOUND/assets/textures/ui/vignette.png projects/INKBOUND/assets/textures/ui/scanlines.png
git commit -m "Generate vignette and scanline UI textures"
```

### Task 1.4: `MenuController` state machine + rebuilt `Menu` scene skeleton

**Files:**
- Create: `projects/INKBOUND/scripts/MenuController.cs`
- Modify (via MCP authoring + save): `projects/INKBOUND/scenes/Menu.scene.toml`

**Interfaces:**
- Produces: `enum MenuScreen { Title, LevelSelect, Settings }`; `MenuController.Go(MenuScreen)`; `MenuController.Current`. Consumed by the three screen scripts.
- Consumes: `Scene.Find`, `Entity.SetActive`, `Input.IsKeyPressed`.

- [ ] **Step 1: Write `MenuController.cs`.** It owns the three roots and routes input to the active screen script via a shared interface:

```csharp
using AetherCore;

namespace AetherGame;

public enum MenuScreen { Title, LevelSelect, Settings }

/// <summary>Menu state machine: shows exactly one screen root, routes per-frame
/// input to that screen's controller, and handles global Escape (back to Title).</summary>
public sealed class MenuController : EntityScript
{
    public static MenuController? Instance;
    public MenuScreen Current { get; private set; } = MenuScreen.Title;

    private Entity _titleRoot, _levelRoot, _settingsRoot;

    public override void OnAttach()
    {
        Instance = this;
        Time.Resume();
        GameSettings.Load();
        _titleRoot = Scene.Find("TitleRoot");
        _levelRoot = Scene.Find("LevelSelectRoot");
        _settingsRoot = Scene.Find("SettingsRoot");
        Go(MenuScreen.Title);
    }

    public void Go(MenuScreen s)
    {
        Current = s;
        if (_titleRoot.IsValid) _titleRoot.SetActive(s == MenuScreen.Title);
        if (_levelRoot.IsValid) _levelRoot.SetActive(s == MenuScreen.LevelSelect);
        if (_settingsRoot.IsValid) _settingsRoot.SetActive(s == MenuScreen.Settings);
        ActiveScreen()?.OnShown();
    }

    public override void OnUpdate(float dt)
    {
        if (Current != MenuScreen.Title && Input.IsKeyPressed(Key.Escape))
        {
            Go(MenuScreen.Title);
            return;
        }
        ActiveScreen()?.HandleInput();
    }

    // Resolved lazily from the registry each call, so it never races script attach order.
    private IMenuScreen? ActiveScreen() => Current switch
    {
        MenuScreen.Title => ScreenRegistry<TitleScreen>.Get("TitleRoot"),
        MenuScreen.LevelSelect => ScreenRegistry<LevelSelectScreen>.Get("LevelSelectRoot"),
        MenuScreen.Settings => ScreenRegistry<SettingsScreen>.Get("SettingsRoot"),
        _ => null,
    };
}

/// <summary>Uniform screen interface the controller drives.</summary>
public interface IMenuScreen
{
    void OnShown();
    void HandleInput();
}
```

Note: the `ScreenRegistry<T>` indirection avoids a native "get script instance by type" API that doesn't exist. Implement it minimally in Step 2.

- [ ] **Step 2: Add the tiny screen registry** at the bottom of `MenuController.cs` so the controller can reach each screen's instance (each screen registers itself in `OnAttach`):

```csharp
using System.Collections.Generic;

namespace AetherGame;

internal static class ScreenRegistry<T> where T : EntityScript
{
    private static readonly Dictionary<string, T> s_byRoot = new();
    public static void Register(string rootName, T inst) => s_byRoot[rootName] = inst;
    public static T? Get(string rootName) => s_byRoot.TryGetValue(rootName, out var v) ? v : null;
}
```

- [ ] **Step 3: Author the `Menu` scene skeleton via MCP.** Replace the current menu contents:
  - Keep `Main Camera` (2D, orthographic) as-is.
  - Create `MenuUI` with `UI Canvas` (scale_mode `scale_with_reference`, reference `[1920,1080]`) + `UI Rect` full-stretch; attach script `MenuController`.
  - Create empty roots `TitleRoot`, `LevelSelectRoot`, `SettingsRoot`, each parented under `MenuUI` with a full-stretch `UI Rect`.
  - Remove the old `MenuTitle/MenuSubtitle/PlayButton/PlayLabel/MenuHint` entities and the `MainMenu` script attachment.
  - `save_scene`.

- [ ] **Step 4: Verify.** `play`, then `get_console_log minimumLevel:error`. Expected: no errors; the three roots exist (only `TitleRoot` will have content later). `stop`.

- [ ] **Step 5: Commit.**

```bash
git add projects/INKBOUND/scripts/MenuController.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add MenuController state machine and rebuild the Menu scene skeleton"
```

### Task 1.5: `MenuShell` — authored atmosphere + eye-blink animation

**Files:**
- Create: `projects/INKBOUND/scripts/MenuShell.cs`
- Modify (MCP + save): `Menu.scene.toml` (shell entities under `MenuUI`)

**Interfaces:**
- Consumes: `GameSettings.Accent`, `Ui.*`, authored eye entities named `Eye0..EyeN`.

- [ ] **Step 1: Author the shell entities via MCP** under `MenuUI`, ordered so vignette/scanlines draw last (topmost):
  - `ShellBg`: `UI Rect` full-stretch + `UI Image` color `[0.051,0.059,0.078,1]` (`#0d0f14`).
  - `ShellBgTop`: `UI Rect` anchored to the top third + `UI Image` color `[0.086,0.094,0.122,0.5]` (fakes the top gradient band).
  - `Eye0`..`Eye3`: small `UI Rect` (≈14×14 px) circles (`UI Image` corner_radius 7) color `[0.416,0.165,0.165,0.0]` (`#6a2a2a`, start hidden) placed in dark corners.
  - `ShellVignette`: `UI Rect` full-stretch + `UI Image` `texture: "assets/textures/ui/vignette.png"`, color `[1,1,1,1]`.
  - `ShellScanlines`: `UI Rect` full-stretch + `UI Image` `texture: "assets/textures/ui/scanlines.png"`, `pixel_art: true`, color `[1,1,1,1]`.
  - Attach `MenuShell` to `MenuUI` (alongside `MenuController`) or to `ShellBg`. `save_scene`.

- [ ] **Step 2: Write `MenuShell.cs`** — blinks the eyes on staggered cycles:

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Animates the shared menu atmosphere: staggered red "watching eye" blinks.
/// The gradient/vignette/scanlines are static authored images.</summary>
public sealed class MenuShell : EntityScript
{
    private readonly Entity[] _eyes = new Entity[4];
    private readonly float[] _phase = { 0f, 3.3f, 6.7f, 9.1f };
    private readonly float[] _period = { 9f, 11f, 13f, 10f };
    private static readonly Vector4 EyeColor = new(0.416f, 0.165f, 0.165f, 1f);
    private float _t;

    public override void OnAttach()
    {
        for (int i = 0; i < _eyes.Length; i++) _eyes[i] = Scene.Find($"Eye{i}");
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        for (int i = 0; i < _eyes.Length; i++)
        {
            if (!_eyes[i].IsValid) continue;
            // Mostly closed; a brief opening near the end of each period.
            float u = ((_t + _phase[i]) % _period[i]) / _period[i];
            float open = u > 0.86f ? System.MathF.Sin((u - 0.86f) / 0.14f * System.MathF.PI) : 0f;
            Vector4 c = EyeColor; c.W = 0.85f * System.Math.Clamp(open, 0f, 1f);
            Ui.SetImageColor(_eyes[i], c);
        }
    }
}
```

- [ ] **Step 3: Verify.** `play`, `screenshot`. Expected: dark screen with vignette + faint scanlines; eyes flicker open occasionally (take 2–3 screenshots a second apart to catch a blink). `stop`.

- [ ] **Step 4: Commit.**

```bash
git add projects/INKBOUND/scripts/MenuShell.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add MenuShell atmosphere with blinking watching-eyes"
```

---

## Phase 2 — Title screen

### Task 2.1: Author the Title layout

**Files:** Modify (MCP + save): `Menu.scene.toml` (entities under `TitleRoot`)

- [ ] **Step 1: Author the static Title entities via MCP** under `TitleRoot`, left-aligned composition (anchors toward the left; use the storyboard tokens from the spec §4.1):
  - `TitleWordmark` (`UI Text` "INKBOUND", `PixelStorm`, large, color `#eef1f7` `[0.933,0.945,0.969,1]`, left-aligned).
  - `TitleBlob` (`UI Image` circle, corner_radius large, cyan accent) to the wordmark's left.
  - `TitleDrip0..2` (thin `UI Image` rects, cyan, ~0.5 alpha) hanging under the wordmark.
  - `TitleTagline` (`UI Text` "the ink knows the way out. you don't.", `IBMPlexMonoItalic`, `#7a8296`).
  - `MenuItem0..3` each a row with: `Marker{n}` (selected → filled cyan blot w/ corner_radius; unselected → hollow ring approximated by a small dark image with a lighter border image), `Label{n}` (`UI Text`, PixelStorm), `Gloss{n}` (`UI Text`, `IBMPlexMonoItalic`). Labels/glosses per spec: descend/return/attune/release.
  - `Wanderer` (`UI Image` textured with `assets/textures/player/idle32x32.png`) bottom-right + `InkPool` (cyan circle, low alpha) behind it.
  - `FlavorBR`, `FlavorBL` (`UI Text`, dim).
  - `save_scene`.

- [ ] **Step 2: Verify layout by screenshot.** `play` (TitleRoot is the active screen), `screenshot`, compare against the storyboard composition; nudge offsets via `set_component "UI Rect"` until it reads right. `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Author INKBOUND title screen layout"
```

### Task 2.2: `TitleScreen` behavior — selection, activation, animation

**Files:** Create: `projects/INKBOUND/scripts/TitleScreen.cs`; attach it to `TitleRoot` (MCP `add_script`).

**Interfaces:**
- Implements `IMenuScreen`; registers into `ScreenRegistry<TitleScreen>` in `OnAttach`.
- Consumes: `MenuController.Instance`, `GameSettings.Accent`, `Scene.Load`, `Ui.*`, `Input.IsKeyPressed`.

- [ ] **Step 1: Write `TitleScreen.cs`:**

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class TitleScreen : EntityScript, IMenuScreen
{
    private int _sel;
    private float _t;
    private Entity _wordmark;
    private readonly Entity[] _markers = new Entity[4];
    private readonly Entity[] _labels = new Entity[4];

    private static readonly Vector4 SelColor = GameSettings.Accent;
    private static readonly Vector4 Unsel = new(0.337f, 0.361f, 0.431f, 1f); // #565c6e

    public override void OnAttach()
    {
        ScreenRegistry<TitleScreen>.Register("TitleRoot", this);
        _wordmark = Scene.Find("TitleWordmark");
        for (int i = 0; i < 4; i++) { _markers[i] = Scene.Find($"Marker{i}"); _labels[i] = Scene.Find($"Label{i}"); }
        Apply();
    }

    public void OnShown() { _sel = 0; Apply(); }

    public void HandleInput()
    {
        if (Input.IsKeyPressed(Key.Down)) { _sel = (_sel + 1) & 3; Apply(); }
        if (Input.IsKeyPressed(Key.Up))   { _sel = (_sel + 3) & 3; Apply(); }
        if (Input.IsKeyPressed(Key.Enter) || Input.IsKeyPressed(Key.Space)) Activate();
    }

    private void Activate()
    {
        switch (_sel)
        {
            case 0: Scene.Load("Level1"); break;                       // descend
            case 1: MenuController.Instance?.Go(MenuScreen.LevelSelect); break; // return
            case 2: MenuController.Instance?.Go(MenuScreen.Settings); break;    // attune
            case 3: Log.Info("[INKBOUND] release (quit)"); break;      // release — real quit export is a follow-up
        }
    }

    private void Apply()
    {
        for (int i = 0; i < 4; i++)
        {
            bool on = i == _sel;
            if (_labels[i].IsValid) { Ui.SetTextColor(_labels[i], on ? SelColor : Unsel); Ui.SetFontSize(_labels[i], on ? 34f : 30f); }
            if (_markers[i].IsValid) { Vector4 c = SelColor; c.W = on ? 1f : 0f; Ui.SetImageColor(_markers[i], c); }
        }
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        if (_wordmark.IsValid) // 7s flicker
        {
            float f = 0.86f + 0.14f * System.MathF.Abs(System.MathF.Sin(_t * 0.9f)) * (Rand(_t) > 0.08f ? 1f : 0.4f);
            Vector4 c = new(0.933f, 0.945f, 0.969f, f);
            Ui.SetTextColor(_wordmark, c);
        }
    }

    private static float Rand(float t) { float s = System.MathF.Sin(t * 91.7f) * 43758.5f; return s - System.MathF.Floor(s); }
}
```

Note on quit: `release` logs and no-ops here because no script-facing quit export exists yet. Adding an `aether_request_quit` interop export (runtime-safe, per the scripting-parity roadmap) is a flagged follow-up, out of this plan's scope.

- [ ] **Step 2: Attach + verify.** MCP `add_script TitleRoot MenuController`? No — `add_script` `TitleScreen` to `TitleRoot`. `save_scene`, `play`. Drive `send_input {down:["down"]}`/`{up:["up"]}` and screenshot: the ink-blot marker + cyan highlight track the selection. Press Enter on `attune`/`return` and confirm the controller switches screens (they'll be empty until Phases 3–4). `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/TitleScreen.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add title screen selection, activation, and flicker"
```

---

## Phase 3 — Level Select

### Task 3.1: Author the Level Select layout

**Files:** Modify (MCP + save): `Menu.scene.toml` (under `LevelSelectRoot`)

- [ ] **Step 1: Author via MCP** per spec §4.2: `PathLine` dashed connector (a row of short `UI Image` dash rects), `Node0..5` circles (corner_radius, states colored later), `Padlock4/5` small images on locked nodes, and a right `DetailPanel` group: `DetailTitle`, `DetailName`, `DetailPreview` (image, `assets/textures/ui/stripe.png` — generate a 135° stripe PNG in this step via Pillow like Task 1.3), `DetailStats`, `DetailFlavor`. `save_scene`.

- [ ] **Step 2: Verify** by screenshot with `LevelSelectRoot` active (temporarily `Go(LevelSelect)` by pressing `return` on the title, or set start screen). Adjust offsets. `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scenes/Menu.scene.toml projects/INKBOUND/assets/textures/ui/stripe.png
git commit -m "Author level select layout"
```

### Task 3.2: `LevelSelectScreen` behavior — nav, lock-skip, launch, detail panel

**Files:** Create: `projects/INKBOUND/scripts/LevelSelectScreen.cs`; attach to `LevelSelectRoot`.

**Interfaces:** Implements `IMenuScreen`; registers into `ScreenRegistry<LevelSelectScreen>`.

- [ ] **Step 1: Write `LevelSelectScreen.cs`** with the data-driven node list and nav:

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class LevelSelectScreen : EntityScript, IMenuScreen
{
    private readonly struct Node
    {
        public readonly string Name, Flavor, Scene, Caps, Par;
        public readonly bool Locked;
        public Node(string n, string f, string s, string caps, string par, bool locked)
        { Name = n; Flavor = f; Scene = s; Caps = caps; Par = par; Locked = locked; }
    }

    private static readonly Node[] Nodes =
    {
        new("1-1 · The Cheerful Plunge", "you were smiling when you fell in.", "Level1", "●●●○○", "01:10", false),
        new("1-2 · Quiet, Please",       "the dark prefers you keep your voice down.", "Level2", "●●●●○", "01:25", false),
        new("1-3 · The Hollow Descent",  "the walls here still remember every route you've drawn.", "Level3", "●●●●○", "01:40", false),
        new("1-4 · The Fourth Descent",  "you don't remember a fourth. and yet.", "Level4", "●●●●●", "02:05", false),
        new("it's not ready for you yet","", "", "○○○○○", "--:--", true),
        new("best not to think about this one","", "", "○○○○○", "--:--", true),
    };

    private int _sel;
    private readonly Entity[] _nodes = new Entity[6];
    private Entity _name, _flavor, _stats;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 Done = new(0.106f, 0.125f, 0.188f, 1f);
    private static readonly Vector4 Locked = new(0.094f, 0.102f, 0.133f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<LevelSelectScreen>.Register("LevelSelectRoot", this);
        for (int i = 0; i < 6; i++) _nodes[i] = Scene.Find($"Node{i}");
        _name = Scene.Find("DetailName"); _flavor = Scene.Find("DetailFlavor"); _stats = Scene.Find("DetailStats");
    }

    public void OnShown() { _sel = FirstUnlocked(); Apply(); }

    public void HandleInput()
    {
        if (Input.IsKeyPressed(Key.Right)) { Step(+1); }
        if (Input.IsKeyPressed(Key.Left))  { Step(-1); }
        if (Input.IsKeyPressed(Key.Enter) && !Nodes[_sel].Locked) Scene.Load(Nodes[_sel].Scene);
    }

    private static int FirstUnlocked() { for (int i = 0; i < Nodes.Length; i++) if (!Nodes[i].Locked) return i; return 0; }

    private void Step(int dir)
    {
        int i = _sel;
        for (int guard = 0; guard < Nodes.Length; guard++)
        {
            i += dir;
            if (i < 0 || i >= Nodes.Length) return;      // clamp at ends
            if (!Nodes[i].Locked) { _sel = i; Apply(); return; } // skip locked
        }
    }

    private void Apply()
    {
        for (int i = 0; i < 6; i++)
        {
            if (!_nodes[i].IsValid) continue;
            Vector4 c = Nodes[i].Locked ? Locked : Done;
            Ui.SetImageColor(_nodes[i], i == _sel ? Cyan : c);
        }
        if (_name.IsValid) Ui.SetText(_name, Nodes[_sel].Name);
        if (_flavor.IsValid) Ui.SetText(_flavor, Nodes[_sel].Flavor);
        if (_stats.IsValid) Ui.SetText(_stats, $"INK CAPACITY {Nodes[_sel].Caps}   PAR TIME {Nodes[_sel].Par}");
    }
}
```

- [ ] **Step 2: Attach + verify.** `add_script LevelSelectScreen` to `LevelSelectRoot`, `save_scene`, `play`, enter Level Select from the title (`return`). Drive `send_input {down:["right"]}`/`{down:["left"]}`; confirm selection skips locked nodes 5–6 and the detail panel updates; Enter on node 1 loads `Level1`. `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/LevelSelectScreen.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add level select navigation, lock-skipping, and detail panel"
```

---

## Phase 4 — Settings

### Task 4.1: Author the Settings layout

**Files:** Modify (MCP + save): `Menu.scene.toml` (under `SettingsRoot`)

- [ ] **Step 1: Author via MCP** per spec §4.3: `SettingsHeader` ("attune", cyan, PixelStorm) + `HeaderUnderline` (thin cyan image), `BackLink`, `Subtitle`; three slider groups `Slider{Music,Sfx,InkGlow}` each = `…Label` (`IBMPlexMonoItalic`), `…Track` (dark inset image), `…Fill` (cyan image, width set by script), `…Orb` (cyan circle at fill edge); `ShakeLabel` + `ShakeSocket` + `ShakeOrb`; `RedactRow` (blurred label approximated by a dim `UI Text` + `RedactBars` striped image over it); `Footer`. `save_scene`.

- [ ] **Step 2: Verify** by screenshot with `SettingsRoot` active (enter via `attune`). Adjust. `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Author settings (attune) layout"
```

### Task 4.2: `SettingsScreen` behavior — adjust, toggle, persist

**Files:** Create: `projects/INKBOUND/scripts/SettingsScreen.cs`; attach to `SettingsRoot`.

**Interfaces:** Implements `IMenuScreen`; registers into `ScreenRegistry<SettingsScreen>`; reads/writes `GameSettings`.

- [ ] **Step 1: Write `SettingsScreen.cs`:**

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class SettingsScreen : EntityScript, IMenuScreen
{
    private int _sel;               // 0 music, 1 sfx, 2 inkglow, 3 shake
    private Entity _musicFill, _sfxFill, _inkFill, _musicOrb, _sfxOrb, _inkOrb, _shakeOrb;
    private const float TrackW = 210f;

    public override void OnAttach()
    {
        ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
        _musicFill = Scene.Find("SliderMusicFill"); _musicOrb = Scene.Find("SliderMusicOrb");
        _sfxFill = Scene.Find("SliderSfxFill");     _sfxOrb = Scene.Find("SliderSfxOrb");
        _inkFill = Scene.Find("SliderInkGlowFill"); _inkOrb = Scene.Find("SliderInkGlowOrb");
        _shakeOrb = Scene.Find("ShakeOrb");
    }

    public void OnShown() { _sel = 0; Apply(); }

    public void HandleInput()
    {
        if (Input.IsKeyPressed(Key.Down)) { _sel = (_sel + 1) & 3; }
        if (Input.IsKeyPressed(Key.Up))   { _sel = (_sel + 3) & 3; }
        float step = 0f;
        if (Input.IsKeyPressed(Key.Right)) step = +0.05f;
        if (Input.IsKeyPressed(Key.Left))  step = -0.05f;
        if (step != 0f)
        {
            switch (_sel)
            {
                case 0: GameSettings.MusicVolume = GameSettings.ClampUnit(GameSettings.MusicVolume + step); break;
                case 1: GameSettings.SfxVolume = GameSettings.ClampUnit(GameSettings.SfxVolume + step); break;
                case 2: GameSettings.InkGlow = GameSettings.ClampUnit(GameSettings.InkGlow + step); break;
            }
            GameSettings.Save(); Apply();
        }
        if (_sel == 3 && (Input.IsKeyPressed(Key.Enter) || Input.IsKeyPressed(Key.Space)))
        {
            GameSettings.ScreenShake = !GameSettings.ScreenShake; GameSettings.Save(); Apply();
        }
    }

    private void SetFill(Entity fill, Entity orb, float v)
    {
        if (fill.IsValid) { Vector4 r = Ui.GetRect(fill); Ui.SetRect(fill, r.X, r.Y, TrackW * v, r.W); }
        if (orb.IsValid) { Vector4 r = Ui.GetRect(orb); Ui.SetRect(orb, TrackW * v, r.Y, r.Z, r.W); }
    }

    private void Apply()
    {
        SetFill(_musicFill, _musicOrb, GameSettings.MusicVolume);
        SetFill(_sfxFill, _sfxOrb, GameSettings.SfxVolume);
        SetFill(_inkFill, _inkOrb, GameSettings.InkGlow);
        if (_shakeOrb.IsValid) { Vector4 c = GameSettings.Accent; c.W = GameSettings.ScreenShake ? 1f : 0.12f; Ui.SetImageColor(_shakeOrb, c); }
    }
}
```

- [ ] **Step 2: Attach + verify.** `add_script SettingsScreen` to `SettingsRoot`, `save_scene`, `play`, enter via `attune`. Drive `send_input` up/down to move between controls, left/right to adjust; confirm fills + orbs move and the toggle flips; check `settings.json` is written under LocalAppData. Restart the editor, re-enter Settings, confirm values persisted. `stop`.

- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/SettingsScreen.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add settings adjust, toggle, and persistence"
```

### Task 4.3: Wire Ink Glow + Screen Shake into gameplay

**Files:**
- Modify: `projects/INKBOUND/scripts/InkBlock.cs` (tint alpha scaled by Ink Glow)
- Modify: `projects/INKBOUND/scripts/CameraFollow.cs` (respect Screen Shake flag — read only)

**Interfaces:** Consumes `GameSettings.InkGlow`, `GameSettings.ScreenShake`.

- [ ] **Step 1: Scale the ink glow.** In `InkBlock.OnAttach`, after `_baseTint` is set for anchored ink, scale its alpha by Ink Glow so the setting is visible:

```csharp
// (anchored branch) let the Ink Glow setting scale the conjured-ink glow.
_baseTint.W *= 0.4f + 0.6f * GameSettings.InkGlow;
SpriteRenderer.SetTint(Self, _baseTint);
```

- [ ] **Step 2: Gate shake.** In `CameraFollow`, wherever a shake offset would be applied, multiply it by `(GameSettings.ScreenShake ? 1f : 0f)`. If `CameraFollow` has no shake yet, add a no-op guarded hook: `float shake = GameSettings.ScreenShake ? _shake : 0f;` used at the offset site (leave `_shake` at 0 if unused — this establishes the flag consumer without inventing a shake system).

- [ ] **Step 3: Verify.** `play` Level1, draw ink, confirm it renders; open Settings, drop Ink Glow to 0, return to Level1, confirm the ink glow is dimmer. `stop`.

- [ ] **Step 4: Commit.**

```bash
git add projects/INKBOUND/scripts/InkBlock.cs projects/INKBOUND/scripts/CameraFollow.cs
git commit -m "Wire Ink Glow and Screen Shake settings into gameplay"
```

---

## Phase 5 — Full-fidelity animation polish

### Task 5.1: Title blob bob + drip fall

**Files:** Modify: `projects/INKBOUND/scripts/TitleScreen.cs`

- [ ] **Step 1: Animate blob + drips** in `TitleScreen.OnUpdate` (add fields `_blob`, `_drips[]` cached in `OnAttach`): bob the blob with `Ui.SetRect` y-offset `3*sin(t/3.5*2π)`; animate each drip's alpha/length on a staggered loop. (Use `Ui.GetRect`/`Ui.SetRect` to move within the authored anchors.)
- [ ] **Step 2: Verify** with 2–3 spaced screenshots during `play`; confirm motion. `stop`.
- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/TitleScreen.cs
git commit -m "Animate title ink blob bob and drips"
```

### Task 5.2: Settings pooling-orb glow pulse + slider polish

**Files:** Modify: `projects/INKBOUND/scripts/SettingsScreen.cs`

- [ ] **Step 1: Pulse the selected slider's orb** in `OnUpdate` (gentle alpha sine) so the active control reads as "pooling". 
- [ ] **Step 2: Verify** by screenshot during `play`. `stop`.
- [ ] **Step 3: Commit.**

```bash
git add projects/INKBOUND/scripts/SettingsScreen.cs
git commit -m "Add pooling-orb pulse to settings sliders"
```

### Task 5.3: Full menu walkthrough + push

- [ ] **Step 1: End-to-end verification.** `play` from the `Menu` scene. Walk Title → (attune) Settings → back → (return) Level Select → back → (descend) Level1. Screenshot each screen; confirm the shell atmosphere is continuous (no flash) and the console is error-free (`get_console_log minimumLevel:warn`).
- [ ] **Step 2: Push.**

```bash
git push
```

---

## Self-Review Notes

- **Spec coverage:** Title (§4.1)→Phase 2; Level Select (§4.2)→Phase 3; Settings (§4.3)→Phase 4; shell/atmosphere (§3)→Task 1.5; fonts (§6)→Task 1.1; textures (§6)→Task 1.3 & 3.1; state/persistence (§5)→Task 1.2; wiring (§4.3)→Task 4.3; animations (full-fidelity)→Task 1.5, 2.2, 5.x; MCP-authoring enablement (the discovered prerequisite)→Phase 0. Testing method (§7 MCP visual+input)→every task's verify step.
- **Known follow-ups (out of scope, flagged):** the `release`/quit script export (Task 2.2 note); a real camera-shake system (Task 4.3 leaves only the flag consumer); an audio system for Music/SFX. These are deliberately deferred per the spec's out-of-scope list.
