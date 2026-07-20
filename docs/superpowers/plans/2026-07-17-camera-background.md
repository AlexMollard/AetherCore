# Camera Background (Unified 2D/3D) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the camera's `useSkyGradient` bool with a first-class Background mode (Solid Colour / multi-stop angled Gradient / procedural Sky Gradient) whose Solid and Gradient modes are WYSIWYG-exact, and stop the phantom sun gizmo appearing in 2D scenes.

**Architecture:** Solid/Gradient backgrounds are composited in **display space at the tonemap pass**, using the HDR target's alpha channel as a scene-coverage mask (alpha 0 = background, alpha 1 = scene). The procedural sky path is unchanged (skybox writes alpha 1). Background parameters reach the tonemap pass via a BDA-addressed buffer, not `FrameConstants`.

**Tech Stack:** C++20, Vulkan (via engine `gpu`/`vulkan` layers), Slang shaders, EnTT ECS, toml++ serialization, doctest tests, aethercore MCP for runtime verification.

## Global Constraints

- Interop stays runtime-safe: no `ComponentCatalog`/editor deps in `*Exports.cpp` (not touched here, but do not add such deps).
- `FrameConstants` is a locked 768-byte layout with per-field offset `static_assert`s — **do not add fields to it**. Background params go through the tonemap pass's own buffer.
- Bindless textures bind `g_textures[]` at **set 0** in fullscreen passes (tonemap already does this).
- Shader edits require rebuilding the `App_CompileShaders` target **and** the `EngineAssetsPak`; run the app from the build-tree root (shaders are CWD-relative) or it segfaults at pipeline creation.
- Build dir is whichever is configured (`build-ninja-clang/`, `build/`, or `build-vs2022-msvc/`); commands below use `build-ninja-clang` — substitute your configured dir.
- Prod compiler is MSVC; clang-tidy/clangd uses clang-cl and its errors are real — keep both clean.
- Reflection `FieldType` has **no list type**; the gradient-stop list must be handled with custom TOML read/write and a custom inspector editor, not `AE_FIELD_*`.
- Follow existing patterns: enum reflection mirrors `CameraProjectionEnum()`; per-frame post-process config mirrors `SetExposure`.

---

## File Structure

- `src/engine/scene/CameraComponents.hpp` — new `CameraBackground` enum, `GradientStop`, camera fields; remove `useSkyGradient`.
- `src/app/scene/reflection/CoreComponents.reflect.cpp` — `CameraBackgroundEnum()`, reflect `background` + `gradient_angle`, keep `clear_color`, drop `use_sky_gradient`.
- `src/app/scene/SceneSerializerToml.cpp` — custom `gradient_stops` read/write + `use_sky_gradient` back-compat migration.
- `tests/scene/SceneSerializerTests.cpp` — round-trip + migration + clamping tests.
- `src/app/debug/ComponentDrawers.cpp` — mode dropdown + solid picker + gradient stop list editor + angle.
- `src/engine/rendering/RenderFramePacket.hpp` — carry resolved background state on the packet.
- `src/engine/AetherCore.cpp` — extract background state into the packet; remove the `skyVoidColor.w` flat-clear hack.
- `src/shaders/skybox.slang` — remove flat-clear branch; output transparent (alpha 0) in WYSIWYG mode; sky writes alpha 1.
- `src/engine/rendering/RenderingSubsystem.cpp` — skybox push `drawSky` bit; HDR clear alpha 0; feed background params to the post-process stack.
- `src/shaders/gltf_mesh.slang` — opaque forward writes coverage alpha 1.
- `src/engine/passes/PostProcessStack.{hpp,cpp}` — background params buffer + setter; extend tonemap push.
- `src/shaders/tonemap.slang` — display-space background eval + coverage composite.
- `src/app/debug/LightingPanel.cpp` — gate sun gizmo to 3D scenes with a real directional source.
- `src/app/debug/HierarchyPanel.cpp`, `resources/scenes/default2d.scene.toml` — 2D cameras default to Solid Colour.

---

## Phase 1 — Data model, reflection, serialization, editor UI

### Task 1: Camera background data model

**Files:**
- Modify: `src/engine/scene/CameraComponents.hpp:18-34`
- Modify: `src/engine/AetherCore.cpp:670-679` (keep behavior via mode mapping — render rework lands in Phase 2)
- Modify: `src/app/debug/ComponentDrawers.cpp:347-356` (compile fix; full UI in Task 5)
- Modify: `tests/scene/SceneSerializerTests.cpp:511,1177-1191` (compile fix; assertions updated in Task 4)

**Interfaces:**
- Produces: `enum class CameraBackground : std::uint8_t { SolidColour=0, Gradient=1, SkyGradient=2 };`
  `struct GradientStop { glm::vec3 colour; float position; };`
  `CameraComponent::background`, `::clearColor` (kept), `::gradientStops`, `::gradientAngleDegrees`.

- [ ] **Step 1: Write the failing test**

Add to `tests/scene/SceneSerializerTests.cpp` (near other camera tests):

```cpp
TEST_CASE("CameraComponent defaults to SkyGradient with two gradient stops")
{
    aether::CameraComponent cam{};
    CHECK(cam.background == aether::CameraBackground::SkyGradient);
    CHECK(cam.gradientStops.size() == 2);
    CHECK(cam.gradientStops.front().position == doctest::Approx(0.0f));
    CHECK(cam.gradientStops.back().position == doctest::Approx(1.0f));
    CHECK(cam.gradientAngleDegrees == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-ninja-clang --target EngineTests` — expect a **compile** failure (`background` / `gradientStops` unknown). That failing build is the red state.

