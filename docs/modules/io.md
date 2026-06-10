# `io/` - Virtual file system and async I/O

`io/` provides a virtual file system with pluggable backends and an async coroutine-driven loader.

## Files

| File | Role |
|---|---|
| `FileSystem.hpp` / `FileSystem.cpp` | Virtual file system, mount points, and the default mount set. |
| `IFileBackend.hpp` | Backend interface. |
| `DirectoryBackend.hpp` / `DirectoryBackend.cpp` | Reads directly from a directory. Used for hot iteration. |
| `PakBackend.hpp` / `PakBackend.cpp` | Reads from `.pak` files (zstd-compressed). Used for shipping. |
| `FileRequest.hpp` / `FileRequest.cpp` | The async I/O request type. |
| `FileGlobOptions.hpp` | Glob options for `FileSystem::Glob`. |
| `IOThread.hpp` / `IOThread.cpp` | Coroutine-driven async loader. |

## Virtual paths

The virtual file system mounts prefixes that resolve to backends. Defaults (from `FileSystem::InitializeDefaultMounts`):

- `assets://...` → `assets.pak` (`PakBackend`).
- `shaders://...` → shader sources or compiled SPIR-V.
- (developer-defined) → directory mounts for hot iteration.

Applications can add their own mounts at any time via `FileSystem::Mount(prefix, backend)`.

## Backends

```cpp
class IFileBackend {
public:
    virtual ~IFileBackend() = default;
    virtual std::optional<std::vector<std::byte>> Read(const std::filesystem::path& path) = 0;
    virtual bool Exists(const std::filesystem::path& path) const = 0;
    virtual std::vector<std::filesystem::path> Glob(const std::filesystem::path& pattern, const FileGlobOptions& opts) = 0;
};
```

- `DirectoryBackend` - reads from a real directory.
- `PakBackend` - opens a `.pak` file (xxHashed index, zstd-compressed entries). Created by the asset packer.

## `IOThread`

`src/engine/io/IOThread.hpp`. Coroutine-driven async loader. Uses `aether::coro::async` to overlap disk I/O with game-thread work:

```cpp
aether::coro::Task<std::vector<std::byte>> LoadFile(std::string virtualPath);
```

A worker pool inside `IOThread` resolves requests; results are returned via a `coro::channel` to the awaiting engine-thread coroutine.

The IO thread is the only place `GpuHeap` access happens (for asset staging). The render thread never touches it.

## See also

- [`modules/assets.md`](assets.md) - what consumes the loaded data.
- [`modules/utils.md`](utils.md) - coroutines.
