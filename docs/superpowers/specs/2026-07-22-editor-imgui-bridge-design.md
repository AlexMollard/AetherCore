# Editor ImGui Bridge + Project Windows — Design

**Date:** 2026-07-22
**Status:** Approved (brainstorming complete; ready for implementation plan)

## Goal

Let a project's C# scripts draw their own ImGui windows in the editor, without any project-specific
code living in the engine/app/editor. A project implements a small `IEditorWindow` interface and calls
an `EditorGui.*` immediate-mode API; the editor discovers and pumps those windows. This is the generic
hook (Spec 1). The **dialogue node-graph editor** is its first consumer and gets its own spec (Spec 2).

## Guiding constraints

- **Project boundary (hard rule):** the bridge is *generic* — `src/app` + `managed/AetherCore` only,
  with no knowledge of dialogue or any game. Consumers live entirely in `projects/<game>/`. Same rule
  the CustomPass hook follows. See [[aethercore_project_code_boundary]].
- **Editor/dev-tooling only:** the ImGui surface is a development tool. It must be inert (and must not
  require ImGui) in a shipped `GameRuntime` build.
- **No shortcuts** — proper architecture, matching the existing script-bridge patterns.
- Immediate-mode (chosen): projects draw real ImGui however they like — not a reflection auto-panel or
  a retained widget tree.

## Architecture overview

```
   Editor ImGui pass (DebugLayer)
      │  calls once per frame
      ▼
   DrawEditorWindows()  [UnmanagedCallersOnly managed entrypoint]   ← same wiring as InvokeUpdate
      │  iterates discovered IEditorWindow instances
      ▼
   IEditorWindow.OnGui()  (project C#)   e.g. INKBOUND DialogueGraphEditor (Spec 2)
      │  calls
      ▼
   EditorGui.Begin/Text/Button/AddBezier/...  (managed SDK, generic)
      │  P/Invoke
      ▼
   aether_editorgui_*  (AE_SCRIPT_API, src/app, editor-only)  →  ImGui::*  on the live context
```

Two directions, both already proven by the script lifecycle:
- **C++ → C#**: a new `[UnmanagedCallersOnly] DrawEditorWindows()` in `AetherCore.Interop`, wired to a
  C++-callable function pointer exactly like `InvokeUpdate`, called from the editor's ImGui pass.
- **C# → C++**: `EditorGui.*` P/Invokes into `aether_editorgui_*` exports that call `ImGui::*` on the
  current context (valid because the editor calls `DrawEditorWindows` synchronously inside its own
  ImGui frame, on the main thread).

## 1 · `EditorGui` surface (curated MVP, node-graph-capable)

A deliberately small but graph-sufficient slice of ImGui (not a full wrapper). All in
`managed/AetherCore/EditorGui.cs` (generic SDK) → `src/app/scripting/interop/EditorGuiExports.cpp`.

- **Windows / layout:** `Begin(title, ref bool open) : bool` / `End`, `BeginChild(id, w, h, border) : bool`
  / `EndChild`, `SameLine`, `Separator`, `Spacing`, `GetContentRegionAvail() : Vector2`,
  `GetCursorScreenPos()`/`SetCursorScreenPos(Vector2)`.
- **Text / widgets:** `Text`, `TextColored(Vector4, string)`, `Button(label, size?) : bool`,
  `SmallButton`, `Checkbox(label, ref bool) : bool`, `InputText(label, ref string, maxLen) : bool`,
  `InputFloat(label, ref float) : bool`, `Combo(label, ref int, string[] items) : bool`,
  `Selectable(label, selected) : bool`, `TreeNode(label) : bool` / `TreePop`.
- **Canvas draw list (for the graph):** `AddLine(a, b, color, thickness)`,
  `AddRectFilled(min, max, color, rounding)`, `AddRect(min, max, color, rounding, thickness)`,
  `AddBezierCubic(p1,p2,p3,p4, color, thickness)`, `AddCircleFilled(center, r, color)`,
  `AddText(pos, color, string)`. Colors are `Vector4`; positions are screen-space `Vector2`.
