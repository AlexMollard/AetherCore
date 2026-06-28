# `engine/` - AetherCore orchestrator

The `engine/` module is the composition root. `AetherCore` (`src/engine/AetherCore.hpp:23`) owns every subsystem and the frame lifecycle.

## Files

| File | Role |
|---|---|
| `AetherCore.hpp` / `AetherCore.cpp` | Engine class, frame lifecycle, subsystem init/shutdown. |
| `ServiceContainer.hpp` | Type-erased service locator (lives under `utils/` but is core to the engine's design). |

## The `AetherCore` class

```cpp
class AetherCore {
public:
    struct Config {
        const char* appName          = "AetherCore";
        int         width            = 1280;
        int         height           = 720;
        bool        enableVsync      = true;
        const char* settingsFile     = "engine.toml";
    };

    explicit AetherCore(const Config& config);
    AetherCore(const Config& config, const EngineSettings& settings);
    ~AetherCore();

    [[nodiscard]] ServiceContainer& GetServiceContainer();

    [[nodiscard]] bool ShouldClose();
    void PumpEvents();
    void Tick(float dt);
    [[nodiscard]] RenderFramePacket PrepareFrame(uint32_t drawSlot, uint64_t frameIndex);
    void ExecuteRenderFrame(const RenderFramePacket& packet);
    void WaitIdle();

    [[nodiscard]] static GpuFormat GetForwardColorFormat();
    [[nodiscard]] std::vector<DebugVertex>& GetPendingDebugVertices();
};
```

## Subsystem init order

See `ARCHITECTURE.md §1` for the full dependency-driven order. The constructor at `src/engine/AetherCore.cpp:45` performs the following phases:

1. **Platform** - `PlatformSubsystem::Init`. Registers `Window` and `Input`.
2. **Graphics device** - `GpuDevice::Init`. Creates Vulkan instance/device/swapchain/VMA/bindless manager.
3. **Scene** - `SceneSubsystem::Init`. Registers `World` and `Scene`.
4. **Assets** - `AssetSubsystem::Init`. Registers `AssetManager`, `MeshArena`, `MeshUploadQueue`, `MaterialBuffer`.
5. **Cameras** - `CameraSubsystem::Init`. Registers `CameraManager`, `LightingManager`.
6. **Rendering** - `RenderingSubsystem::Init`. Registers `Renderer`, `RenderQueue`, `RenderGraph`, `ShadowService`, `RenderTargetService`.
7. **ImGui** - `ImguiSubsystem::Init`. Registers the debug/tooling UI integration.
8. **Async compute** - `AsyncComputeContext::Init` (skipped if no dedicated compute queue).
9. **Animation systems** - `AnimationBlendSystem` and `AnimationRootMotionSystem` constructed and registered.
10. **Cross-subsystem wiring** - `LightingManager.LinkRenderer`, `AssetSubsystem.LinkRenderingDeps`, animation systems wired into `RenderQueue`.
11. **Default main camera** - `CameraManager::Create` + `SetMainCamera`.
12. **Swapchain callback** - `GpuDevice::SetSwapchainRecreatedCallback` registered.

The destructor at `src/engine/AetherCore.cpp:184` reverses this order, prefixed by `m_gpu->WaitIdle()`.

## Frame lifecycle

```
while (!engine.ShouldClose()) {
    engine.PumpEvents();
    engine.Tick(dt);

    auto packet = engine.PrepareFrame(slot, frameIndex);
    engine.ExecuteRenderFrame(packet);

    ++frameIndex;
}
```

- `Tick` - game-thread simulation. Updates input, cameras, ECS, animation systems.
- `PrepareFrame` - builds the per-frame `RenderFramePacket` snapshot.
- `ExecuteRenderFrame` - submits the packet to `RenderThread` (or executes inline if no render thread).

## `ServiceContainer` (`src/engine/utils/ServiceContainer.hpp`)

```cpp
template<typename T> void Register(T& service);
template<typename T> void RegisterOwned(std::unique_ptr<T> service);
template<typename T> T& Get() const;       // asserts if not registered
template<typename T> T* TryGet() const;    // nullptr if not registered
template<typename T> bool Has() const;
template<typename T> void Unregister();
```

Thread safety: **NOT thread-safe.** All registrations happen on the engine thread during init. Reads after init are safe as long as no concurrent modification occurs.

`RegisterOwned` keeps shared ownership inside the container; the service is destroyed when the container is cleared. This is the recommended way to register subsystems - `AetherCore` does it for every one of them.

## Threads of execution

| Thread | Started by | Work |
|---|---|---|
| Engine thread | user (`Application::Run`) | `Tick`, `PrepareFrame`, `ExecuteRenderFrame` |
| Render thread | `RenderThread::Start` | Consumes `RenderFramePacket`, drives `RenderGraph` |
| IO thread | `IOThread::Start` | Coroutine-driven async I/O |
| Animation compile pool | `AssetSubsystem::GetUploadPool` | `AnimationCompiler` work |

The engine thread is the only one that touches the ECS. The render thread operates exclusively on the `RenderFramePacket` snapshot.

## Cross-cutting helpers

`GetPendingDebugVertices()` is a per-frame immediate-mode debug vertex buffer. Layers append to it during `OnUpdate`; `PrepareFrame` moves the contents into the outgoing packet. The `$PhysicsDebug` render pass consumes them on the render thread. Lock-free: the channel transfer of the packet is the synchronization point.

`GetForwardColorFormat()` returns the canonical forward color format (`R16G16B16A16Sfloat`). Use it consistently for HDR pipeline targets.
