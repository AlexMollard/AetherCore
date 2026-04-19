# AetherCore

AetherCore is a C++20 Vulkan engine with a small app layer on top.
The project is split into:

- `Engine` (static library): rendering, ECS world/scene, assets, input, camera, render graph
- `App` (executable): game/application loop, layers, UI wiring, engine systems

## Current Runtime Model

- Game thread:
	- pumps window/input events
	- updates world systems and layers
	- writes draw/UI data into per-frame slots
	- builds a `RenderFramePacket` snapshot
	- submits packet to render thread
- Render thread:
	- owns Vulkan frame execution (`BeginFrame`/`EndFrame` path)
	- waits on GPU fences and performs record/submit/present
- Frame pacing:
	- `FramePacer` is used at the top of the app loop
	- target FPS is auto-set from the display refresh rate
	- improves frame-time consistency and reduces game-thread submit stalls

## Configuration

- Engine settings default to `engine.toml`.

## Rendering Highlights

- Vulkan + vk-bootstrap + VMA
- Render graph based pass execution
- GPU culling path + forward pass + post-processing stack
- Triple buffering (`kMaxFramesInFlight = 3`)
- Bindless-style resource access for materials/textures
- Offscreen camera render targets (RTT)

## Asset + Content Pipeline

- `AssetPacker` tool is built as part of the workspace
- `App` post-build step packs everything under `resources/` into:
	- `build/<preset>/data/assets.pak`
- Runtime loads assets through virtual paths (for example `assets://...`, `shaders://...`)

### Material Presets (TOML)

You can define reusable PBR material presets as TOML files under `resources/materials/`.

Example:

```toml
[material]
baseColorFactor = [1.0, 1.0, 1.0, 1.0]
metallicFactor = 0.0
roughnessFactor = 0.9

[textures]
albedo = "assets://textures/your_basecolor.png"
normal = "assets://textures/your_normal.png"
metallicRoughness = "assets://textures/your_metalrough.png"
occlusion = "assets://textures/your_ao.png"
emissive = "assets://textures/your_emissive.png"
```

Usage from app/systems:

```cpp
std::vector<aether::Texture> keepAlive;
aether::Material mat = assets.LoadMaterialPreset("assets://materials/YourMaterial.toml", keepAlive);
```

Keep the returned textures alive as long as the material is used.

### Folder-Based Material Layout

You can also package materials as folders (useful for downloaded PBR zip contents):

```text
resources/materials/MyPBRFolder/
	properties.toml
	albedo.png
	normal.png
	roughness.png
```

Load it by folder path:

```cpp
std::vector<aether::Texture> keepAlive;
aether::Material mat = assets.LoadMaterialPreset("assets://materials/MyPBRFolder", keepAlive);
```

Notes:

- `properties.toml` is required for explicit folder presets.
- Texture values can be explicit file names (`albedo.png`), stems (`albedo`), or full virtual paths.

### AssetPacker Material Import

AssetPacker supports auto-generating `properties.toml` files from raw extracted texture folders:

```powershell
AssetPacker import-materials resources
```

or as part of packing:

```powershell
AssetPacker --import-materials resources build/vs2022/data/assets.pak
```

Importer behavior:

- Detects common messy downloaded naming patterns (for example `*_Color`, `*_NormalGL`, `*_Roughness`, `*_AmbientOcclusion`).
- Auto-refreshes previously auto-generated (or placeholder-template) `properties.toml` files during build.
- Leaves custom hand-authored `properties.toml` files untouched.

## Dependencies

Managed through CMake FetchContent:

- Vulkan SDK
- GLFW
- GLM
- EnTT
- vk-bootstrap
- VulkanMemoryAllocator (VMA)
- stb
- cgltf
- FreeType
- Tracy (optional instrumentation, enabled by default in this repo)


## Build (Linux)

Prerequisites:

- GCC 11+ or Clang 13+
- CMake 3.20+
- Vulkan SDK (or system Vulkan development packages)
- GLFW, GLM, FreeType, and other dependencies (see below)

### Install dependencies (Ubuntu/Debian example)

```bash
sudo apt-get update
sudo apt-get install build-essential cmake git libvulkan-dev vulkan-tools libglfw3-dev libglm-dev libfreetype6-dev
```

### Configure and build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Run

```bash
./build/src/app/App
```

### Notes
- If you use a non-standard Vulkan SDK, set `VULKAN_SDK` in your environment before running CMake.
- If you see missing symbols for `dladdr` or backtrace, ensure you are linking with `-ldl` (CMake handles this automatically).
- Tracy is enabled by default; to disable: `-DAETHERCORE_ENABLE_TRACY=OFF`.
- For other distros, install the equivalent development packages for Vulkan, GLFW, GLM, and FreeType.

## Build (Windows)

Prerequisites:

- Visual Studio 2022 (or Ninja)
- CMake
- Vulkan SDK installed and available to CMake

Recommended preset flow:

```powershell
cmake --preset windows-vs2022
cmake --build --preset vs2022-debug
```

Alternative (faster incremental builds):

```powershell
cmake --preset windows-ninja
cmake --build --preset ninja-debug
```

## Run

After build, launch `App` from your selected build preset output.
In Visual Studio generators, `App` is configured as the startup project.

## Profiling

Tracy instrumentation is integrated through engine macros.
To disable Tracy at configure time:

```powershell
cmake --preset windows-vs2022 -DAETHERCORE_ENABLE_TRACY=OFF
```

## Repo Layout (Quick View)

- `src/engine/` core engine systems and renderer
- `src/app/` application loop, layers, app-facing glue
- `shaders/` slang shader sources
- `resources/` source assets to be packed
- `tools/assetpack/` asset packer tool
- `CMake/` dependency and helper modules
