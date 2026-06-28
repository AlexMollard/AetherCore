# API Reference

This is an index of AetherCore's public types and their responsibilities. For deep dives, see the per-module docs under [`modules/`](../modules/) and the linked `file:line` references.

> The engine compiles as C++26 on Clang and C++23 on MSVC. Some types use C++26 features (`std::expected`, contracts in spirit). Most signatures are stable; the engine follows semver-style API stability for the major surface.

---

## `aether::AetherCore` - engine orchestrator

Defined in `src/engine/AetherCore.hpp:23`. The composition root and frame lifecycle driver.

```cpp
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
```

See [`modules/engine.md`](../modules/engine.md).

## `aether::ServiceContainer` - type-erased service locator

Defined in `src/engine/utils/ServiceContainer.hpp:21`.

```cpp
template<typename T> void Register(T& service);
template<typename T> void RegisterOwned(std::unique_ptr<T> service);
template<typename T> T& Get() const;          // asserts if not registered
template<typename T> T* TryGet() const;
template<typename T> bool Has() const;
template<typename T> void Unregister();
void Clear();
```

NOT thread-safe. All `Register` / `Unregister` calls must be on the engine thread.

## `aether::GpuDevice` - GPU device façade

Defined in `src/engine/gpu/GpuDevice.hpp:23`.

```cpp
struct Config { const char* appName = "AetherCore"; bool enableVsync = true; };

void Init(ServiceContainer& services, const Config& config);
void Shutdown();
void WaitIdle();

[[nodiscard]] bool HasDedicatedComputeQueue() const;
[[nodiscard]] GpuFormat GetSwapchainColorFormat() const;
[[nodiscard]] GpuFormat GetSwapchainDepthFormat() const;
[[nodiscard]] GpuExtent2D GetSwapchainExtent() const;
[[nodiscard]] bool SwapchainNeedsRecreation() const;
void ClearSwapchainRecreationFlag();
[[nodiscard]] bool IsSwapchainFrameValid() const;

void BeginSwapchainFrame();
void RecreateSwapchain(Window& window, bool enableVsync);
void SubmitAndPresent(uint64_t asyncComputeSemaphoreHandle = 0,
                      uint64_t asyncComputeTimelineValue  = 0,
                      uint64_t rootMotionSignalSemaphore  = 0,
                      uint64_t rootMotionSignalValue      = 0);

[[nodiscard]] CommandRecorder GetCurrentCommandRecorder() const;
[[nodiscard]] FrameTarget BuildFrameTarget() const;

void SetSwapchainRecreatedCallback(std::function<void()> cb);
void AdvanceBindlessFrame(uint64_t frameIndex);

[[nodiscard]] Swapchain&       GetSwapchain();
[[nodiscard]] VulkanContext&   GetVulkanContext();
[[nodiscard]] ResourcePool&    GetResourcePool();
[[nodiscard]] BindlessManager& GetBindlessManager();

[[nodiscard]] uint32_t GetComputeQueueFamily() const;
[[nodiscard]] uint32_t GetGraphicsQueueFamily() const;

[[nodiscard]] static constexpr GpuFormat GetForwardColorFormat();
```

See [`modules/gpu.md`](../modules/gpu.md).

## `aether::RenderFramePacket` - per-frame snapshot

Defined in `src/engine/rendering/RenderFramePacket.hpp:16`. The value-typed packet passed engine thread → render thread. Holds camera matrices, lighting state, point/spot light lists, debug vertices, and the material buffer BDA.

See [`modules/rendering.md`](../modules/rendering.md).

## `aether::RenderThread` - dedicated render thread

Defined in `src/engine/rendering/RenderThread.hpp:25`. Double-buffered `coro::channel<RenderFramePacket>` of capacity 2.

```cpp
void Start(AetherCore& engine);
void Stop();
void SubmitFrame(RenderFramePacket packet);   // non-blocking
void WaitIdle();
static void SetReloadInProgress(bool inProgress);
static bool IsReloadInProgress();
```

See [`modules/rendering.md`](../modules/rendering.md).

## `aether::RenderQueue` - draw command queue

Defined in `src/engine/rendering/RenderQueue.hpp:80`. Collects `DrawCommand` entries on the engine thread, sorts them, and exposes them to `ForwardPass`. Wired to the animation systems.

## `aether::RenderGraph` - DAG of passes

Defined in `src/engine/rendering/RenderGraph.hpp`. Holds `RenderPass` nodes with declared inputs/outputs. The graph compiler handles ordering and resource aliasing.

## `aether::Renderer` - per-frame state aggregator

Defined in `src/engine/rendering/Renderer.hpp`. Updated by `CameraSubsystem` and `LightingManager`. Owns:

- Camera, sun, ambient, sky state.
- `std::vector<PointLight>` and `std::vector<SpotLight>`.

## `aether::CameraManager`

Defined in `src/engine/camera/CameraManager.hpp`.

```cpp
[[nodiscard]] CameraHandle Create(const CameraDesc&);
void Destroy(CameraHandle);
[[nodiscard]] Camera& Get(CameraHandle);
void SetMainCamera(CameraHandle);
[[nodiscard]] Camera& GetMainCamera();
void Update(Input&, float dt);
```

## `aether::LightingManager`

Defined in `src/engine/camera/LightingManager.hpp`.

```cpp
void SetSun(const glm::vec4& directionIntensity, const glm::vec4& color);
void SetAmbient(const glm::vec4& color);
void SetSky(const glm::vec4& horizon, const glm::vec4& zenith, const glm::vec4& skyVoid);
void AddPointLight(const Renderer::PointLight&);
void AddSpotLight(const Renderer::SpotLight&);
void ClearLights();
void FlushTo(Renderer&);
void LinkRenderer(Renderer&);
```

## `aether::SceneSubsystem` / `aether::World` / `aether::Scene`

Defined in `src/engine/scene/`. The scene module wraps EnTT.

## `aether::AssetManager` / `aether::AssetSubsystem` / `aether::MeshArena` / `aether::MaterialBuffer`

Defined in `src/engine/assets/`. Asset loading and GPU staging.

## `aether::BindlessManager` - bindless descriptor sets

Defined in `src/engine/gpu/BindlessManager.hpp`. Manages the global sampled-image and storage-buffer descriptor sets.

## `aether::AsyncComputeContext` - optional compute queue

Defined in `src/engine/gpu/AsyncComputeContext.hpp:12`.

```cpp
void Init(GpuDevice&);
void Shutdown(GpuDevice&);
void BeginFrame();
void EndCommandBuffer();
void Submit(uint64_t semaphoreHandle, uint64_t value);
[[nodiscard]] CommandRecorder GetCommandRecorder() const;
```

## `aether::UniqueBuffer` / `aether::UniqueImage`

Defined in `src/engine/vulkan/`. Move-only RAII wrappers.

```cpp
static Expected<UniqueBuffer> Create(const BufferDesc&);
static Expected<UniqueImage>  Create(const ImageDesc&);
```

## `aether::GpuHeap` / `aether::GpuSpan<T>`

Defined in `src/engine/vulkan/`. Device-local memory arena.

```cpp
template<typename T>
[[nodiscard]] GpuSpan<T> Alloc(size_t count);
void Free(const GpuSpan<T>&);
```

**Not thread-safe** - only call from the asset loading thread.

## `aether::Window` / `aether::Input`

Defined in `src/engine/platform/`. GLFW-backed window and per-frame input state.

## `aether::Application` - main loop

Defined in `src/app/Application.hpp`.

```cpp
int Run(AetherCore& engine);
void PushLayer(std::unique_ptr<AppLayer>);
void PopLayer(AppLayer&);
```

## `aether::AppLayer` - base layer

Defined in `src/app/layers/AppLayer.hpp`.

```cpp
virtual void OnAttach(AetherCore&, ServiceContainer&) = 0;
virtual void OnUpdate(AetherCore&, ServiceContainer&, float dt) = 0;
virtual void OnImGui(AetherCore&, ServiceContainer&) {}
virtual void OnDetach() = 0;
virtual std::string_view Name() const = 0;
```

## `aether::ImguiSubsystem`

Defined in `src/engine/imgui/`. Owns the Dear ImGui context and platform/render backends used by debug/tooling UI.

## `aether::coro::*` - coroutines

Defined in `src/engine/utils/coro/`.

- `Task<T>` / `async<T>` - coroutine return type.
- `Channel<T>` - bounded MPMC queue.
- `Executor` / `inline_executor` / `queued_executor` - schedulers.
- `Sleep(duration)` - coroutine-friendly sleep.

## Macros

```cpp
AE_INFO(category, fmt, ...)    // log at info level
AE_WARN(category, fmt, ...)    // log at warn level
AE_ERROR(category, fmt, ...)   // log at error level

AE_ASSERT(expr, msg)           // Debug-only check
AE_ASSERT_ALWAYS(expr, msg)    // Release-grade assertion

AE_TRY(var, expr)              // unwrap Expected, return on error
AE_EXPECT_OR_THROW(var, expr)  // unwrap or throw typed exception
AE_UNEXPECTED(err)             // return std::unexpected(err)

AE_PROFILE_ZONE()              // Tracy profiler zone
```

## See also

- The full module docs under [`modules/`](../modules/).
- `AGENTS.md` (repo root) for naming and formatting conventions.
