# Editor ImGui Bridge + Project Windows — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let project C# implement `IEditorWindow` and draw real ImGui in the editor via an immediate-mode `EditorGui.*` API — a generic hook with no game code in engine/app/editor.

**Architecture:** C++→C# adds a `DrawEditorWindows` entrypoint to the existing `ManagedScriptApi` struct (same wiring as `InvokeUpdate`); the editor's ImGui pass pumps it. C#→C++ adds `aether_editorgui_*` exports (ImGui calls) behind `EditorGui.*` P/Invokes. `IEditorWindow` implementers are discovered by reflection like `EntityScript` types.

**Tech Stack:** C# (CoreCLR interop, `[UnmanagedCallersOnly]`, `LibraryImport`), C++20 (Dear ImGui), CMake.

**Design spec:** `docs/superpowers/specs/2026-07-22-editor-imgui-bridge-design.md`. This is **Spec 1**; the dialogue node-graph editor is Spec 2 (separate).

## Global Constraints

- **Boundary:** bridge is generic — `src/app` + `managed/AetherCore` + `managed/AetherCore.Interop`, no game knowledge. Consumers live in `projects/<game>/`.
- **GameRuntime has no ImGui** (CMake filters out `imgui/`, `editor/`, `debug/`). `EditorGuiExports.cpp` must be **excluded from GameRuntime** — it may never require ImGui in a shipped build.
- **ABI struct parity:** `ManagedScriptApi` exists in BOTH `managed/AetherCore.Interop/Abi.cs` and `src/engine/scripting/ManagedInterop.hpp`; `Bootstrap.Init` asserts `sizeof` equality. Any field added must be appended to BOTH at the same position.
- New interop `.cpp` needs a `cmake -S . -B build/vs2022-msvc` reconfigure or the Editor omits it (`EntryPointNotFoundException`).
- Managed csprojs are SDK-style (glob `**/*.cs`) — new `.cs` needs no csproj edit.
- Verify in-editor via the aethercore MCP; **never** desktop control. Finish by fast-forward merge + push to `master`. Address the user as "Pog Champ".

## Reference patterns (verified)

- **ABI field wiring:** `Abi.cs` `struct ManagedScriptApi` (fields are `delegate* unmanaged<...>`), mirrored in `ManagedInterop.hpp` (`ReturnType (*Name)(args) = nullptr;`), filled in `Bootstrap.Init` (`outApi->InvokeUpdate = &ScriptRegistry.InvokeUpdate;`).
- **Managed entrypoint:** `[UnmanagedCallersOnly] internal static void InvokeUpdate(...)` in `ScriptRegistry.cs`, try/catch → `Bootstrap.ReportError`.
- **Type discovery:** `ScriptRegistry.LoadScripts` loops `assembly.GetTypes()`, filters `!type.IsAbstract && typeof(EntityScript).IsAssignableFrom(type)`, instantiates via `Activator.CreateInstance`.
- **App→api call:** `ScriptComponentSystem` does `const auto* api = cs.Api();` then `api->InvokeUpdate(handle, dt)` where `cs` is the `CSharpScriptingSubsystem` (from `LayerContext`).
- **String-return interop:** `UiExports.cpp aether_ui_get_text` (`char* buf, int len` → memcpy, return length); managed `Ui.GetText` (`stackalloc`, `fixed`, `Encoding.UTF8.GetString`).
- **CMake runtime exclusion:** `list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "[/\\\\]imgui[/\\\\]")` in `src/app/CMakeLists.txt`.

---

## Task 1: ABI — add the `DrawEditorWindows` entrypoint

**Files:**
- Modify: `managed/AetherCore.Interop/Abi.cs` (append struct field)
- Modify: `src/engine/scripting/ManagedInterop.hpp` (append mirror field)
- Modify: `managed/AetherCore.Interop/Bootstrap.cs` (wire the pointer)
- Modify: `managed/AetherCore.Interop/ScriptRegistry.cs` (the entrypoint + an empty window list)

**Interfaces:**
- Produces: `ManagedScriptApi.DrawEditorWindows` (native `void(*)()`); `ScriptRegistry.DrawEditorWindows()`.