- [ ] **Step 3: Implement the data model**

In `src/engine/scene/CameraComponents.hpp`, replace the background portion of `CameraComponent` (lines 26-30) and add the enum + struct above `struct CameraComponent`:

```cpp
enum class CameraBackground : std::uint8_t
{
    SolidColour = 0,
    Gradient    = 1,
    SkyGradient = 2, // procedural sky (renderer environment / DayNight entity)
};

struct GradientStop
{
    glm::vec3 colour{0.0f};
    float     position = 0.0f; // 0..1 along the gradient axis
};
```

Inside `struct CameraComponent`, replace `bool useSkyGradient = true;` and the `clearColor` line with:

```cpp
// Scene background policy, owned by the main camera.
CameraBackground background = CameraBackground::SkyGradient;
glm::vec3 clearColor{0.10f, 0.10f, 0.12f};                 // SolidColour mode
std::vector<GradientStop> gradientStops{                    // Gradient mode (>=2)
        GradientStop{{0.05f, 0.06f, 0.10f}, 0.0f},
        GradientStop{{0.02f, 0.02f, 0.03f}, 1.0f},
};
float gradientAngleDegrees = 0.0f;                          // 0 = top->bottom
```

Add `#include <vector>` to the header if not already present (it includes `<vector>` at line 6 — verify).

- [ ] **Step 4: Fix compile sites (behavior-preserving)**

In `src/engine/AetherCore.cpp:672`, change the condition from `!cameraComponent->useSkyGradient` to `cameraComponent->background != CameraBackground::SkyGradient` (Solid or Gradient still uses the temporary flat-clear path for now; Gradient collapses to `clearColor` until Phase 2):

```cpp
if (const auto* cameraComponent = world.TryGet<CameraComponent>(mainCamera);
    cameraComponent != nullptr && cameraComponent->background != CameraBackground::SkyGradient)
```

In `src/app/debug/ComponentDrawers.cpp:347-356`, replace with a minimal compiling stand-in (full UI in Task 5):

```cpp
int background = static_cast<int>(cam->background);
constexpr const char* kBackgrounds[] = {"Solid Color", "Gradient", "Sky Gradient"};
if (PropCombo("Clear", &background, kBackgrounds, IM_ARRAYSIZE(kBackgrounds)))
{
    cam->background = static_cast<CameraBackground>(background);
}
if (cam->background == CameraBackground::SolidColour)
{
    PropColor3("Color", &cam->clearColor.x);
}
```

In `tests/scene/SceneSerializerTests.cpp`, at the two sites (`:511`, `:1177-1178`, `:1190-1191`) replace `useSkyGradient` usage: setter `camera.useSkyGradient = false;` becomes `camera.background = aether::CameraBackground::SolidColour;`, and `CHECK_FALSE(...->useSkyGradient)` / `CHECK_FALSE(parsed->...->useSkyGradient)` become `CHECK(...->background == aether::CameraBackground::SolidColour)`.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build-ninja-clang --target EngineTests && ctest --test-dir build-ninja-clang -R SceneSerializer --output-on-failure`
Expected: build succeeds; new default test PASSES.

- [ ] **Step 6: Commit**

```bash
git add src/engine/scene/CameraComponents.hpp src/engine/AetherCore.cpp src/app/debug/ComponentDrawers.cpp tests/scene/SceneSerializerTests.cpp
git commit -m "feat(camera): add CameraBackground mode + gradient stops data model"
```

---

### Task 2: Reflect the background enum + angle

**Files:**
- Modify: `src/app/scene/reflection/CoreComponents.reflect.cpp:19-25` (add enum), `:164-172` (fields)

**Interfaces:**
- Consumes: `CameraBackground`, `CameraComponent::background`, `::gradientAngleDegrees`, `::clearColor`.
- Produces: `CameraBackgroundEnum()` returning an `EnumTable` with keys `"solid_colour"`, `"gradient"`, `"sky_gradient"`.

- [ ] **Step 1: Add the enum table**

In the anonymous namespace (after `CameraProjectionEnum()` at line 21-25) add:

```cpp
const reflect::EnumTable& CameraBackgroundEnum()
{
    static const reflect::EnumTable table{{
            {"solid_colour", static_cast<int>(CameraBackground::SolidColour)},
            {"gradient", static_cast<int>(CameraBackground::Gradient)},
            {"sky_gradient", static_cast<int>(CameraBackground::SkyGradient)},
    }};
    return table;
}
```

- [ ] **Step 2: Update the reflected fields**

Replace lines 170-171 (`use_sky_gradient` + `clear_color`) with:

```cpp
AE_FIELD_ENUM("background", background, CameraBackgroundEnum())
AE_FIELD_N("clear_color", clearColor, Color3)
AE_FIELD_N("gradient_angle", gradientAngleDegrees, Float)
```

(The `gradientStops` list is NOT reflected — reflection has no list type. It is serialized in Task 3.)

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-ninja-clang --target EngineTests`
Expected: PASS (compiles).

- [ ] **Step 4: Commit**

```bash
git add src/app/scene/reflection/CoreComponents.reflect.cpp
git commit -m "feat(camera): reflect background mode enum + gradient angle"
```

---

### Task 3: Serialize gradient stops + migrate legacy scenes

