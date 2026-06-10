# `gpu/` - Engine-facing GPU API

This is the only GPU header set the rest of the engine should include. **`gpu/` exposes zero `Vk*` types.** All Vulkan concerns live in `vulkan/`.

## Files

| File | Role |
|---|---|
| `GpuTypes.hpp` | Engine-facing enums (`GpuFormat`, `GpuExtent2D`, etc.). |
| `GpuDevice.hpp` / `GpuDevice.cpp` | The GPU device façade. Owns instance, device, swapchain, VMA, bindless. |
| `BindlessManager.hpp` / `BindlessManager.cpp` | Bindless descriptor set allocation. |
| `AsyncComputeContext.hpp` / `AsyncComputeContext.cpp` | Optional dedicated compute queue. |

## `GpuDevice`

The entry point. Header: `src/engine/gpu/GpuDevice.hpp:23`.

```cpp
class GpuDevice {
public:
    struct Config {
        const char* appName = "AetherCore";
        bool        enableVsync = true;
    };

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

    [[nodiscard]] Swapchain&     GetSwapchain();
    [[nodiscard]] VulkanContext& GetVulkanContext();
    [[nodiscard]] ResourcePool&  GetResourcePool();
    [[nodiscard]] BindlessManager& GetBindlessManager();

    [[nodiscard]] uint32_t GetComputeQueueFamily() const;
    [[nodiscard]] uint32_t GetGraphicsQueueFamily() const;

    [[nodiscard]] static constexpr GpuFormat GetForwardColorFormat() {
        return GpuFormat::R16G16B16A16Sfloat;
    }
};
```

### Lifetime

- `Init` - creates the Vulkan instance via `vk-bootstrap`, selects queues, creates the device, swapchain, VMA, bindless manager. Registers `VulkanContext` and the swapchain on the service container.
- `Shutdown` - destroys in reverse order. VMA is destroyed **last**.
- `WaitIdle` - flush all GPU work. Call before freeing any GPU resource or before shutdown.

### Frame boundary

```
GpuDevice::BeginSwapchainFrame();
auto cmd = gpu.GetCurrentCommandRecorder();
// ... record passes into cmd ...
gpu.SubmitAndPresent(computeSem, computeValue, rootMotionSem, rootMotionValue);
```

`SubmitAndPresent` accepts timeline semaphore parameters so the graphics queue can wait on async-compute and root-motion compute work.

### Swapchain recreation

`RecreateSwapchain` is called when the window resizes or returns from minimized. It rebuilds the swapchain and clears `SwapchainNeedsRecreation`. Subsystems that own extent-dependent resources register a callback via `SetSwapchainRecreatedCallback` - see `ARCHITECTURE.md §4` for the pattern.

## `BindlessManager`

`src/engine/gpu/BindlessManager.hpp`. Allocates a single large descriptor set for sampled images and another for storage buffers/textures. All materials, textures, and large buffers are addressed by **bindless indices** in shaders - there is essentially no per-draw descriptor work.

`AdvanceBindlessFrame` tells the bindless manager which frame is currently in flight. This enables safety checks (e.g. the manager refuses to reuse a slot whose previous owner is still in flight).

`GetCapacity()` is logged at engine init in `AetherCore.cpp:181`.

## `AsyncComputeContext`

`src/engine/gpu/AsyncComputeContext.hpp:12`. Owns a dedicated compute queue and a small set of pre-allocated command buffers/frame resources. Lifecycle:

- `Init(GpuDevice&)` - only if `GpuDevice::HasDedicatedComputeQueue()` returns true and `EngineSettings.graphics.asyncCompute` is set.
- `BeginFrame()` / `EndCommandBuffer()` / `Submit(semaphore, value)` / `GetCommandRecorder()` - frame-level API.
- `Shutdown(GpuDevice&)` - called from `AetherCore::~AetherCore`.

Use it for: GPU skinning, particle update, large compute dispatches that can overlap with graphics.

## `GpuFormat`

`src/engine/gpu/GpuTypes.hpp`. A flat enum mirroring the formats we use. Notable values:

- `R16G16B16A16Sfloat` - canonical HDR color (returned by `GpuDevice::GetForwardColorFormat`).
- `B8G8R8A8Srgb` / `B8G8R8A8Unorm` - typical swapchain formats.
- `D32Sfloat` / `D24UnormS8Uint` - depth.
- BCn variants for compressed textures.

Engine code uses `GpuFormat` only. Conversion to `VkFormat` happens inside the `vulkan/` module.

## Threading

- `GpuDevice` is touched on the engine thread for state queries and on the render thread for `BeginSwapchainFrame` / `SubmitAndPresent` / `GetCurrentCommandRecorder`. The methods are **not** internally synchronized - callers must serialize.
- `BindlessManager` is touched on the engine thread when slots are allocated and on the render thread when descriptors are bound. Same caveat.
- `AsyncComputeContext` is render-thread-only.
