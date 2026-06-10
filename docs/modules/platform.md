# `platform/` - Window, input, crash handling

`platform/` is the OS-facing layer. It owns the application window, input state, and optional crash dump capture.

## Files

| File | Role |
|---|---|
| `PlatformSubsystem.hpp` / `PlatformSubsystem.cpp` | Subsystem façade. Owns `Window` and `Input`. Registers them. |
| `Window.hpp` / `Window.cpp` | GLFW-backed window. |
| `Input.hpp` / `Input.cpp` | Per-frame input state (keyboard, mouse, gamepad). |
| `CrashHandler.hpp` / `CrashHandler.cpp` | Optional NVIDIA Nsight Aftermath integration for GPU crash dumps. |

## `Window`

`src/engine/platform/Window.hpp`. GLFW-backed. Public surface:

```cpp
class Window {
public:
    void Init(const WindowDesc&);
    void Shutdown();
    void PollEvents();
    [[nodiscard]] bool ShouldClose() const;
    [[nodiscard]] Extent2D GetFramebufferSize() const;
    [[nodiscard]] Extent2D WaitForValidFramebufferSize(); // blocks until non-zero
    // ... GLFW accessors (raw handle, vulkan surface) ...
};
```

`WaitForValidFramebufferSize` is called by `GpuDevice::RecreateSwapchain` to block until the window is non-minimized before recreating the swapchain.

## `Input`

`src/engine/platform/Input.hpp`. Per-frame state for keyboard, mouse, and gamepad. Updated once per `Tick`:

```cpp
class Input {
public:
    void Update();
    [[nodiscard]] bool IsKeyDown(KeyCode) const;
    [[nodiscard]] bool WasKeyPressed(KeyCode) const;     // edge-triggered
    [[nodiscard]] glm::vec2 GetMousePos() const;
    [[nodiscard]] glm::vec2 GetMouseDelta() const;
    [[nodiscard]] float GetMouseScroll() const;
    // ... gamepad accessors ...
};
```

`CameraManager::Update(Input&, dt)` consumes the input state and applies it to the controllable main camera.

## `PlatformSubsystem`

`src/engine/platform/PlatformSubsystem.hpp`. The subsystem façade. Owns `Window` and `Input`. Registered in step 1 of `AetherCore` init. Exposes:

- `GetWindow()`
- `GetInput()`

Both are registered individually on the service container, so subsystems can `Get<Window>()` / `Get<Input>()` directly.

## `CrashHandler`

Optional NVIDIA Nsight Aftermath integration (`src/engine/platform/CrashHandler.hpp`). On a GPU crash (device lost, page fault, etc.), Aftermath can capture a full GPU state dump for post-mortem analysis. The handler is opt-in and currently targets NVIDIA GPUs.

## See also

- [`modules/engine.md`](engine.md) - subsystem init order.
- [`modules/gpu.md`](gpu.md) - how `Window` provides the Vulkan surface.
