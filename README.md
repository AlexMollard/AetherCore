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
