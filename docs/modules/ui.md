# `ui/` - In-engine immediate-mode UI

AetherCore has a self-contained immediate-mode UI system, similar in spirit to Dear ImGui but engine-integrated. UI commands are recorded on the engine thread and rendered by dedicated passes in the render graph.

## Files

| File | Role |
|---|---|
| `UISubsystem.hpp` / `UISubsystem.cpp` | Subsystem façade. Owns `UIRenderer`, `UiContext`, `UiSystem`. Registers them on the service container. |
| `UiSystem.hpp` / `UiSystem.cpp` | Per-frame state machine (`Begin` / `Window` / `End`). |
| `UiContext.hpp` | The user-facing draw API: `Button`, `Slider`, `Text`, etc. |
| `UiWidgets.hpp` / `UiWidgets.cpp` | Reusable widgets built on top of `UiContext`. |
| `UiLayout.hpp` | Flex/anchor layout engine. |
| `UiTheme.hpp` | Theme tokens (colors, sizes, fonts). |
| `UiComponents.hpp` | Internal UI ECS components. |
| `UIRenderer.hpp` / `UIRenderer.cpp` | Translates UI commands into render graph passes. |
| `QuadRenderer.hpp` / `QuadRenderer.cpp` | Quad primitive renderer used by the UI. |

## Subsystem init

UI is opt-in - `UISubsystem` is only initialized if `AetherCore::Config::uiFontPath` is non-empty (`src/engine/AetherCore.cpp:111`). When enabled, it registers:

- `UIRenderer`
- `ui::UiContext`
- `ui::UiSystem`

The UI subsystem also re-registers its render graph passes on swapchain recreation through the `GpuDevice` callback installed in `AetherCore.cpp:168`.

## Per-frame flow

```
UISubsystem::BeginFrame();   // starts the per-frame UI state
// ... game code calls ui::Context::Button / Slider / Text ...
UISubsystem::EndFrame();     // closes the frame, defers rendering
```

On the render thread, `UIRenderer` consumes the recorded commands and emits render graph passes named with the configured `uiPassNamePrefix` (default `"UIPass"`).

## Custom themes

`UiTheme.hpp` is the theming layer. To skin the UI, define a `UiTheme` and pass it to the subsystem before `Init`. Themes provide:

- Color tokens (background, foreground, accent, danger, etc.).
- Spacing tokens.
- Font references.

## Adding a new widget

1. Add a public method to `ui::UiContext` (`src/engine/ui/UiContext.hpp`).
2. Implement it in `UiWidgets.cpp` (or in a new file under `ui/`).
3. If it needs new primitive rendering, extend `UIRenderer` and add a render pass.
4. Document it here.

## See also

- [`modules/text.md`](text.md) - font atlas used by the UI.
- [`modules/rendering.md`](rendering.md) - how UI passes fit the render graph.