**Files:**
- Modify: `src/app/scene/SceneSerializerToml.cpp` — camera write block (near `WriteReflectedToToml("Camera", ...)`) and camera read block (`:1378-1384`).

**Interfaces:**
- Consumes: `CameraComponent::gradientStops`, `::background`, `::clearColor` (from Task 1); `CameraBackground` enum.
- Produces: TOML `[entity.camera]` with a `gradient_stops` array of `{ colour = [r,g,b], position = p }` tables; legacy `use_sky_gradient` read maps to `background`.

- [ ] **Step 1: Write the failing round-trip test**

Add to `tests/scene/SceneSerializerTests.cpp`:

```cpp
TEST_CASE("CameraComponent gradient stops round-trip through TOML")
{
    using namespace aether;
    SceneData scene;
    EntityRecord rec;
    CameraComponent cam{};
    cam.background = CameraBackground::Gradient;
    cam.gradientAngleDegrees = 30.0f;
    cam.gradientStops = {
            {{1.0f, 0.0f, 0.0f}, 0.0f},
            {{0.0f, 1.0f, 0.0f}, 0.5f},
            {{0.0f, 0.0f, 1.0f}, 1.0f},
    };
    rec.camera = cam;
    rec.mainCamera = true;
    scene.entities.push_back(rec);

    const std::string toml = SerializeSceneToToml(scene);
    const auto parsed = DeserializeSceneFromToml(toml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities.size() == 1);
    REQUIRE(parsed->entities[0].camera.has_value());
    const auto& out = *parsed->entities[0].camera;
    CHECK(out.background == CameraBackground::Gradient);
    CHECK(out.gradientAngleDegrees == doctest::Approx(30.0f));
    REQUIRE(out.gradientStops.size() == 3);
    CHECK(out.gradientStops[1].position == doctest::Approx(0.5f));
    CHECK(out.gradientStops[2].colour.b == doctest::Approx(1.0f));
}
```