- **Interaction (drag / pan / link):** `InvisibleButton(id, size) : bool`, `IsItemActive() : bool`,
  `IsItemHovered() : bool`, `IsItemClicked() : bool`, `GetMousePos() : Vector2`,
  `GetMouseDragDelta() : Vector2`, `IsMouseDragging() : bool`, `GetMouseWheel() : float`.

String/`ref` marshalling follows the existing interop conventions (UTF-8 in, small stack buffers out,
two-call sizing where needed — as in `Ui.GetText`). `ref bool`/`ref float`/`ref int` pass a pointer the
native side reads+writes so ImGui's in/out semantics survive the boundary; `ref string` uses a fixed
buffer the native `InputText` writes back.

## 2 · `IEditorWindow` discovery + dispatch

- **SDK (generic):** `public interface IEditorWindow { string Title { get; } void OnGui(); }` in
  `managed/AetherCore/IEditorWindow.cs`.
- **Discovery:** in `AetherCore.Interop` (where `ScriptRegistry` already enumerates the project assembly
  for `EntityScript` types), also enumerate for non-abstract `IEditorWindow` implementers, instantiate
  one of each (parameterless ctor), and hold them in a list. Re-runs on script reload, like scripts.
- **Dispatch:** `[UnmanagedCallersOnly] static void DrawEditorWindows()` iterates the list and calls
  `OnGui()` on each (wrapped in try/catch → `Bootstrap.ReportError`, so one bad window can't crash the
  editor). Each window owns its `ImGui::Begin(Title, ...)`/`End` inside `OnGui`.
- **Toggle:** the window's own open-bool (persisted by the project if it likes) governs visibility; a
  single editor-side "Project Windows" enable is the pump's on/off. No per-window editor registration.

## 3 · Editor pump + gating

- **Pump:** a minimal call site in the editor's ImGui pass (`DebugLayer`) invokes the
  `DrawEditorWindows` function pointer once per frame, after the built-in panels. Not a `DebugPanel`
  subclass (project windows self-`Begin`); just a pump call guarded by the editor's dev-tooling flag.
- **Gating:** `EditorGuiExports.cpp` compiles its ImGui calls only under the dev-tooling/editor config
  (`AE_DEV_TOOLING` or an editor-only source set); in a shipped `GameRuntime` the exports are absent or
  no-op and `DrawEditorWindows` is never called, so ImGui is not required there. The managed
  `EditorGui` P/Invokes resolve lazily and are simply never invoked in a shipped build.

## 4 · Build order (Spec 1)

Each phase ends independently verifiable in-editor.

1. **C++ `EditorGui` interop** — the curated surface calling `ImGui::*`; editor-only gating; a new
   interop TU (`cmake -S/-B` reconfigure so the Editor target compiles it).
2. **Managed `EditorGui` API** — P/Invokes + the `ref`/string marshalling; `IEditorWindow` interface.
3. **Discovery + dispatch + pump** — enumerate `IEditorWindow` in `AetherCore.Interop`, add
   `DrawEditorWindows`, wire its function pointer like `InvokeUpdate`, call it from `DebugLayer`.
4. **Validate** — a throwaway demo `IEditorWindow` (a window with text, a button that logs, an
   `InputText`, and two draw-list lines) proves the round trip end-to-end; then delete it.

## Verification

- In-editor via the aethercore MCP: launch the editor, confirm the demo window appears, interact
  (`ui_*`/`send_input` where possible) and `screenshot`; read the console for the button's log line.
- `EngineTests` stays green (only additive interop + a managed entrypoint touch C++).
- Confirm a shipped-style `GameRuntime` build still links/compiles (no ImGui requirement leaked).

## Out of scope (Spec 1)

- The dialogue node-graph editor itself — **Spec 2**, an INKBOUND `IEditorWindow` built on this surface
  (node canvas, bezier links, drag, add/delete nodes+choices, save JSON, live-highlight current node).
- Docking/menu integration, per-window editor persistence, undo — add only if a consumer needs them.
- A comprehensive ImGui wrapper — the surface grows on demand, not speculatively.
