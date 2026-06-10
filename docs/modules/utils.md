# `utils/` - Cross-cutting utilities

`utils/` is the engine's "standard library" - primitives used by every other module.

## Files

| File | Role |
|---|---|
| `Logger.hpp` / `Logger.cpp` | The `AE_INFO` / `AE_WARN` / `AE_ERROR` macros. |
| `LogCategory.hpp` | The `LogCategory` enum and category traits. |
| `Assert.hpp` | `AE_ASSERT` / `AE_ASSERT_ALWAYS` / `AE_TRY` macros. |
| `AetherExceptions.hpp` | Typed exceptions thrown by `AE_EXPECT_OR_THROW`. |
| `Expected.hpp` | `std::expected` polyfill. |
| `ServiceContainer.hpp` | Type-erased service locator. |
| `EngineSettings.hpp` / `EngineSettings.cpp` | The settings schema and TOML loader. |
| `FramePacer.hpp` | Coarse-sleep + fine-spin frame rate regulation. |
| `LoadingManager.hpp` / `LoadingManager.cpp` | Async load progress tracking. |
| `Profiler.hpp` | Tracy integration. |
| `GpuProfiler.hpp` | GPU-side profiling (timestamp queries). |
| `BinaryReader.hpp` | `std::istream`-like reader over a `std::span<std::byte>`. |
| `StringUtils.hpp` | String helpers. |
| `TextIni.hpp` / `TextIni.cpp` | Tiny INI parser. |
| `MemoryTracker.cpp` | Debug allocator. |
| `coro/` | Coroutines. See below. |

## `coro/` - coroutines

| File | Role |
|---|---|
| `Task.hpp` | `Task<T>` (return type) and `async<T>` (factory). |
| `Channel.hpp` | Bounded MPMC channel. `RenderThread` uses capacity 2. |
| `Executor.hpp` / `Executor.cpp` | `Executor`, `inline_executor`, `queued_executor`. |
| `Sleep.hpp` | Coroutine-friendly sleep. |

```cpp
aether::coro::Task<int> ComputeAsync() {
    co_await aether::coro::Sleep(100ms);
    co_return 42;
}

auto task = ComputeAsync();
int value = co_await task;  // resolves to 42
```

## `Logger` and `LogCategory`

```cpp
AE_INFO(LogCategory::Engine, "Engine initialized with capacity {}", capacity);
AE_WARN(LogCategory::Asset, "Texture not found, using fallback");
AE_ERROR(LogCategory::Vulkan, "vkCreateImageView failed: {}", error.what());
```

Categories live in `LogCategory.hpp`. To add a new one:

1. Add the enumerator to `enum class LogCategory`.
2. Add a matching `LogCategoryTraits` specialization (name, color).

## `Assert` and `Expected`

```cpp
AE_TRY(auto buffer, UniqueBuffer::Create(desc));
AE_ASSERT(frameIndex < kMaxFramesInFlight, "Frame index out of range");
AE_ASSERT_ALWAYS(m_gpu != nullptr, "GpuDevice must be initialized");
```

`AE_TRY` unwraps `Expected<T>` and returns on error. `AE_ASSERT` is Debug-only; `AE_ASSERT_ALWAYS` ships in Release and aborts.

## `EngineSettings`

`src/engine/utils/EngineSettings.hpp`. The schema. Loaded from TOML at `AetherCore::AetherCore` construction:

```cpp
AetherCore::AetherCore({.appName = "MyGame", .settingsFile = "mygame.toml"});
```

Fields include:

```toml
[window]
width  = 1280
height = 720

[graphics]
vsync        = true
asyncCompute = true

[engine]
# ... logging, profiling, etc. ...
```

The default file is created on first run if it doesn't exist.

## `FramePacer`

`src/engine/utils/FramePacer.hpp`. Regulates the engine-thread cadence to a target FPS using a coarse sleep + fine spin combination. Used by `Application::Run` to clamp frame rate without busy-looping.

## `LoadingManager`

`src/engine/utils/LoadingManager.hpp`. Tracks async load progress. `LoadingLayer` (in `src/app/layers/`) reads from it to render a full-screen overlay with a progress bar.

## `Profiler` / `GpuProfiler`

`src/engine/utils/Profiler.hpp` and `GpuProfiler.hpp`. Tracy integration. Use the `AE_PROFILE_ZONE()` macro at the top of a function to record it in the Tracy timeline. `GpuProfiler` issues timestamp queries per pass and feeds them back into Tracy.

## See also

- [`ARCHITECTURE.md §12`](../ARCHITECTURE.md) - error handling philosophy.
- [`ARCHITECTURE.md §13`](../ARCHITECTURE.md) - logging.
- [`modules/io.md`](io.md) - coroutine async I/O.
- [`modules/rendering.md`](rendering.md) - `RenderThread` channel.
