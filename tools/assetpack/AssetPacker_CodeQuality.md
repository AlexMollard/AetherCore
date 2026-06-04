# AssetPacker — Code Quality Audit

Separate from the bug/correctness audit. This covers structure, type choices, C++ idioms,
and anything where the code works today but would slow you down or cause problems as the
project grows.

---

## Table of Contents

1. [File & Module Structure](#1-file--module-structure)
2. [Type Choices](#2-type-choices)
3. [C++ Idiom Issues](#3-c-idiom-issues)
4. [Function & Class Design](#4-function--class-design)
5. [Include Strategy](#5-include-strategy)
6. [Naming](#6-naming)
7. [Minor / One-liners](#7-minor--one-liners)

---

## 1. File & Module Structure

### 1.1 `PakWriter.cpp` is doing too many jobs — split it

The file currently contains:
- Manifest load/save logic
- Build log formatting
- Asset type dispatch (`ProcessAsset`)
- Per-file read/compress task (`ProcessFile`)
- The `PakWriter` class implementation

That is five distinct responsibilities in one file. As the pipeline grows (new asset types,
more complex manifest logic), this becomes a maintenance problem. Suggested split:

```
PakWriter.cpp         — PakWriter::AddDirectory, PakWriter::Write only
AssetProcessor.cpp/hpp — ProcessAsset dispatch, ProcessedFile, FileResult
PakManifest.cpp/hpp   — ManifestEntry, ManifestMap, LoadManifest, SaveManifest, IsUpToDate
PakLog.cpp/hpp        — LogEntry, SaveLog, FormatSize, FormatDuration, FormatTimestamp
```

`AssetProcessor.hpp` then becomes the extension point — adding a new asset type means
touching one file, not scrolling through 700 lines.

---

### 1.2 `MaterialImporter` should be a namespace, not a class

The entire class is static methods with no instance state, no virtual dispatch, and no
inheritance. A class buys nothing here. Every call site writes `MaterialImporter::Foo()`
either way. Converting to a namespace removes the misleading implication that this is
ever instantiated:

```cpp
// Before
class MaterialImporter {
public:
    static int ImportDirectory(const fs::path&);
private:
    static bool GeneratePropertiesForFolder(const fs::path&);
    // ...
};

// After
namespace MaterialImporter {
    int  ImportDirectory(const fs::path&);
    // private helpers become anonymous-namespace free functions in the .cpp
}
```

The private helpers (`ToLowerAscii`, `NormalizeForMatch`, `TokenizeStem`,
`IsImageExtension`, `ContainsAlias`, `FindBestTexture`) all move into the anonymous
namespace in `MaterialImporter.cpp`. They're never needed from outside.

---

### 1.3 String/formatting utilities don't belong in `PakWriter.cpp`

`FormatSize`, `FormatDuration`, `FormatTimestamp` are generic. They'll be useful the
moment you add any other tool (a pak inspector, a diff tool, a shader compiler). Put
them in `PipelineUtils.hpp` alongside other candidates:

```cpp
// PipelineUtils.hpp
namespace PipelineUtils
{
    std::string FormatSize(uint64_t bytes);
    std::string FormatDuration(double seconds);
    std::string FormatTimestamp();
    std::string ToLowerAscii(std::string s);
    std::string ToLowerExt(const fs::path& path); // combines extension() + tolower
}
```

`ToLowerAscii` currently exists in `MaterialImporter.cpp` (private) and is essentially
reimplemented inline in multiple other places (`ChooseFormat`, `IsAlreadyCompressed`,
`ProcessAsset`). There are at least four separate lowercase-extension idioms in the
codebase right now.

---

### 1.4 `MeshProcessor::Process` is ~400 lines and should be decomposed

The `Process` function handles everything: parsing, bone collection, bounds computation,
`.skel` serialization, `.mesh` serialization, and `.anim`/`.animset` serialization. Each
of those is a self-contained operation. Suggested breakdown into internal free functions
(staying inside the anonymous namespace):

```cpp
// Internal helpers in anonymous namespace
std::vector<std::byte> WriteSkelFile(const std::vector<BoneInfo>& bones, const std::vector<uint32_t>& remap, const std::string& name);
std::vector<std::byte> WriteMeshFile(cgltf_data* data, const std::vector<BoneInfo>& bones, const std::vector<uint32_t>& remap, ...);
std::vector<std::byte> WriteAnimsetFile(const std::vector<std::string>& animPaths, uint64_t skelHash);
AnimFileResult         WriteAnimFile(const cgltf_animation& anim, const std::vector<uint32_t>& remap, ...);
```

`Process` then becomes a coordinator of ~40 lines that calls these and assembles the
`ProcessedResult`. Each sub-function is independently testable.

---

### 1.5 DDS format definitions should be a shared header

`TextureProcessor.cpp` defines `DDSHeader`, `DDSHeaderDXT10`, `DDSPixelFormat`, all the
`DDSD_*`/`DXGI_*` constants, and the `DDS_MAGIC`. If you ever write a DDS loader for the
editor, a pak inspector, or a thumbnail tool, you'll re-define or copy these. They belong
in a `DDSFormat.hpp` in the shared `include/` directory alongside `PakFormat.hpp` and
`BinaryFormats.hpp`.

---

## 2. Type Choices

### 2.1 Inconsistent byte buffer type — pick `std::vector<std::byte>` everywhere

The codebase uses three different types for binary buffers:
- `std::vector<std::byte>` — MeshProcessor outputs, TextureProcessor, SpirvProcessor
- `std::vector<char>` — `pathData` in PakWriter.cpp
- `uint8_t*` / raw pointer — GatherBlock output, stb_image pixels

Pick `std::vector<std::byte>` as the canonical buffer type for all owned binary data.
The only exception is interfaces that require `char*` (file I/O), where you cast at the
boundary. This also makes `reinterpret_cast` sites explicit and findable.

While you're at it, add a type alias to reduce verbosity:
```cpp
// PipelineUtils.hpp or a dedicated ByteBuffer.hpp
using ByteBuffer = std::vector<std::byte>;
```

---

### 2.2 `std::vector<char> pathData` in PakWriter.cpp

`pathData` is a binary section containing null-terminated UTF-8 strings. Using `char`
here is semantically off — `char` implies text, `std::byte` implies binary data. The
`out.write(pathData.data(), ...)` call just needs a `reinterpret_cast` at the boundary
either way. Use `std::vector<std::byte>` and a `PushString` helper:

```cpp
void PushString(ByteBuffer& buf, std::string_view s)
{
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    buf.insert(buf.end(), p, p + s.size());
    buf.push_back(std::byte{0}); // null terminator
}
```

---

### 2.3 Use `std::span` for buffer view parameters

Many functions take `(const void* data, size_t n)` or `(const std::vector<std::byte>& buf)`.
C++20 `std::span<const std::byte>` is the right type for a non-owning view of a binary
buffer — it carries both pointer and size, works with vectors, arrays, and raw pointers,
and makes the non-owning intent explicit:

```cpp
// Before
std::vector<std::byte> Strip(const std::vector<std::byte>& spv);
std::vector<std::byte> ToDDS(const std::vector<std::byte>& imageData, const fs::path& sourcePath);

// After
std::vector<std::byte> Strip(std::span<const std::byte> spv);
std::vector<std::byte> ToDDS(std::span<const std::byte> imageData, const fs::path& sourcePath);
```

This removes the forced `std::vector` at call sites — a future caller that has a
memory-mapped file or a sub-range of a larger buffer doesn't need to copy into a vector.
The output stays `std::vector<std::byte>` (owned result).

---

### 2.4 `float[3]`/`float[4]` raw arrays throughout — consider a minimal vec type

`BoneInfo`, `TempVertex`, `Bounds`, and every DiskXxx struct use raw `float` arrays for
vectors and matrices. This is fine for the disk structs (they're layout-controlled).
For the in-memory processing types (`TempVertex`, `BoneInfo`, `Bounds`) it leads to
verbose, error-prone code everywhere (`v.position[0]`, `v.normal[1]`, etc.) and manual
loops for dot products, cross products, and normalization.

You don't need a full math library. A minimal `Vec3`/`Vec4` with operator overloads and
free functions (`dot`, `cross`, `normalize`, `length`) as a single-header internal type
cleans up `GenerateNormals`, `ComputeBounds`, and the future tangent generation
significantly. Keep the disk structs as raw arrays — they need to be layout-stable.

---

### 2.5 `cgltf_size` vs `uint32_t` vs `std::size_t` — pick a loop type

Inside `MeshProcessor.cpp`, loop indices over cgltf arrays alternate between
`cgltf_size` (which is `size_t`), `uint32_t`, and plain `int`. Several places then
cast between them:

```cpp
for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)  // cgltf_size
for (uint32_t v = 0; v < vertCount; ++v)               // uint32_t
for (int j = 0; j < 4; ++j)                           // int
```

Standardize: use `cgltf_size` (i.e. `size_t`) when iterating over cgltf arrays,
`uint32_t` when iterating over your own mesh data (counts are stored as `uint32_t` in
the disk format), and `int` nowhere unless you need negative sentinel values.
The casts disappear and the compiler stops warning.

---

### 2.6 `ManifestEntry::mtimeTicks` type

`mtimeTicks` is `int64_t`, which holds `file_time_type::duration::rep`. The actual
type of `time_since_epoch().count()` is implementation-defined (it's `long long` on
MSVC and GCC, which is 64-bit, but it's not guaranteed). A safer approach is to store
it as the duration count of a known tick rate, or use `std::filesystem::file_time_type`
directly and serialize it via `duration_cast<std::chrono::seconds>`. At minimum document
the assumption in the struct.

---

### 2.7 `GatherBlock` output parameter — use a reference-to-array

```cpp
// Before
void GatherBlock(const uint8_t* pixels, int width, int height, int channels,
                 int blockX, int blockY, uint8_t out[4 * 4 * 4])

// After — decay-to-pointer is prevented, size is self-documenting
void GatherBlock(const uint8_t* pixels, int width, int height, int channels,
                 int blockX, int blockY, uint8_t (&out)[64])
```

Or wrap it:
```cpp
using Block4x4 = std::array<uint8_t, 64>;
void GatherBlock(const uint8_t* pixels, int width, int height, int channels,
                 int blockX, int blockY, Block4x4& out);
```

---

## 3. C++ Idiom Issues

### 3.1 `static` functions inside an anonymous namespace — pick one

In `MeshProcessor.cpp`, several functions are marked `static` inside the anonymous
namespace:

```cpp
namespace {
    static int32_t ToIndex(...) { ... }  // static is redundant here
    static std::string SafeStr(...) { ... }
    static uint32_t PackColorRGBA8(...) { ... }
}
```

Anonymous namespace already provides internal linkage. `static` inside it is
redundant and slightly misleading (it implies something about storage duration rather
than linkage). Remove the `static` keyword on all functions in anonymous namespaces.

---

### 3.2 `stbi_load` result is not RAII-managed — memory leaks on exception

```cpp
uint8_t* pixels = stbi_load_from_memory(...);
if (!pixels) return {};
// ... if anything between here and stbi_image_free throws, leak
stbi_image_free(pixels);
```

Wrap it:
```cpp
struct StbiDeleter {
    void operator()(void* p) const { stbi_image_free(p); }
};
using StbiImage = std::unique_ptr<uint8_t, StbiDeleter>;

StbiImage pixels(stbi_load_from_memory(...));
if (!pixels) return {};
// pixels freed automatically
```

`CompressBlocks` and `BuildDDS` can then take `const uint8_t*` raw (they don't own it).

---

### 3.3 `std::stoi` in `main.cpp` can throw — unhandled exception

```cpp
compressionLevel = std::stoi(argv[argOffset + 1]);
```

`std::stoi` throws `std::invalid_argument` on non-integer input and `std::out_of_range`
on overflow. Either wrap it or use `std::from_chars` which doesn't throw:

```cpp
int level = 3;
const char* s = argv[argOffset + 1];
auto [ptr, ec] = std::from_chars(s, s + std::strlen(s), level);
if (ec != std::errc{} || level < 0 || level > 22)
{
    std::cerr << "AssetPacker: invalid compression level '" << s << "' (expected 0-22)\n";
    return 1;
}
compressionLevel = level;
```

---

### 3.4 `ZSTD_compress` / `ZSTD_compressBound` called without a reusable context

Every compressed file calls `ZSTD_compress`, which allocates and frees a compression
context internally on each call. This is the documented "simple" API and is fine for
occasional use, but in a parallel pipeline where dozens of files are compressed
simultaneously, the repeated context alloc/free adds up.

Use `ZSTD_CCtx` per worker thread:
```cpp
// Inside ProcessFile or passed in to it
ZSTD_CCtx* cctx = ZSTD_createCCtx();
const std::size_t result = ZSTD_compressCCtx(cctx, dst, bound, src, srcSize, level);
ZSTD_freeCCtx(cctx);
```

Or better — pass a `ZSTD_CCtx*` into `ProcessFile` from a per-thread pool so the
context is reused across files on the same thread. With `std::async` this is harder;
a proper thread pool makes it straightforward.

---

### 3.5 `std::async(launch::async)` creates one thread per file — use a thread pool

```cpp
for (const auto& file : m_files)
    futures.push_back(std::async(std::launch::async, ProcessFile, ...));
```

For a directory with 500 assets, this spawns 500 threads simultaneously. Most OSes have
a practical limit of a few thousand threads, but the overhead from context switching alone
hurts throughput. Use a thread pool bounded to `std::thread::hardware_concurrency()`.

A minimal thread pool with a `std::queue` + `std::mutex` + `std::condition_variable`
is ~80 lines and gives much better real-world throughput. Alternatively, if C++17 parallel
algorithms are available, `std::for_each(std::execution::par_unseq, ...)` over the file
list is a one-line swap that the standard library handles correctly.

---

### 3.6 `reinterpret_cast<const uint32_t*>` on `std::byte*` in SpirvProcessor — use `std::bit_cast`

```cpp
const uint32_t* words = reinterpret_cast<const uint32_t*>(spv.data());
```

This is technically UB in C++ (violates strict aliasing rules) even though it works
everywhere in practice for SPIR-V. C++20 `std::bit_cast` doesn't apply here (wrong
size), but `std::memcpy` into a `uint32_t` is the strictly-correct alternative, or
use a typed span:

```cpp
// Option A — memcpy at access points (zero overhead with optimization)
auto readWord = [&](std::size_t i) {
    uint32_t w;
    std::memcpy(&w, spv.data() + i * sizeof(uint32_t), sizeof(uint32_t));
    return w;
};

// Option B — assert alignment and use std::launder (C++17)
// Only valid because std::byte storage is well-aligned for any type
const uint32_t* words = std::launder(reinterpret_cast<const uint32_t*>(spv.data()));
```

`memcpy` with `uint32_t` locals is zero overhead at `-O2` and strictly correct.

---

### 3.7 Missing `[[nodiscard]]` on return values that signal errors

The following return values are meaningful and silently discardable today:

```cpp
bool PakWriter::Write(const fs::path&);            // false = failed
int  MaterialImporter::ImportDirectory(const fs::path&); // -1 = error
bool MaterialImporter::ShouldWrite(...);
bool MaterialImporter::GeneratePropertiesForFolder(...);
```

Mark them `[[nodiscard]]`:
```cpp
[[nodiscard]] bool Write(const fs::path& outPath) const;
[[nodiscard]] static int ImportDirectory(const fs::path& sourceDir);
```

The compiler will then warn on any call site that ignores the result.

---

### 3.8 `ComputeSkeletonHash` allocates a heap buffer unnecessarily

```cpp
std::vector<uint8_t> payload;
payload.reserve(sortedBones.size() * 128);
// fill payload...
return XXH3_64bits(payload.data(), payload.size());
```

XXH3 supports streaming via `XXH3_state_t` — you can feed bone data directly without
assembling an intermediate buffer:

```cpp
uint64_t ComputeSkeletonHash(const std::vector<BoneInfo>& bones, const std::vector<uint32_t>& remap)
{
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (const auto& bone : bones)
    {
        XXH3_64bits_update(&state, bone.name.data(), bone.name.size());
        const int32_t remappedParent = (bone.parentIndex >= 0)
            ? static_cast<int32_t>(remap[bone.parentIndex]) : -1;
        XXH3_64bits_update(&state, &remappedParent, sizeof(remappedParent));
        XXH3_64bits_update(&state, bone.ibm.data(), sizeof(float) * 16);
    }
    return XXH3_64bits_digest(&state);
}
```

No allocation, same result.

---

### 3.9 Magic scoring numbers in `FindBestTexture`

```cpp
score += 25;     // include alias match
score += 50;     // exact stem match
score += 20;     // preferred alias match
```

These should be named constants:
```cpp
constexpr int kIncludeAliasScore  = 25;
constexpr int kExactStemBonus     = 50;
constexpr int kPreferredAliasScore = 20;
```

---

## 4. Function & Class Design

### 4.1 `TempVertex` → `DiskMeshVertex` copy is unnecessary

`TempVertex` is built per-primitive, then immediately copied field-by-field into
`DiskMeshVertex` in a second loop. The two structs have the same fields in the same
logical order. Either:

**Option A:** remove `TempVertex` and build directly into `DiskMeshVertex`. The normal
generation and bounds computation functions take `DiskMeshVertex&` instead.

**Option B:** if you want to keep `TempVertex` as a "working" type (easier to extend
without touching the disk format), at minimum make the copy a single `static_assert`-
guarded `memcpy` or a conversion operator to make the intent explicit.

The current field-by-field memcpy loop is 10 lines of noise that will go stale if either
struct changes.

---

### 4.2 `ProcessAsset` output parameters should be a struct

```cpp
std::vector<std::byte> ProcessAsset(
    const std::vector<std::byte>& raw,
    const fs::path&               diskPath,
    const std::string&            virtualPath,
    const fs::path&               sourceDir,
    std::string&                  outExt,      // out
    std::vector<ProcessedFile>&   outFiles)    // out
```

Two output parameters via non-const reference is the old C-style approach. Return a
struct:

```cpp
struct AssetProcessResult
{
    std::vector<std::byte>     data;
    std::string                replacementExt; // empty = no change
    std::vector<ProcessedFile> extraFiles;
};

AssetProcessResult ProcessAsset(std::span<const std::byte> raw, const fs::path& diskPath,
                                const std::string& virtualPath, const fs::path& sourceDir);
```

Callers become self-documenting and you can't accidentally forget to check one of the
outputs.

---

### 4.3 `ShouldWrite` reads the full file into memory for a header check

```cpp
const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
if (text.find("# Auto-generated by AssetPacker") != std::string::npos)
```

This reads the entire file (potentially hundreds of KB for a hand-edited TOML) just
to check whether the first non-blank line matches a header. Read only what you need:

```cpp
std::string line;
while (std::getline(in, line))
{
    if (!line.empty())
        return line.find("# Auto-generated by AssetPacker") != std::string::npos;
}
return false;
```

This also avoids the second `ShouldWrite` check for the placeholder patterns — those can
be moved to the same early-exit line scan.

---

### 4.4 `LoadManifest` version check is fragile

```cpp
const std::string expected = "# AetherPak manifest v" + std::to_string(kPackerVersion);
if (line != expected)
    return {}; // force full repack
```

This allocates a string and does a full string compare every time a manifest is loaded.
Minor issue, but the pattern repeats. More importantly: the manifest version and the
packer version are the same constant (`kPackerVersion`). If you ever want to bump the
packer version for a reason unrelated to manifest format (e.g., a BC7 quality change),
you'd force a full repack unnecessarily. Separate `kManifestVersion` from
`kPackerVersion`.

---

## 5. Include Strategy

### 5.1 `CGLTF_IMPLEMENTATION`, `STB_IMAGE_IMPLEMENTATION`, `XXH_IMPLEMENTATION` — isolate in one TU

Single-header library implementations should be compiled exactly once, in a dedicated
translation unit:

```cpp
// ThirdPartyImpl.cpp
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include <xxhash.h>
```

Remove the `#define XXH_IMPLEMENTATION` from `MeshProcessor.cpp` and the
`#define STB_IMAGE_IMPLEMENTATION` from `TextureProcessor.cpp`. Currently:
- `XXH_INLINE_ALL` in `PakWriter.cpp` and `XXH_IMPLEMENTATION` in `MeshProcessor.cpp`
  compile two different modes of the same library in the same binary.
- `CGLTF_IMPLEMENTATION` in `MeshProcessor.cpp` means cgltf recompiles every time
  MeshProcessor.cpp changes.

This also dramatically improves incremental build times — `stb_image` especially takes
several seconds to compile.

---

### 5.2 `PakFormat.hpp` include path is inconsistent

In `PakWriter.hpp`:
```cpp
#include <PakFormat.hpp>   // angle brackets — searches include paths
```

In most other places, engine/tool headers use quoted includes. `PakFormat.hpp` is in
`${CMAKE_SOURCE_DIR}/include` per the CMakeLists. Using angle brackets for your own
headers is not wrong (LLVM does it), but it's inconsistent with the rest of the project.
Pick one convention and apply it everywhere. Quoted includes (`"PakFormat.hpp"`) for
project headers, angle brackets for third-party/system headers is the most common
convention.

---

### 5.3 Forward declarations instead of full includes in headers

`MeshProcessor.hpp` includes `<filesystem>` and `<vector>` — both needed for the
function signature. `PakWriter.hpp` includes `<filesystem>`, `<string>`, `<vector>`,
and `<PakFormat.hpp>`. These are all reasonable given the signatures.

`MaterialImporter.hpp` includes `<optional>`, `<string_view>`, `<vector>`, and
`<filesystem>` — also all needed. These headers are already lean.

No action needed here, but note: if `BinaryFormats.hpp` or `PakFormat.hpp` starts
growing heavier, consider a forward-declaration header for the disk structs (just
`struct MeshHeaderDisk;` etc.) to keep compile times down.

---

### 5.4 `<iostream>` pulled into every translation unit

Every `.cpp` file includes `<iostream>` for `std::cout`/`std::cerr`. For a build tool
this is fine, but when you eventually move processors to a library used by an editor or
runtime, you don't want them printing directly. Consider a logger interface passed in or
a simple callback:

```cpp
// Minimal, no virtual dispatch required
using LogFn = std::function<void(std::string_view)>;
ProcessedResult Process(..., const LogFn& log = nullptr);
```

Or at minimum extract a `Log(std::string_view)` free function in `PipelineUtils` so
you have one place to redirect output later.

---

## 6. Naming

### 6.1 `uv` used for joint index scratch buffer

In `MeshProcessor.cpp`:
```cpp
std::array<cgltf_uint, 4> uv{};
// ...
cgltf_accessor_read_uint(jointsAcc, v, uv.data(), 4); // reading joint indices into 'uv'
```

`uv` is used as a scratch buffer for joint indices (integers), not UVs (floats). This
is directly above actual UV reading code and causes genuine double-takes when reading.
Rename to `jointIdx` or `scratch4u`.

---

### 6.2 `fv` is too terse for a scratch buffer name

```cpp
std::array<float, 4> fv{};
cgltf_accessor_read_float(posAcc, v, fv.data(), 3);
dst.position[0] = fv[0]; dst.position[1] = fv[1]; dst.position[2] = fv[2];
```

`fv` reads as "float vector" but doesn't tell you it's a scratch buffer. `scratch` or
`tmp` makes the intent clear. Alternatively, eliminate it entirely for position/normal/UV
reading since `cgltf_accessor_read_float` can write directly into struct member arrays:

```cpp
cgltf_accessor_read_float(posAcc, v, dst.position, 3);
cgltf_accessor_read_float(normAcc, v, dst.normal, 3);
cgltf_accessor_read_float(uvAcc, v, dst.uv, 2);
```

This removes the scratch buffer entirely for the simple cases.

---

### 6.3 `kPackerVersion` constant lives in the anonymous namespace of `PakWriter.cpp`

The packer version is internal to the manifest logic — that's correct. But the name
`kPackerVersion` doesn't distinguish it from a potential `PAK_VERSION` format version.
Rename to `kManifestVersion` (and separate it from a hypothetical `kToolVersion` if you
ever version the tool itself).

---

### 6.4 `ProcessedFile` vs `FileResult` vs `ExtraFile` — three structs for the same concept

`ProcessedFile` (inside the anonymous namespace) and `FileResult` both represent a
virtual-path + binary data + flags + hash. `FileResult` extends `ProcessedFile` with
`ok`, `errorMsg`, and `extraFiles: vector<ProcessedFile>`. This means `ProcessedFile`
is used both as a primary result and as a child of `FileResult`, which is confusing.

Rename for clarity:
```
FileResult         → PakFileResult   (the top-level per-file worker result)
ProcessedFile      → PakFileData     (the stripped-down version for extras)
```

Or flatten the hierarchy: `PakFileResult` contains `std::vector<PakFileData> outputs`
where the first entry is always the primary file, and subsequent entries are extras.
This removes the asymmetry between primary and extra files entirely.

---

## 7. Minor / One-Liners

These are small things that each take less than five minutes to fix but collectively
reduce noise.

**7.1 — Initialise `Bounds` members properly**

```cpp
struct Bounds {
    float aabbMin[3] = { 0, 0, 0 };
    float aabbMax[3] = { 0, 0, 0 };
    // ...
};
```

`= { 0, 0, 0 }` is redundant — zero-initialisation is the default for aggregates with
value-initialization. Either remove it or use `{}`. The inconsistency (some members use
`= { 0, 0, 0 }`, `sphereRadius = 0`) adds noise.

---

**7.2 — `AppendRawStr` is a misleading name**

```cpp
void AppendRawStr(std::vector<std::byte>& buf, const std::string& s)
{
    AppendBytes(buf, s.data(), s.size()); // no length prefix, no null terminator
}
```

"Raw" doesn't tell you it has no framing. Call it `AppendStringData` or `AppendStringBytes`
to distinguish from `AppendStr` (which writes a `uint16_t` length prefix).

---

**7.3 — `IsAlreadyCompressed` should be `constexpr`-friendly**

The `kSkip` array is already `static constexpr`. The function itself could be `constexpr`
if the extension comparison were done at compile time. At minimum, the `static` on the
array is redundant inside a function — `constexpr` implies static storage duration.

---

**7.4 — `main.cpp` arg parsing should extract into a struct**

The flag parsing loop in `main.cpp` outputs three values (`importMaterials`,
`compressionLevel`, `argOffset`) via mutation of surrounding locals. Even without a
third-party parser, extracting into a function that returns a struct is cleaner and
more testable:

```cpp
struct Args {
    bool importMaterials  = false;
    int  compressionLevel = 3;
    fs::path sourceDir;
    fs::path outputPath;
};
std::optional<Args> ParseArgs(int argc, char* argv[]);
```

---

**7.5 — `.skel` magic bytes and version are default-initialised in the struct but never verified on the read side**

`SkelHeaderDisk::magic` is default-initialised to `{'S','K','E','L'}`. This is fine for
writing. But `BinaryFormats.hpp` has no `Validate(const SkelHeaderDisk&)` helper or even
a comparison helper for the magic bytes. The magic fields exist for a reason — add
`inline bool CheckMagic(const SkelHeaderDisk& h)` etc. so the engine loader has
something canonical to call. Or at minimum define `SKEL_MAGIC` as `std::array<char, 4>`
so it can be compared with `==` directly.

---

## Summary Table

| # | File(s) | Category | Change |
|---|---------|----------|--------|
| 1.1 | PakWriter.cpp | Structure | Split into 4 files |
| 1.2 | MaterialImporter | Structure | Class → namespace |
| 1.3 | PakWriter.cpp | Structure | Extract formatting utilities to PipelineUtils |
| 1.4 | MeshProcessor.cpp | Structure | Decompose `Process()` into sub-functions |
| 1.5 | TextureProcessor.cpp | Structure | Extract DDS definitions to DDSFormat.hpp |
| 2.1 | Multiple | Types | Standardize on `std::vector<std::byte>` + `ByteBuffer` alias |
| 2.2 | PakWriter.cpp | Types | `pathData` should be `std::byte`, not `char` |
| 2.3 | Multiple | Types | Use `std::span<const std::byte>` for buffer view params |
| 2.4 | MeshProcessor.cpp | Types | Add minimal Vec3/Vec4 for processing code |
| 2.5 | MeshProcessor.cpp | Types | Standardize loop index types |
| 2.6 | PakWriter.cpp | Types | Document or fix `mtimeTicks` platform dependency |
| 2.7 | TextureProcessor.cpp | Types | `GatherBlock` out param → `Block4x4&` |
| 3.1 | MeshProcessor.cpp | Idioms | Remove `static` from anonymous-namespace functions |
| 3.2 | TextureProcessor.cpp | Idioms | RAII-wrap `stbi_load` result |
| 3.3 | main.cpp | Idioms | Replace `std::stoi` with `std::from_chars` |
| 3.4 | PakWriter.cpp | Idioms | Reuse `ZSTD_CCtx` per thread |
| 3.5 | PakWriter.cpp | Idioms | Replace `std::async` free-for-all with thread pool |
| 3.6 | SpirvProcessor.cpp | Idioms | Replace strict-alias `reinterpret_cast` with `memcpy` |
| 3.7 | Multiple | Idioms | Add `[[nodiscard]]` to error-returning functions |
| 3.8 | MeshProcessor.cpp | Idioms | Streaming XXH3 — remove scratch allocation |
| 3.9 | MaterialImporter.cpp | Idioms | Name the scoring constants |
| 4.1 | MeshProcessor.cpp | Design | Remove `TempVertex` → `DiskMeshVertex` copy |
| 4.2 | PakWriter.cpp | Design | `ProcessAsset` out-params → return struct |
| 4.3 | MaterialImporter.cpp | Design | `ShouldWrite` — don't read full file |
| 4.4 | PakWriter.cpp | Design | Separate manifest version from packer version |
| 5.1 | Multiple | Includes | Isolate single-header impls in `ThirdPartyImpl.cpp` |
| 5.2 | PakWriter.hpp | Includes | Consistent angle-bracket vs quote include style |
| 5.4 | Multiple | Includes | Decouple `<iostream>` from processors |
| 6.1 | MeshProcessor.cpp | Naming | `uv` scratch buffer → `jointIdx` |
| 6.2 | MeshProcessor.cpp | Naming | `fv` scratch → write directly into struct members |
| 6.3 | PakWriter.cpp | Naming | `kPackerVersion` → `kManifestVersion` |
| 6.4 | PakWriter.cpp | Naming | Rename `ProcessedFile`/`FileResult` for clarity |
| 7.1–7.5 | Various | Minor | See section 7 |