- [ ] **Step 1: Append the managed struct field.** In `Abi.cs`, at the END of `struct ManagedScriptApi` (after `GetDefaultProperty`):
```csharp
    // Editor tooling (editor-only; never called from a shipped GameRuntime).
    public delegate* unmanaged<void> DrawEditorWindows;
```

- [ ] **Step 2: Append the native mirror.** In `ManagedInterop.hpp`, at the END of `struct ManagedScriptApi` (after `GetDefaultProperty`):
```cpp
		void (*DrawEditorWindows)() = nullptr;
```

- [ ] **Step 3: Add the entrypoint + window list to ScriptRegistry.** In `ScriptRegistry.cs`, add a field near the other statics:
```csharp
    private static readonly List<IEditorWindow> s_editorWindows = new();
```
Add the entrypoint (near `InvokeUpdate`):
```csharp
    [UnmanagedCallersOnly]
    internal static void DrawEditorWindows()
    {
        // Editor-only pump: each project IEditorWindow draws its own ImGui window. A throwing
        // window is isolated so it can't take down the editor.
        for (int i = 0; i < s_editorWindows.Count; i++)
        {
            try { s_editorWindows[i].OnGui(); }
            catch (Exception ex) { Bootstrap.ReportError($"{s_editorWindows[i].GetType().Name}.OnGui: {ex}"); }
        }
    }
```
Ensure `ResetRegistry()` (wherever `s_types`/`s_props` are cleared) also does `s_editorWindows.Clear();`.

