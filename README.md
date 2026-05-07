# ⚡ AetherCore

A C++20 Vulkan game engine with a modular app layer, ECS-driven gameplay, Jolt physics, and a custom asset pipeline.

---

## ✨ Features

- **Vulkan renderer** - render graph, GPU culling, forward pass, post-processing, triple buffering
- **Bindless resources** - materials and textures accessed via a bindless descriptor model
- **ECS world** - EnTT-based entity/component system with typed entity handles
- **Jolt physics** - component-based rigid body and shape authoring, no engine-call boilerplate
- **Animation system** - skeletal animation with a per-entity animator
- **Offscreen rendering** - render-to-texture camera targets
- **Voxel world layer** - chunk meshing and block registry
- **Asset pipeline** - virtual file paths, `.pak` bundles, PBR material presets via TOML
- **Tracy profiling** - integrated instrumentation via engine macros
- **Day/night cycle** - time-of-day driven lighting system
- **Async I/O** - background file loading with PAK and directory backends

---

## 🗂️ Project Layout

```
src/engine/       Core engine: renderer, ECS, physics, animation, assets, I/O
src/app/          Application loop, layer stack, game systems, voxel world
shaders/          Slang shader sources
resources/        Source assets (packed at build time)
tools/assetpack/  Asset packer tool
CMake/            Dependency and helper modules
```

---

## 🔧 Building

### Windows

**Prerequisites:** Visual Studio 2022, CMake, Vulkan SDK

```powershell
cmake --preset windows-vs2022
cmake --build --preset vs2022-debug
```

For faster incremental builds with Ninja:

```powershell
cmake --preset windows-ninja
cmake --build --preset ninja-debug
```

`App` is set as the startup project in Visual Studio generators.

---

### Linux

**Prerequisites:** GCC 11+ or Clang 13+, CMake 3.20+, Vulkan SDK

Install dependencies (Ubuntu/Debian):

```bash
sudo apt-get install build-essential cmake git libvulkan-dev vulkan-tools \
  libglfw3-dev libglm-dev libfreetype6-dev
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

Run:

```bash
./build/src/app/App
```

> If you use a non-standard Vulkan SDK, set `VULKAN_SDK` in your environment before running CMake.

---

## 📦 Asset Pipeline

Assets under `resources/` are packed into `build/<preset>/data/assets.pak` as a post-build step.
At runtime, assets are accessed via virtual paths: `assets://...`, `shaders://...`, etc.

### PBR Material Presets

Define reusable materials as TOML files under `resources/materials/`:

```toml
[material]
baseColorFactor = [1.0, 1.0, 1.0, 1.0]
metallicFactor  = 0.0
roughnessFactor = 0.9

[textures]
albedo            = "assets://textures/your_basecolor.png"
normal            = "assets://textures/your_normal.png"
metallicRoughness = "assets://textures/your_metalrough.png"
occlusion         = "assets://textures/your_ao.png"
emissive          = "assets://textures/your_emissive.png"
```

You can also use a folder layout (handy for downloaded PBR zips):

```
resources/materials/MyPBRFolder/
    properties.toml
    albedo.png
    normal.png
    roughness.png
```

### Auto-importing raw texture folders

```powershell
AssetPacker import-materials resources
```

or combined with packing:

```powershell
AssetPacker --import-materials resources build/vs2022/data/assets.pak
```

The importer detects common naming patterns (`*_Color`, `*_NormalGL`, `*_Roughness`, etc.) and auto-generates `properties.toml` files, leaving any hand-authored ones untouched.

---

## 🔬 Profiling

Tracy is enabled by default. To disable at configure time:

```powershell
cmake --preset windows-vs2022 -DAETHERCORE_ENABLE_TRACY=OFF
```

---

## 📚 Dependencies

Managed via CMake FetchContent / CPM:

| Library | Purpose |
|---|---|
| Vulkan SDK | Graphics API |
| vk-bootstrap | Vulkan instance/device setup |
| VulkanMemoryAllocator | GPU memory management |
| GLFW | Window and input |
| GLM | Math |
| EnTT | ECS |
| Jolt Physics | Rigid body simulation |
| stb | Image loading |
| cgltf | glTF model loading |
| FreeType | Font rasterization |
| Tracy | Performance profiling |