(Use the actual serialize/deserialize entry-point names present in `SceneSerializerToml.cpp` — match the signatures the other tests in this file already call.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-ninja-clang --target EngineTests && ctest --test-dir build-ninja-clang -R SceneSerializer --output-on-failure`
Expected: FAIL — `gradientStops` size is 0 after round-trip (not serialized yet).

- [ ] **Step 3: Write gradient_stops on save**

In the camera write block (where the reflected camera table is built via `WriteReflectedToToml("Camera", &*rec.camera)`), after the reflected table is produced and before it is inserted, append the stop array:

```cpp
toml::table camTable = WriteReflectedToToml("Camera", &*rec.camera);
if (rec.mainCamera) camTable.insert("main", true);
{
    toml::array stops;
    for (const auto& s: rec.camera->gradientStops)
    {
        toml::table st;
        st.insert("colour", toml::array{s.colour.r, s.colour.g, s.colour.b});
        st.insert("position", s.position);
        stops.push_back(std::move(st));
    }
    camTable.insert("gradient_stops", std::move(stops));
}
```

(Adapt variable names to the existing block; the existing code already inserts the `main` flag — keep that.)

- [ ] **Step 4: Read gradient_stops + migrate legacy on load**

In the camera read block (`:1380-1383`), after `ReadReflectedFromToml("Camera", *c, &cam);`:

```cpp
CameraComponent cam{};
ReadReflectedFromToml("Camera", *c, &cam);

// Back-compat: pre-background scenes used a `use_sky_gradient` bool.
const toml::node_view<const toml::node> cv{*c};
if (!cv["background"] && cv["use_sky_gradient"])
{
    cam.background = cv["use_sky_gradient"].value_or(true)
            ? CameraBackground::SkyGradient
            : CameraBackground::SolidColour;
}

// Gradient stops (custom: reflection has no list type).
if (const auto* stops = cv["gradient_stops"].as_array())
{
    cam.gradientStops.clear();
    for (const auto& node: *stops)
    {
        if (const auto* st = node.as_table())
        {
            GradientStop s{};
            if (const auto* col = (*st)["colour"].as_array(); col && col->size() >= 3)
            {
                s.colour = glm::vec3(
                        (*col)[0].value_or(0.0f),
                        (*col)[1].value_or(0.0f),
                        (*col)[2].value_or(0.0f));
            }
            s.position = glm::clamp((*st)["position"].value_or(0.0f), 0.0f, 1.0f);
            cam.gradientStops.push_back(s);
        }
    }
    std::sort(cam.gradientStops.begin(), cam.gradientStops.end(),
            [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
    if (cam.gradientStops.size() < 2)
    {
        cam.gradientStops = CameraComponent{}.gradientStops; // fall back to default pair
    }
}
```

Ensure `<algorithm>` and `<glm/common.hpp>` are included in the TU (add if missing).

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build-ninja-clang --target EngineTests && ctest --test-dir build-ninja-clang -R SceneSerializer --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app/scene/SceneSerializerToml.cpp tests/scene/SceneSerializerTests.cpp
git commit -m "feat(camera): serialize gradient stops + migrate legacy use_sky_gradient"
```

---

### Task 4: Migration + clamping tests

**Files:**
- Modify: `tests/scene/SceneSerializerTests.cpp`
- Modify: `tests/scene/SceneSerializerTests.cpp:503-512` (the existing 2D-scene load test that checked `useSkyGradient` — update assertion)

**Interfaces:**
- Consumes: everything from Tasks 1-3.

- [ ] **Step 1: Write the migration + clamping tests**

```cpp
TEST_CASE("Legacy use_sky_gradient=false migrates to SolidColour")
{
    using namespace aether;
    const std::string legacy = R"(
[[entity]]
name = "Cam"
[entity.camera]
projection = "orthographic"
use_sky_gradient = false
clear_color = [0.2, 0.3, 0.4]
main = true
)";
    const auto parsed = DeserializeSceneFromToml(legacy);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entities[0].camera.has_value());
    const auto& cam = *parsed->entities[0].camera;
    CHECK(cam.background == CameraBackground::SolidColour);
    CHECK(cam.clearColor.g == doctest::Approx(0.3f));
}

TEST_CASE("Legacy use_sky_gradient=true migrates to SkyGradient")
{
    using namespace aether;
    const std::string legacy = R"(
[[entity]]
name = "Cam"
[entity.camera]
use_sky_gradient = true
main = true
)";
    const auto parsed = DeserializeSceneFromToml(legacy);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entities[0].camera->background == CameraBackground::SkyGradient);
}

TEST_CASE("Gradient stops are clamped and sorted on load")
{
    using namespace aether;
    const std::string toml = R"(
[[entity]]
name = "Cam"
[entity.camera]
background = "gradient"
[[entity.camera.gradient_stops]]
colour = [0,0,1]
position = 1.5
[[entity.camera.gradient_stops]]
colour = [1,0,0]
position = -0.5
)";
    const auto parsed = DeserializeSceneFromToml(toml);
    REQUIRE(parsed.has_value());
    const auto& stops = parsed->entities[0].camera->gradientStops;
    REQUIRE(stops.size() == 2);
    CHECK(stops.front().position == doctest::Approx(0.0f)); // clamped from -0.5, sorted first
    CHECK(stops.back().position == doctest::Approx(1.0f));  // clamped from 1.5
}
```

Also update the existing 2D-scene test at `:511`: replace `CHECK_FALSE(scene->entities[0].camera->useSkyGradient);` with `CHECK(scene->entities[0].camera->background == aether::CameraBackground::SolidColour);` (and confirm the fixture 2D scene declares `background = "solid_colour"` or legacy `use_sky_gradient = false`; if it uses the legacy key, migration covers it).

- [ ] **Step 2: Run to verify pass/fail**

Run: `cmake --build build-ninja-clang --target EngineTests && ctest --test-dir build-ninja-clang -R SceneSerializer --output-on-failure`
Expected: all PASS (logic already implemented in Task 3). If a case fails, fix the Task 3 read logic before proceeding.

- [ ] **Step 3: Commit**

```bash
git add tests/scene/SceneSerializerTests.cpp
git commit -m "test(camera): background migration + stop clamping coverage"
```

---

### Task 5: Editor UI — mode dropdown, solid picker, gradient stop editor

**Files:**
- Modify: `src/app/debug/ComponentDrawers.cpp:344-360`

**Interfaces:**
- Consumes: `CameraComponent::{background,clearColor,gradientStops,gradientAngleDegrees}`, `CameraBackground`.

- [ ] **Step 1: Replace the background UI block**

Replace the Task 1 stand-in (`:344-356`) with the full editor:

```cpp
ImGui::SeparatorText("Background");
int background = static_cast<int>(cam->background);
constexpr const char* kBackgrounds[] = {"Solid Color", "Gradient", "Sky Gradient"};
if (PropCombo("Clear", &background, kBackgrounds, IM_ARRAYSIZE(kBackgrounds)))
{
    cam->background = static_cast<CameraBackground>(background);
    if (cam->background == CameraBackground::Gradient && cam->gradientStops.size() < 2)
    {
        cam->gradientStops = CameraComponent{}.gradientStops;
    }
}
if (cam->background == CameraBackground::SolidColour)
{
    PropColor3("Color", &cam->clearColor.x);
}
else if (cam->background == CameraBackground::Gradient)
{
    PropFloat("Angle", &cam->gradientAngleDegrees, 1.0f, -360.0f, 360.0f, "%.0f\xc2\xb0");
    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(cam->gradientStops.size()); ++i)
    {
        ImGui::PushID(i);
        auto& stop = cam->gradientStops[static_cast<std::size_t>(i)];
        ImGui::ColorEdit3("##col", &stop.colour.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##pos", &stop.position, 0.0f, 1.0f, "%.2f");
        stop.position = std::clamp(stop.position, 0.0f, 1.0f);
        if (cam->gradientStops.size() > 2)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeIdx = i;
        }
        ImGui::PopID();
    }
    if (removeIdx >= 0)
    {
        cam->gradientStops.erase(cam->gradientStops.begin() + removeIdx);
    }
    constexpr std::size_t kMaxStops = 8;
    if (cam->gradientStops.size() < kMaxStops && ImGui::SmallButton("+ Add stop"))
    {
        cam->gradientStops.push_back(GradientStop{{1.0f, 1.0f, 1.0f}, 1.0f});
    }
    std::sort(cam->gradientStops.begin(), cam->gradientStops.end(),
            [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
}
if (!isMain)
{
    ImGui::TextDisabled("Applies while this is the main camera");
}
```

Ensure `<algorithm>` is included in the TU (for `std::clamp`/`std::sort`).

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build-ninja-clang --target Editor` (or the editor target name in your build). Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add src/app/debug/ComponentDrawers.cpp
git commit -m "feat(camera): background mode + gradient stop inspector UI"
```

---

## Phase 2 — Render architecture (WYSIWYG composite)

### Task 6: Carry background state on the frame packet + remove the flat-clear hack

**Files:**
- Modify: `src/engine/rendering/RenderFramePacket.hpp:59-103`
- Modify: `src/engine/AetherCore.cpp:664-679`

**Interfaces:**
- Produces: on `RenderFramePacket`:
  ```cpp
  std::uint32_t backgroundMode = 2;      // 0 solid, 1 gradient, 2 sky
  float backgroundAngleRadians = 0.0f;
  std::uint32_t backgroundStopCount = 0;
  std::array<glm::vec4, 8> backgroundStops{}; // xyz = display colour, w = position
  ```
  (`kMaxBackgroundStops = 8`.)

- [ ] **Step 1: Add packet fields**

In `RenderFramePacket.hpp`, add near the sky fields (after line 74) and add `#include <array>`:

```cpp
static constexpr std::uint32_t kMaxBackgroundStops = 8;
std::uint32_t backgroundMode = 2; // matches CameraBackground; 2 = SkyGradient
float backgroundAngleRadians = 0.0f;
std::uint32_t backgroundStopCount = 0;
std::array<glm::vec4, kMaxBackgroundStops> backgroundStops{};
```

- [ ] **Step 2: Fill from the main camera; delete the flat-clear signalling**

In `src/engine/AetherCore.cpp`, replace the block at `:666-679` (the `skyVoidColor.w == 0` hack) with background extraction:

```cpp
// The scene background is owned by the main camera. Solid/Gradient are
// composited WYSIWYG in the tonemap pass; SkyGradient uses the procedural sky.
if (const Entity mainCamera = ecs::GetMainCameraEntity(world); mainCamera.IsValid())
{
    if (const auto* cam = world.TryGet<CameraComponent>(mainCamera); cam != nullptr)
    {
        packet.backgroundMode = static_cast<std::uint32_t>(cam->background);
        packet.backgroundAngleRadians = glm::radians(cam->gradientAngleDegrees);
        if (cam->background == CameraBackground::SolidColour)
        {
            packet.backgroundStopCount = 1;
            packet.backgroundStops[0] = glm::vec4(cam->clearColor, 0.0f);
        }
        else if (cam->background == CameraBackground::Gradient)
        {
            const std::uint32_t n = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(cam->gradientStops.size()),
                    RenderFramePacket::kMaxBackgroundStops);
            packet.backgroundStopCount = n;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                packet.backgroundStops[i] =
                        glm::vec4(cam->gradientStops[i].colour, cam->gradientStops[i].position);
            }
        }
    }
}
```

Delete the lines that set `packet.skyHorizonColor = clear; packet.skyZenithColor = clear; packet.skyVoidColor = glm::vec4(..., 0.0f);`. Keep `packet.skyVoidColor = renderer.GetSkyVoidColorVector();` at `:664` intact (sky path unchanged).

- [ ] **Step 3: Build**

Run: `cmake --build build-ninja-clang --target EngineTests`
Expected: compiles (no test asserts this yet — verified at runtime in Task 12).

- [ ] **Step 4: Commit**

```bash
git add src/engine/rendering/RenderFramePacket.hpp src/engine/AetherCore.cpp
git commit -m "feat(render): extract camera background state into frame packet"
```

---

### Task 7: Skybox — transparent in WYSIWYG mode, alpha 1 for sky, HDR clears alpha 0

**Files:**
- Modify: `src/shaders/skybox.slang:14-19` (push), `:176-235` (fragment)
- Modify: `src/engine/rendering/RenderingSubsystem.cpp:617-640` (skybox pass push + clear value)

**Interfaces:**
- Consumes: `RenderFramePacket::backgroundMode` (via the skybox push).
- Produces: skybox writes `alpha = 1` in sky mode and `alpha = 0` (transparent) in solid/gradient mode; HDR target background is `(0,0,0,0)`.

- [ ] **Step 1: Extend the skybox push and branch on mode**

In `skybox.slang`, change the push struct and fragment:

```cpp
struct SkyboxPush
{
    DevicePtr<FrameConstantsData> frame;
    uint drawSky;   // 1 = procedural sky (alpha 1); 0 = WYSIWYG background (alpha 0)
    uint _pad;
};
```

Replace the flat-clear branch (`:181-188`) with:

```cpp
    // WYSIWYG background modes composite in the tonemap pass; the sky pass just
    // marks these pixels as "not scene" (alpha 0) so the compositor fills them.
    if (pc.drawSky == 0u)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
```

At the final `return` (`:234`), keep alpha 1 (already `1.0f`).

- [ ] **Step 2: Pass drawSky + clear alpha 0 in the pass**

In `RenderingSubsystem.cpp:617-640`, set the skybox pass clear value to `(0,0,0,0)` (whatever the `AddFullscreenPass` clear-value field is — set alpha to 0 explicitly; if the API has no clear-value field, the `LoadOp::Clear` default must be confirmed to be zero including alpha). Then extend the push:

```cpp
struct SkyboxPushData { gpu::DeviceAddress frame; std::uint32_t drawSky; std::uint32_t pad; } push;
push.frame = ctx.frameConstantsAddr;
push.drawSky = (ctx.packet->backgroundMode == 2u) ? 1u : 0u; // 2 = SkyGradient
push.pad = 0u;
cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
cmd.Draw(3);
```

(Resolve how the executing lambda accesses the packet's `backgroundMode` — if `PassContext` does not expose the packet, capture the value into the pass lambda at registration time from the packet the subsystem already holds, mirroring how other per-frame scalars are captured.)

Update the comment block at `:629-631` to describe the new drawSky semantics.

- [ ] **Step 3: Recompile shaders + build**

Run: `cmake --build build-ninja-clang --target App_CompileShaders && cmake --build build-ninja-clang --target EngineAssetsPak && cmake --build build-ninja-clang`
Expected: shaders compile, engine builds.

- [ ] **Step 4: Commit**

```bash
git add src/shaders/skybox.slang src/engine/rendering/RenderingSubsystem.cpp
git commit -m "feat(render): skybox emits transparent coverage in WYSIWYG background modes"
```

---

### Task 8: Opaque forward writes coverage alpha 1

**Files:**
- Modify: `src/shaders/gltf_mesh.slang:287`

**Interfaces:**
- Produces: opaque 3D geometry writes `alpha = 1` so the tonemap compositor treats it as scene, not background.

- [ ] **Step 1: Force coverage alpha for opaque forward**

Change `:287` from `return float4(finalColor, baseColor.a);` to:

```cpp
    // Alpha carries scene coverage for the background compositor (opaque forward).
    return float4(finalColor, 1.0f);
```

(Rationale: the forward pass is opaque/depth-tested with no transparent mesh path today. If a blended mesh path is added later, it must supply its own coverage alpha.)

- [ ] **Step 2: Recompile shaders + build**

Run: `cmake --build build-ninja-clang --target App_CompileShaders && cmake --build build-ninja-clang --target EngineAssetsPak`
Expected: shaders compile.

- [ ] **Step 3: Commit**

```bash
git add src/shaders/gltf_mesh.slang
git commit -m "feat(render): opaque forward writes alpha=1 as background coverage"
```

---

### Task 9: Background params buffer + tonemap composite

**Files:**
- Modify: `src/engine/passes/PostProcessStack.hpp` (add setter + per-frame buffer + fields)
- Modify: `src/engine/passes/PostProcessStack.cpp:60-135` (create buffer), `:230-284` (fill + push)
- Modify: `src/shaders/tonemap.slang:19-32` (push), `:71-130` (composite)

**Interfaces:**
- Consumes: background state (mode, angle, stops) supplied via `PostProcessStack::SetBackgroundParams`.
- Produces: `TonemapPush` gains `uint64_t backgroundParamsAddr` (0 => pass-through). GPU buffer layout:
  ```
  struct GpuBackgroundParams { uint mode; uint stopCount; float angleRadians; uint pad;
                               float4 stops[8]; } // xyz=display colour, w=position
  ```

- [ ] **Step 1: Add the setter + storage to the header**

In `PostProcessStack.hpp`, near `SetExposure` (`:92-99`), add:

```cpp
struct BackgroundParams
{
    std::uint32_t mode = 2; // 0 solid, 1 gradient, 2 sky (composite disabled)
    std::uint32_t stopCount = 0;
    float angleRadians = 0.0f;
    std::array<glm::vec4, 8> stops{}; // xyz=display colour, w=position
};
void SetBackgroundParams(const BackgroundParams& p) { m_background = p; }
```

Add member `BackgroundParams m_background{};` and a per-frame mapped buffer array `std::array<gpu::BufferHandle, kMaxFramesInFlight> m_backgroundBuffer{};` (mirror `m_histogramOutput`). Include `<array>`/`<glm/glm.hpp>` as needed.

- [ ] **Step 2: Create the buffer in `Create`**

In `PostProcessStack::Create` (alongside the histogram buffers, `:115-128`), allocate the background params buffer per frame:

```cpp
for (auto& buf: stack.m_backgroundBuffer)
{
    buf = gpu::ResourceRegistry::CreateMappedBuffer({
            .size = sizeof(std::uint32_t) * 4 + sizeof(glm::vec4) * 8,
            .usage = gpu::BufferUsage::ShaderDeviceAddress,
            .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
            .debugName = "PostProcess.BackgroundParams",
    });
    if (!buf.IsValid()) Throw(AetherError::Engine("PostProcessStack: BackgroundParams CreateMappedBuffer failed"));
}
```

Destroy them in `Destroy()` alongside `m_histogramOutput` (`:168-175`).

- [ ] **Step 3: Fill the buffer + extend the tonemap push**

In the tonemap pass execute lambda (`:260-283`), before `PushDataRaw`, write the buffer for `ctx.frameSlot` and compute its address (0 when mode == sky):

```cpp
std::uint64_t bgAddr = 0;
if (m_background.mode != 2u)
{
    const std::uint32_t slot = ctx.frameSlot % kMaxFramesInFlight;
    const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_backgroundBuffer[slot]);
    if (view.mappedPtr)
    {
        auto* p = static_cast<std::uint8_t*>(view.mappedPtr);
        const std::uint32_t header[4] = {m_background.mode, m_background.stopCount, 0u, 0u};
        std::memcpy(p, header, sizeof(header));
        std::memcpy(p + 8, &m_background.angleRadians, sizeof(float)); // header[2] slot
        std::memcpy(p + 16, m_background.stops.data(), sizeof(glm::vec4) * 8);
        bgAddr = gpu::ResourceRegistry::ResolveBufferDeviceAddress(m_backgroundBuffer[slot]);
    }
}
```

(Match the exact device-address resolver name used elsewhere in this file, e.g. how `shadowLightDataAddr`-style BDA buffers resolve their address.)

Extend the push struct (`:260-271`) with a trailing `std::uint64_t backgroundParamsAddr;` and set `push.backgroundParamsAddr = bgAddr;`. Keep 8-byte alignment (place the `uint64_t` last).

- [ ] **Step 4: Composite in the shader**

In `tonemap.slang`, extend `TonemapPush` (`:20-31`) with a trailing `uint64_t backgroundParamsAddr;` and add a background struct + eval above `fragmentMain`:

```cpp
struct GpuBackgroundParams
{
    uint  mode;         // 0 solid, 1 gradient
    uint  stopCount;
    float angleRadians;
    uint  _pad;
    float4 stops[8];    // xyz = display-space colour, w = position
};

float3 EvalBackground(GpuBackgroundParams* bg, float2 uv /*0..1, y-down*/)
{
    if (bg->mode == 0u || bg->stopCount <= 1u)
        return bg->stops[0].rgb;

    // Project the pixel onto the gradient axis (0 deg = top->bottom).
    const float2 dir = float2(sin(bg->angleRadians), cos(bg->angleRadians));
    float t = saturate(dot(uv - 0.5f, dir) + 0.5f);

    float3 col = bg->stops[0].rgb;
    for (uint i = 1u; i < bg->stopCount; ++i)
    {
        const float p0 = bg->stops[i - 1u].w;
        const float p1 = bg->stops[i].w;
        const float seg = saturate((t - p0) / max(p1 - p0, 1e-5f));
        col = (t >= p0) ? lerp(bg->stops[i - 1u].rgb, bg->stops[i].rgb, seg) : col;
    }
    return col;
}
```

In `fragmentMain` (non-debug branch), after computing `mapped` + gamma, composite:

```cpp
    float3 outColor = pow(max(mapped, float3(0.0f)), float3(1.0f / 2.2f));
    if (pc.backgroundParamsAddr != 0ull)
    {
        GpuBackgroundParams* bg = (GpuBackgroundParams*)pc.backgroundParamsAddr;
        const float  coverage = g_textures[pc.hdrSlot].Load(int3(coord, 0)).a;
        const float2 uv = (float2(coord) + 0.5f) / float2(max(pc.screenWidth,1u), max(pc.screenHeight,1u));
        const float3 bgCol = EvalBackground(bg, uv);
        outColor = lerp(bgCol, outColor, saturate(coverage));
    }
    return float4(outColor, 1.0f);
```

Note the background is authored in display space, so it is NOT re-gamma'd — it is mixed after the scene's gamma step. Remove the old unconditional `return float4(gamma, 1.0f);` and use `outColor`.

- [ ] **Step 5: Wire the setter per-frame**

In `RenderingSubsystem.cpp`, where per-frame post-process config is set (near `SetExposure`/`SetOutputToTexture`, `:423`/`:709`), copy the packet's background into the stack before the post-process passes register:

```cpp
PostProcessStack::BackgroundParams bg;
bg.mode = packet.backgroundMode;
bg.stopCount = packet.backgroundStopCount;
bg.angleRadians = packet.backgroundAngleRadians;
bg.stops = packet.backgroundStops;
m_postProcessStack.SetBackgroundParams(bg);
```

(Use the packet reference available at that call site; match the field/array types.)

- [ ] **Step 6: Recompile shaders + build**

Run: `cmake --build build-ninja-clang --target App_CompileShaders && cmake --build build-ninja-clang --target EngineAssetsPak && cmake --build build-ninja-clang`
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add src/engine/passes/PostProcessStack.hpp src/engine/passes/PostProcessStack.cpp src/shaders/tonemap.slang src/engine/rendering/RenderingSubsystem.cpp
git commit -m "feat(render): composite WYSIWYG background in display space at tonemap"
```

---

## Phase 3 — Sun gizmo fix

### Task 10: Gate the sun gizmo to 3D scenes with a real directional source

**Files:**
- Modify: `src/app/debug/LightingPanel.cpp:130-140` and the `AddLightGizmos` call site (`:149-162`)

**Interfaces:**
- Consumes: scene kind (from the world / scene features), presence of a `DayNightComponent` driver or an explicit directional light.

- [ ] **Step 1: Compute whether a real directional source exists**

At the `AddLightGizmos` call site, gate `options.sunDirection` on both scene kind and an actual source. Add a helper (or inline) that returns true when the world has a live `DayNightComponent` (not disabled) or an explicit directional light component, AND the scene is 3D (`world.GetSceneKind() != SceneKind::Scene2D`). Pass the result:

```cpp
const bool sceneHasSun =
        world.GetSceneKind() != SceneKind::Scene2D && SceneHasDirectionalSource(world);
...
LightGizmoOptions{
        ...
        .sunDirection = m_lightGizmoSunDirection && sceneHasSun,
        ...
},
```

Implement `SceneHasDirectionalSource(const World&)` in the anonymous namespace: true if any live `DayNightComponent` exists (reuse the `HasDisabledAncestor` guard pattern from `DayNightSystem::FindDriver`) or any directional light component exists. Do **not** fall back to `renderer.GetDirectionalLightDirection()` (that is always the renderer default).

- [ ] **Step 2: Build**

Run: `cmake --build build-ninja-clang --target Editor`
Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add src/app/debug/LightingPanel.cpp
git commit -m "fix(editor): only draw sun gizmo in 3D scenes with a real directional light"
```

---

## Phase 4 — Defaults + runtime verification

### Task 11: 2D scenes default to Solid Colour

**Files:**
- Modify: `src/app/debug/HierarchyPanel.cpp:1417-1422` (2D camera menu)
- Modify: `resources/scenes/default2d.scene.toml`

**Interfaces:**
- Consumes: `CameraComponent::background`.

- [ ] **Step 1: New 2D camera defaults to Solid Colour**

In `HierarchyPanel.cpp:1417-1422`, after `camera.projection = CameraProjection::Orthographic;`, add `camera.background = CameraBackground::SolidColour;`.

- [ ] **Step 2: Default 2D scene file**

In `resources/scenes/default2d.scene.toml`, in the camera table, set `background = "solid_colour"` (and keep/rename any `clear_color`). Remove `use_sky_gradient` if present (migration would handle it, but the shipped default should use the new key). Do the same for any other 2D sample scenes touched by tests (`resources/scenes/default2d.scene.toml` and the 2D fixture referenced by `SceneSerializerTests`).

- [ ] **Step 3: Build + serializer tests**

Run: `cmake --build build-ninja-clang --target EngineTests && ctest --test-dir build-ninja-clang -R SceneSerializer --output-on-failure`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/app/debug/HierarchyPanel.cpp resources/scenes/default2d.scene.toml
git commit -m "feat(camera): 2D scenes default to solid-colour background"
```

---

### Task 12: Runtime verification (aethercore MCP)

**Files:** none (verification only). Produces a short findings note; fix regressions in the owning task if any check fails.

**Preconditions:** editor built and running from the build-tree root; aethercore MCP connected (`mcp__aethercore__*`).

- [ ] **Step 1: WYSIWYG solid-colour matches the swatch (2D)**

Create/open a 2D scene, set the main camera `background = SolidColour`, `clear_color = [0.80, 0.15, 0.20]`. `mcp__aethercore__screenshot` the viewport, then sample a background pixel (via `capture_texture`/screenshot inspection). Assert the sampled display-space colour ≈ `(0.80, 0.15, 0.20)` within a small tolerance (e.g. each channel within ±0.03 after sRGB display encoding). This is the core mismatch fix.

- [ ] **Step 2: Vertical gradient end stops (2D)**

Set `background = Gradient`, angle 0, stops `{[1,0,0]@0, [0,0,1]@1}`. Screenshot; sample top-centre ≈ red and bottom-centre ≈ blue.

- [ ] **Step 3: 3D sky unchanged**

Open a 3D scene with a DayNight driver (`background = SkyGradient`). Screenshot; spot-check that the sky renders as before (compare against a pre-change capture or visually confirm gradient + sun). No black background, no flat fill.

- [ ] **Step 4: 3D solid background coverage**

In a 3D scene, set `background = SolidColour` to a known colour with a mesh in view. Confirm the mesh renders tonemapped over an exact background (background pixels match the swatch; mesh pixels unaffected). This validates alpha coverage with real depth-tested geometry.

- [ ] **Step 5: Sun gizmo gone in 2D**

In the 2D scene with light gizmos + debug rendering enabled, confirm via screenshot there is no sun line/sphere/ring. Then confirm it IS present in the 3D DayNight scene.

- [ ] **Step 6: Record results**

Note pass/fail for each check in the PR description / commit message. If any check fails, return to the owning task (Steps: Task 9 for composite/colour, Task 7/8 for coverage, Task 10 for gizmo) and fix before completion.

- [ ] **Step 7: Final commit (if any fixes were applied)**

```bash
git add -A
git commit -m "test(render): runtime verification of WYSIWYG camera background"
```

---

## Self-Review

**Spec coverage:**
- Data model (enum/stops/angle, remove `useSkyGradient`) → Task 1. ✓
- Defaults by scene kind → Task 11. ✓
- WYSIWYG display-space composite + coverage convention → Tasks 6-9. ✓
- SkyGradient unchanged (alpha 1) → Tasks 7 (sky returns alpha 1), 8 (forward alpha 1), 9 (addr 0 = pass-through). ✓
- Background params via BDA buffer, not FrameConstants → Task 9. ✓
- Skybox flat-clear hack removed → Tasks 6 (extraction) + 7 (shader). ✓
- Sun gizmo gating → Task 10. ✓
- Reflection + serialization + legacy migration → Tasks 2-4. ✓
- Editor UI (mode + stop list + angle) → Task 5. ✓
- Testing (serializer unit + runtime MCP) → Tasks 4, 12. ✓
- Exposure regression (auto-exposure) — confirmed manual during planning; no histogram masking needed. ✓ (documented, no task required)

**Placeholder scan:** No TBD/TODO; each code step shows real code. Two deliberate "match the existing name" notes (device-address resolver in Task 9; serialize/deserialize entry-point names in Task 3; packet access in the skybox lambda in Task 7) — these are lookups against existing symbols in the named file, not unfinished logic.

**Type consistency:** `CameraBackground` values {SolidColour=0, Gradient=1, SkyGradient=2} used consistently across component, reflection, extraction (`backgroundMode` uint mirrors the enum), skybox `drawSky` (mode==2 → sky), and tonemap (mode!=2 → composite). `GradientStop{colour,position}` consistent across model, TOML, UI, packet (`vec4 xyz=colour,w=position`), and shader `stops[].xyz/.w`. `kMaxBackgroundStops = 8` consistent (packet, buffer size, shader array, UI cap). `backgroundParamsAddr` (0 = disabled) consistent across push (C++/Slang) and shader guard.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-17-camera-background.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