- [ ] **Step 4: Wire the pointer in Bootstrap.** In `Bootstrap.cs Init`, after the other `outApi->... = &ScriptRegistry....;` lines:
```csharp
        outApi->DrawEditorWindows = &ScriptRegistry.DrawEditorWindows;
```
*(`IEditorWindow` doesn't exist yet — Step from Task 2 adds it; to keep Task 1 compiling, temporarily use `List<object>` and `((IEditorWindow)...)` only after Task 2. Simplest: do Task 2 Step 1 (the interface file) FIRST, then Task 1 compiles cleanly. Reorder locally if needed.)*

- [ ] **Step 5: Build the managed assemblies + Editor.**
Run:
```bash
cmake --build build/vs2022-msvc --target Editor --config Debug
```
Expected: builds; `Bootstrap.Init` size assert passes at editor launch (verified in Task 6). No ABI mismatch.

- [ ] **Step 6: Commit.**
```bash
git add -A && git commit -m "Add DrawEditorWindows entrypoint to the managed script ABI"
```

---

## Task 2: `IEditorWindow` interface + discovery

**Files:**
- Create: `managed/AetherCore/IEditorWindow.cs`
- Modify: `managed/AetherCore.Interop/ScriptRegistry.cs` (discover implementers)

**Interfaces:**
- Produces: `AetherCore.IEditorWindow { string Title { get; } void OnGui(); }`; populated `s_editorWindows`.

- [ ] **Step 1: Define the interface.** Create `managed/AetherCore/IEditorWindow.cs`:
```csharp
namespace AetherCore;

/// <summary>Implement on any class in a project (or the SDK) to add a window to the editor. The editor
/// discovers implementers by reflection (like EntityScript) and calls OnGui() each editor frame; draw
/// with the EditorGui.* API. Editor/dev-tooling only - never invoked in a shipped game.</summary>
public interface IEditorWindow
{
    /// <summary>Window title (also its ImGui id). Keep it stable + unique.</summary>
    string Title { get; }

    /// <summary>Called once per editor frame. Do your own EditorGui.Begin(Title, ...) / End here.</summary>
    void OnGui();
}
```

- [ ] **Step 2: Discover + instantiate in LoadScripts.** In `ScriptRegistry.cs`, inside the `foreach (Type type in assembly.GetTypes())` loop (alongside the `EntityScript` handling), add:
```csharp
            if (!type.IsAbstract && typeof(IEditorWindow).IsAssignableFrom(type))
            {
                try { s_editorWindows.Add((IEditorWindow)Activator.CreateInstance(type)!); }
                catch (Exception ex) { Bootstrap.ReportError($"IEditorWindow {type.Name} ctor: {ex}"); }
            }
```
(Windows and scripts are not mutually exclusive types, so this is a separate `if`, not `else`.) `s_editorWindows.Clear()` already runs in reset (Task 1 Step 3) so reload re-discovers.

- [ ] **Step 3: Build.**
```bash
cmake --build build/vs2022-msvc --target Editor --config Debug
```
Expected: builds. (No visible effect until the pump + EditorGui exist; a window that calls nothing draws nothing.)

- [ ] **Step 4: Commit.**
```bash
git add -A && git commit -m "Add IEditorWindow interface and reflection discovery"
```

---

## Task 3: C++ `EditorGui` interop surface

**Files:**
- Create: `src/app/scripting/interop/EditorGuiExports.cpp`
- Modify: `src/app/CMakeLists.txt` (exclude from GameRuntime)

**Interfaces:**
- Produces: `aether_editorgui_*` exports (the surface in spec §1). Consumed by `EditorGui.cs` (Task 4).

- [ ] **Step 1: Write the interop.** Create `src/app/scripting/interop/EditorGuiExports.cpp`. It uses ImGui directly (this TU only ever compiles where ImGui is linked — Editor). `Vec2`/`Vec4` come from `InteropCommon.hpp`. Full surface — representative functions shown; implement the rest with the identical 1:1 ImGui mapping:
```cpp
// Immediate-mode ImGui bridge for project IEditorWindow tools. Editor-only: this TU is EXCLUDED from
// GameRuntime (which links no ImGui) via src/app/CMakeLists.txt. Each function is a thin wrapper over
// the live ImGui context, which is valid because the editor calls DrawEditorWindows synchronously
// inside its own ImGui frame on the main thread.

#include "scripting/interop/InteropCommon.hpp"

#include <imgui.h>

#include <cstring>

using namespace aether::app::scripting::interop;

namespace
{
    inline ImVec2 V2(Vec2 v) { return {v.x, v.y}; }
    inline ImVec4 V4(Vec4 v) { return {v.x, v.y, v.z, v.w}; }
    inline ImU32  U32(Vec4 v) { return ImGui::ColorConvertFloat4ToU32(V4(v)); }
}

// ── Windows / layout ────────────────────────────────────────────────────────
AE_SCRIPT_API std::int32_t aether_editorgui_begin(const char* title, std::int32_t* open)
{
    bool o = open != nullptr ? (*open != 0) : true;
    const bool visible = ImGui::Begin(title, open != nullptr ? &o : nullptr);
    if (open != nullptr) { *open = o ? 1 : 0; }
    return visible ? 1 : 0;
}
AE_SCRIPT_API void aether_editorgui_end() { ImGui::End(); }

AE_SCRIPT_API std::int32_t aether_editorgui_begin_child(const char* id, Vec2 size, std::int32_t border)
{
    return ImGui::BeginChild(id, V2(size), border != 0) ? 1 : 0;
}
AE_SCRIPT_API void aether_editorgui_end_child() { ImGui::EndChild(); }

AE_SCRIPT_API void aether_editorgui_same_line() { ImGui::SameLine(); }
AE_SCRIPT_API void aether_editorgui_separator() { ImGui::Separator(); }
AE_SCRIPT_API void aether_editorgui_spacing() { ImGui::Spacing(); }
AE_SCRIPT_API Vec2 aether_editorgui_content_avail() { const ImVec2 a = ImGui::GetContentRegionAvail(); return {a.x, a.y}; }
AE_SCRIPT_API Vec2 aether_editorgui_cursor_screen_pos() { const ImVec2 p = ImGui::GetCursorScreenPos(); return {p.x, p.y}; }
AE_SCRIPT_API void aether_editorgui_set_cursor_screen_pos(Vec2 p) { ImGui::SetCursorScreenPos(V2(p)); }

// ── Text / widgets ──────────────────────────────────────────────────────────
AE_SCRIPT_API void aether_editorgui_text(const char* s) { ImGui::TextUnformatted(s); }
AE_SCRIPT_API void aether_editorgui_text_colored(Vec4 col, const char* s) { ImGui::TextColored(V4(col), "%s", s); }
AE_SCRIPT_API std::int32_t aether_editorgui_button(const char* label, Vec2 size) { return ImGui::Button(label, V2(size)) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_small_button(const char* label) { return ImGui::SmallButton(label) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_checkbox(const char* label, std::int32_t* v)
{
    bool b = v != nullptr && *v != 0;
    const bool changed = ImGui::Checkbox(label, &b);
    if (v != nullptr) { *v = b ? 1 : 0; }
    return changed ? 1 : 0;
}
AE_SCRIPT_API std::int32_t aether_editorgui_input_text(const char* label, char* buf, std::int32_t bufLen)
{
    return ImGui::InputText(label, buf, static_cast<std::size_t>(bufLen)) ? 1 : 0;
}
AE_SCRIPT_API std::int32_t aether_editorgui_input_float(const char* label, float* v) { return ImGui::InputFloat(label, v) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_selectable(const char* label, std::int32_t selected) { return ImGui::Selectable(label, selected != 0) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_tree_node(const char* label) { return ImGui::TreeNode(label) ? 1 : 0; }
AE_SCRIPT_API void aether_editorgui_tree_pop() { ImGui::TreePop(); }

// Combo over a newline-joined items string (managed joins string[] with '\n'); returns 1 on change.
AE_SCRIPT_API std::int32_t aether_editorgui_combo(const char* label, std::int32_t* current, const char* itemsNewlineJoined)
{
    // Build a '\0'-separated + double-'\0'-terminated buffer as ImGui::Combo expects.
    std::string items(itemsNewlineJoined);
    for (char& c : items) { if (c == '\n') { c = '\0'; } }
    items.push_back('\0');
    int cur = current != nullptr ? *current : 0;
    const bool changed = ImGui::Combo(label, &cur, items.c_str());
    if (current != nullptr) { *current = cur; }
    return changed ? 1 : 0;
}

// ── Canvas draw list ────────────────────────────────────────────────────────
AE_SCRIPT_API void aether_editorgui_add_line(Vec2 a, Vec2 b, Vec4 col, float thick) { ImGui::GetWindowDrawList()->AddLine(V2(a), V2(b), U32(col), thick); }
AE_SCRIPT_API void aether_editorgui_add_rect_filled(Vec2 mn, Vec2 mx, Vec4 col, float rounding) { ImGui::GetWindowDrawList()->AddRectFilled(V2(mn), V2(mx), U32(col), rounding); }
AE_SCRIPT_API void aether_editorgui_add_rect(Vec2 mn, Vec2 mx, Vec4 col, float rounding, float thick) { ImGui::GetWindowDrawList()->AddRect(V2(mn), V2(mx), U32(col), rounding, 0, thick); }
AE_SCRIPT_API void aether_editorgui_add_bezier(Vec2 p1, Vec2 p2, Vec2 p3, Vec2 p4, Vec4 col, float thick) { ImGui::GetWindowDrawList()->AddBezierCubic(V2(p1), V2(p2), V2(p3), V2(p4), U32(col), thick); }
AE_SCRIPT_API void aether_editorgui_add_circle_filled(Vec2 c, float r, Vec4 col) { ImGui::GetWindowDrawList()->AddCircleFilled(V2(c), r, U32(col)); }
AE_SCRIPT_API void aether_editorgui_add_text(Vec2 p, Vec4 col, const char* s) { ImGui::GetWindowDrawList()->AddText(V2(p), U32(col), s); }

// ── Interaction ─────────────────────────────────────────────────────────────
AE_SCRIPT_API std::int32_t aether_editorgui_invisible_button(const char* id, Vec2 size) { return ImGui::InvisibleButton(id, V2(size)) ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_active() { return ImGui::IsItemActive() ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_hovered() { return ImGui::IsItemHovered() ? 1 : 0; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_item_clicked() { return ImGui::IsItemClicked() ? 1 : 0; }
AE_SCRIPT_API Vec2 aether_editorgui_mouse_pos() { const ImVec2 p = ImGui::GetIO().MousePos; return {p.x, p.y}; }
AE_SCRIPT_API Vec2 aether_editorgui_mouse_drag_delta() { const ImVec2 d = ImGui::GetMouseDragDelta(); return {d.x, d.y}; }
AE_SCRIPT_API std::int32_t aether_editorgui_is_mouse_dragging() { return ImGui::IsMouseDragging(ImGuiMouseButton_Left) ? 1 : 0; }
AE_SCRIPT_API float aether_editorgui_mouse_wheel() { return ImGui::GetIO().MouseWheel; }
```

- [ ] **Step 2: Exclude from GameRuntime.** In `src/app/CMakeLists.txt`, next to the imgui filter, add:
```cmake
list(FILTER APP_RUNTIME_SOURCES EXCLUDE REGEX "EditorGuiExports")
```
(The Launcher already excludes `scripting/`, so no Launcher change is needed.)

- [ ] **Step 3: Reconfigure + build Editor AND GameRuntime.**
```bash
cmake -S . -B build/vs2022-msvc
cmake --build build/vs2022-msvc --target Editor GameRuntime --config Debug
```
Expected: **both** build. GameRuntime building proves ImGui didn't leak into the shipped runtime.

- [ ] **Step 4: Commit.**
```bash
git add -A && git commit -m "Add EditorGui ImGui interop surface (editor-only)"
```

---

## Task 4: Managed `EditorGui` API

**Files:**
- Modify: `managed/AetherCore/Internal/Native.cs` (P/Invokes)
- Create: `managed/AetherCore/EditorGui.cs`

**Interfaces:**
- Produces: `AetherCore.EditorGui` static class (spec §1 surface). Consumed by any `IEditorWindow`.

- [ ] **Step 1: Add P/Invokes.** In `Native.cs`, add one `[LibraryImport(Lib)]` per export (string args → `StringMarshalling.Utf8`; `ref`/out via pointers). Representative:
```csharp
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_begin(string title, int* open);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_end();
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_button(string label, Vector2 size);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_input_text(string label, byte* buf, int bufLen);
    [LibraryImport(Lib)] internal static partial void aether_editorgui_add_bezier(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4, Vector4 col, float thick);
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_editorgui_invisible_button(string id, Vector2 size);
    [LibraryImport(Lib)] internal static partial Vector2 aether_editorgui_mouse_pos();
    // ... one per export from Task 3 (identical mapping) ...
```

- [ ] **Step 2: Write the public API.** Create `managed/AetherCore/EditorGui.cs`. `Begin`/`Checkbox`/`InputText` use `ref` + fixed pointers; `Combo` joins items with `\n`. Representative:
```csharp
using System;
using System.Numerics;
using System.Text;

namespace AetherCore;

/// <summary>Immediate-mode ImGui, callable from an IEditorWindow.OnGui(). Editor/dev-tooling only;
/// these resolve to the editor's live ImGui context. No-ops (never invoked) in a shipped game.</summary>
public static unsafe class EditorGui
{
    public static bool Begin(string title) { int open = 1; return Native.aether_editorgui_begin(title, &open) != 0; }
    public static bool Begin(string title, ref bool open)
    {
        int o = open ? 1 : 0;
        bool visible = Native.aether_editorgui_begin(title, &o) != 0;
        open = o != 0;
        return visible;
    }
    public static void End() => Native.aether_editorgui_end();

    public static bool BeginChild(string id, Vector2 size = default, bool border = false) => Native.aether_editorgui_begin_child(id, size, border ? 1 : 0) != 0;
    public static void EndChild() => Native.aether_editorgui_end_child();
    public static void SameLine() => Native.aether_editorgui_same_line();
    public static void Separator() => Native.aether_editorgui_separator();
    public static void Spacing() => Native.aether_editorgui_spacing();
    public static Vector2 ContentAvail() => Native.aether_editorgui_content_avail();
    public static Vector2 CursorScreenPos() => Native.aether_editorgui_cursor_screen_pos();
    public static void SetCursorScreenPos(Vector2 p) => Native.aether_editorgui_set_cursor_screen_pos(p);

    public static void Text(string s) => Native.aether_editorgui_text(s);
    public static void TextColored(Vector4 color, string s) => Native.aether_editorgui_text_colored(color, s);
    public static bool Button(string label, Vector2 size = default) => Native.aether_editorgui_button(label, size) != 0;
    public static bool SmallButton(string label) => Native.aether_editorgui_small_button(label) != 0;
    public static bool Checkbox(string label, ref bool v)
    {
        int i = v ? 1 : 0; bool changed = Native.aether_editorgui_checkbox(label, &i) != 0; v = i != 0; return changed;
    }
    public static bool InputText(string label, ref string text, int maxLen = 256)
    {
        Span<byte> buf = maxLen <= 512 ? stackalloc byte[maxLen] : new byte[maxLen];
        int n = Encoding.UTF8.GetBytes(text, buf);
        if (n < buf.Length) { buf[n] = 0; } else { buf[^1] = 0; }
        bool changed;
        fixed (byte* p = buf) { changed = Native.aether_editorgui_input_text(label, p, buf.Length) != 0; }
        if (changed)
        {
            fixed (byte* p = buf) { int len = 0; while (len < buf.Length && p[len] != 0) len++; text = Encoding.UTF8.GetString(p, len); }
        }
        return changed;
    }
    public static bool Combo(string label, ref int current, string[] items)
    {
        int cur = current; bool changed = Native.aether_editorgui_combo(label, &cur, string.Join('\n', items)) != 0; current = cur; return changed;
    }
    public static bool Selectable(string label, bool selected = false) => Native.aether_editorgui_selectable(label, selected ? 1 : 0) != 0;
    public static bool TreeNode(string label) => Native.aether_editorgui_tree_node(label) != 0;
    public static void TreePop() => Native.aether_editorgui_tree_pop();

    // Canvas draw list
    public static void AddLine(Vector2 a, Vector2 b, Vector4 color, float thickness = 1f) => Native.aether_editorgui_add_line(a, b, color, thickness);
    public static void AddRectFilled(Vector2 min, Vector2 max, Vector4 color, float rounding = 0f) => Native.aether_editorgui_add_rect_filled(min, max, color, rounding);
    public static void AddRect(Vector2 min, Vector2 max, Vector4 color, float rounding = 0f, float thickness = 1f) => Native.aether_editorgui_add_rect(min, max, color, rounding, thickness);
    public static void AddBezierCubic(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4, Vector4 color, float thickness = 1f) => Native.aether_editorgui_add_bezier(p1, p2, p3, p4, color, thickness);
    public static void AddCircleFilled(Vector2 center, float radius, Vector4 color) => Native.aether_editorgui_add_circle_filled(center, radius, color);
    public static void AddText(Vector2 pos, Vector4 color, string s) => Native.aether_editorgui_add_text(pos, color, s);

    // Interaction
    public static bool InvisibleButton(string id, Vector2 size) => Native.aether_editorgui_invisible_button(id, size) != 0;
    public static bool IsItemActive() => Native.aether_editorgui_is_item_active() != 0;
    public static bool IsItemHovered() => Native.aether_editorgui_is_item_hovered() != 0;
    public static bool IsItemClicked() => Native.aether_editorgui_is_item_clicked() != 0;
    public static Vector2 MousePos() => Native.aether_editorgui_mouse_pos();
    public static Vector2 MouseDragDelta() => Native.aether_editorgui_mouse_drag_delta();
    public static bool IsMouseDragging() => Native.aether_editorgui_is_mouse_dragging() != 0;
    public static float MouseWheel() => Native.aether_editorgui_mouse_wheel();
}
```

- [ ] **Step 3: Build.**
```bash
cmake --build build/vs2022-msvc --target Editor --config Debug
```
Expected: managed assemblies compile (verifies every P/Invoke has a matching export signature).

- [ ] **Step 4: Commit.**
```bash
git add -A && git commit -m "Add managed EditorGui immediate-mode API"
```

---

## Task 5: Editor pump

**Files:**
- Modify: `src/app/layers/DebugLayer.cpp` (call `DrawEditorWindows` in the ImGui pass)

- [ ] **Step 1: Pump the windows.** In `DebugLayer`'s per-frame ImGui code (where built-in panels are drawn), after the panel loop, obtain the scripting subsystem the way `ScriptComponentSystem` does (from `LayerContext`) and call:
```cpp
if (const aether::scripting::CSharpScriptingSubsystem* cs = /* subsystem from context */; cs != nullptr)
{
    if (const auto* api = cs->Api(); api != nullptr && api->DrawEditorWindows != nullptr)
    {
        api->DrawEditorWindows();
    }
}
```
Verify the exact accessor against `ScriptComponentSystem.cpp` (`cs.Api()`), and confirm the subsystem is reachable from `DebugLayer`'s `LayerContext` (route it through the context if not already exposed). This runs every editor frame (edit + play), so windows work while authoring.

- [ ] **Step 2: Build.**
```bash
cmake --build build/vs2022-msvc --target Editor --config Debug
```

- [ ] **Step 3: Commit.**
```bash
git add -A && git commit -m "Pump project IEditorWindow draws from the editor ImGui pass"
```

---

## Task 6: Validate end-to-end with a demo window

**Files:**
- Create (temporary): `projects/INKBOUND/scripts/DemoEditorWindow.cs`

- [ ] **Step 1: Write a demo window.**
```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

// TEMPORARY - proves the EditorGui round trip. Deleted after Task 6.
public sealed class DemoEditorWindow : IEditorWindow
{
    public string Title => "Demo Window";
    private bool _open = true;
    private string _text = "type here";

    public void OnGui()
    {
        if (!EditorGui.Begin(Title, ref _open)) { EditorGui.End(); return; }
        EditorGui.Text("EditorGui bridge is live.");
        if (EditorGui.Button("Log me")) { Log.Info("[DEMO] EditorGui button clicked"); }
        EditorGui.InputText("field", ref _text);
        Vector2 o = EditorGui.CursorScreenPos();
        EditorGui.AddLine(o, o + new Vector2(180, 40), new Vector4(0.30f, 0.85f, 1f, 1f), 2f);
        EditorGui.AddBezierCubic(o, o + new Vector2(60, 60), o + new Vector2(120, -20), o + new Vector2(180, 40),
                                 new Vector4(1f, 0.5f, 0.2f, 1f), 2f);
        EditorGui.End();
    }
}
```

- [ ] **Step 2: Verify in-editor.** Launch the editor on INKBOUND (control port 8787). The scripts compile on project load, so the "Demo Window" should appear once the pump runs. Via MCP: `engine_info` to confirm connected; `screenshot` and confirm the window renders with text, a button, an input field, and the two drawn lines. Click "Log me" if reachable (`ui_click`/`send_input`) and `get_console_log` for `[DEMO] EditorGui button clicked`. If the window doesn't appear, check the console for `IEditorWindow` ctor or `OnGui` errors and the `Bootstrap.Init` size assert.

- [ ] **Step 3: Remove the demo.**
```bash
rm D:/AetherCore/projects/INKBOUND/scripts/DemoEditorWindow.cs
```

- [ ] **Step 4: Commit (bridge validated).**
```bash
git add -A && git commit -m "Verify EditorGui bridge end-to-end (demo window removed)"
```

---

## Task 7: Finalize

- [ ] **Step 1: Regression + shipped-build check.**
```bash
cmake --build build/vs2022-msvc --target EngineTests GameRuntime --config Debug
ctest --test-dir build/vs2022-msvc -C Debug
```
Expected: EngineTests 100% pass; GameRuntime builds (no ImGui leak).

- [ ] **Step 2: Revert any stray editor scene/asset migrations** so the branch is only the bridge.

- [ ] **Step 3: Merge + push.** Use **superpowers:finishing-a-development-branch**; fast-forward merge + push to `master`.

Then **Spec 2** (dialogue node-graph editor) builds entirely on this surface as an INKBOUND `IEditorWindow` — no further engine/app changes expected.

---

## Self-review notes

- **Spec coverage:** ABI entrypoint (T1), interface+discovery (T2), interop surface (T3), managed API (T4), pump (T5), validation (T6), gating/regression (T3 Step 3 + T7). All spec sections map to tasks.
- **Boundary:** only generic engine/app/SDK files; the only project file is the *temporary* demo (removed). Real consumer (dialogue editor) is Spec 2.
- **Deliberate verifications (not placeholders):** the exact `LayerContext`→subsystem accessor for the pump (T5 Step 1, checked against `ScriptComponentSystem`); the ABI size-assert passing (T6, at launch). Both have concrete checks.
- **Type consistency:** `IEditorWindow`, `EditorGui`, `DrawEditorWindows`, `s_editorWindows`, `aether_editorgui_*` names are identical across tasks; every managed P/Invoke (T4) maps 1:1 to a T3 export.
- **Ordering caveat:** `IEditorWindow` (T2 Step 1) is referenced by T1 Step 3/4; create the interface file first if the compiler complains (noted in T1 Step 4).
