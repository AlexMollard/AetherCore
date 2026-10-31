<p align="center">
  <img src="resources/branding/aethercore-app-icon.png" alt="AetherCore logo" width="136">
</p>

<h1 align="center">AetherCore</h1>

<p align="center">
  <strong>A game engine for desktop and web.</strong><br>
  Build 2D and 3D games with C# or Rust, using the same editor and project.
</p>

<p align="center">
  <img alt="C++ 23 / 26" src="https://img.shields.io/badge/C%2B%2B-23%20%2F%2026-FF9429?style=flat-square&amp;labelColor=0F0D0B">
  <img alt="Vulkan" src="https://img.shields.io/badge/graphics-Vulkan-FF9429?style=flat-square&amp;labelColor=0F0D0B">
  <img alt=".NET 10" src="https://img.shields.io/badge/gameplay-.NET%2010-FF9429?style=flat-square&amp;labelColor=0F0D0B">
  <img alt="Rust gameplay" src="https://img.shields.io/badge/gameplay-Rust-FF9429?style=flat-square&amp;labelColor=0F0D0B">
  <img alt="WebGPU browser exports" src="https://img.shields.io/badge/browser-WebGPU-FF9429?style=flat-square&amp;labelColor=0F0D0B">
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake-FF9429?style=flat-square&amp;labelColor=0F0D0B">
</p>

<p align="center">
  <a href="https://alexmollard.github.io/AetherCore/">Website</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#documentation">Documentation</a>
</p>

![AetherCore showcase: FoxShowcase’s shrine in WebGPU, INKBOUND’s 2D scene and native material-graph authoring](docs/images/showcase.webp?v=efeac7e4)

Actual engine captures: browser gameplay, 2D scenes and material authoring. [Explore the engine →](https://alexmollard.github.io/AetherCore/)

## What is AetherCore?

AetherCore brings scene authoring, gameplay, materials, physics and rendering into one native workspace. It uses Vulkan on desktop and WebGPU for supported browser exports; games run on the visitor’s device without an engine installation.

- **Editor:** dockable viewport, hierarchy, component inspectors, sprite tools and material authoring.
- **Gameplay:** C# and Rust SDKs, Jolt 3D and Box2D 2D physics, animation, UI and audio.
- **Rendering:** render graph, GPU-driven culling, lighting, shadows and post-processing.
- **Publishing:** standalone desktop games and self-contained WebAssembly/WebGPU players.
- **Tools:** profiling, resource inspection and MCP access to the live editor.

> [!NOTE]
> Under active development. APIs and asset formats may change. Browser support is profile-based, not universal desktop parity; see [Web exports](docs/web-export.md) and [C# browser limitations](docs/csharp-web.md).

Distribution is currently private; engine licensing is deferred. See [distribution terms](docs/DISTRIBUTION.md) and [desktop support and qualification policy](docs/support-policy.md).

## Quick start

For a Windows source build, install Visual Studio 2022 or newer with the C++ desktop workload, CMake 4.0+, the Vulkan SDK and .NET 10 SDK for C# gameplay.

```powershell
cmake --preset default
cmake --build --preset default
cd build/default
./src/app/RelWithDebInfo/Launcher.exe
```

Create or open a project in the Launcher, then follow the [first game tutorial](docs/getting-started.md).
For Linux, ClangCL, build options and release packaging, see [Building and releases](docs/building.md).

## Documentation

| Guide | Covers |
|---|---|
| [Getting started](docs/getting-started.md) | Make a small 2D game, add physics and scripts, and publish it |
| [Editor and gameplay workflow](docs/editor-workflow.md) | Projects, sprite authoring, C# assembly reload and Rust iteration |
| [Script APIs and examples](docs/getting-started.md#script-apis-and-examples) | Public SDK declarations and working C#/Rust examples |
| [Rust gameplay](docs/rust-gameplay.md) | Rust project creation, safe APIs and publishing |
| [Web exports](docs/web-export.md) | Browser targets, content restrictions and export verification |
| [Building and releases](docs/building.md) | Toolchains, presets, diagnostics, local checks and packaging |
| [Engine architecture and assets](docs/architecture/overview.md) | Subsystem map, frame handoff, asset mounts and processing |
| [Engine MCP](docs/mcp-setup.md) | Editor automation and offline gameplay jobs |
| [Visual Studio debugger MCP](docs/VisualStudio-MCP.md) | Managed debugging setup |
| [Website development](docs/website.md) | Local preview, manual Pages deployment and hosting limits |

Verification runs locally with `./scripts/Run-DebugGauntlet.ps1`. GitHub workflows are manual-only; pushing does not run checks or publish a release.
