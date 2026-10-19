# Rust/C++ Wasm and WebGPU feasibility

This is Task 1 of the Rust/WebGPU plan, deliberately separate from the engine build. It proves a mixed-language module and a textured WebGPU sprite; it is not an AetherCore browser backend or Rust gameplay SDK.

## Reproduce

Run from the repository root in PowerShell 7 on Windows. Prerequisites are Rust/Cargo, clang++, CMake/Ninja, Slang, the pinned Emscripten SDK, existing Box2D source, Node, Playwright and installed Chrome. Nothing is installed automatically.

The tested combination is recorded in toolchain.json. Install the nightly Rust toolchain with rust-src and the wasm32-unknown-emscripten target; the build recompiles std instead of assuming the distributed std matches Emscripten. Install/activate Emscripten 6.0.12 into a local SDK directory. For native Rust use 1.99.0 as the active toolchain. See the linked official instructions in the architecture report.

If Playwright is not already available, an isolated install can be made with npm install --prefix .temp/rust-webgpu-browser-tools playwright@1.62.1. This uses installed Chrome; a separate Playwright browser download is not needed. The SDK, package caches and outputs belong in ignored directories, not beside these sources.

```powershell
./tools/experiments/rust-webgpu/Run-Feasibility.ps1 `
  -EmsdkRoot D:/path/to/emsdk `
  -Box2dSource D:/path/to/box2d-3.1.1 `
  -PlaywrightModule D:/path/to/playwright
```

With the repository-local SDK and downloaded Box2D already available, only -PlaywrightModule is needed. Alternatively set AETHER_PLAYWRIGHT_MODULE to the installed package directory. -BrowserExecutable can select another Chromium binary; that is a new qualification run, not an automatic compatibility claim. -OutputRoot and -ReportRoot override artifacts/rust-webgpu and logs/rust-webgpu. StageTimeoutSeconds defaults to 600 and terminates the child process tree on expiry. A one-second limit is a reproducible timeout/failure-report check on the native/Wasm build stage.

Success requires exit code zero and feasibility-report.json status passed. The runner never treats an expected negative page result as a positive rendering result: it verifies that unavailable-adapter and invalid-shader cases fail for their intended diagnostics. Unknown/software adapters do not satisfy the real-GPU gate.

## Cases

- Native interop: 27 C++/Rust checks including wrapping integers, exact UTF-8 bytes, caller buffers, short-buffer/invalid-input handling, callbacks and target layout.
- Browser interop: the same mixed C++/Rust module and 27 checks, executed in Chrome rather than substituted with a Node run.
- Browser C++/Box2D: C++26 expected, format, span, PMR, coroutine and lexical filesystem operations, plus a single-threaded Box2D 3.1.1 falling-body simulation.
- WebGPU: Emdawn C API, Slang-generated WGSL, frame uniform, sprite storage buffer, sampler and texture; 4,096 readback pixels must match all four colors. Compilation, validation and readback completion all gate success.
- Failure cases: injected unavailable adapter and deliberately invalid shader. Neither disables browser security features.
- Harness checks: software-adapter rejection and positive/negative result classification. The aggregate script's timeout path has also been exercised against a real child build.

## Outputs and limits

The aggregate report contains tool identities, build timings, native/browser results, WGSL hash and generated-file sizes/hashes. Browser reports include browser/adapter identity, console errors, shader diagnostics and pixel samples; the screenshot is evidence of the actual canvas. CMake/compiler/Cargo logs are retained alongside the reports. Internal build intermediates are ignored and not source artifacts.

The C ABI uses target-local pointers and size_t only within a live process/module. Native record size is 32 bytes; Wasm record size is 24. Do not copy these pointer-bearing records into scenes, savegames or network packets. No Rust allocation is freed by C++; formatted data is copied into caller storage.

The prototype uses panic=abort and no C++ exceptions. It does not demonstrate panic recovery, engine-wide exception portability, arbitrary crate compatibility, a second Rust ECS, full native rendering, C# browser support, hot reload or gameplay performance. Native reproduction currently targets Windows/MSVC ABI; Linux and other browsers remain unqualified. See docs/architecture/rust-webgpu-toolchain.md for the measured outcome and baseline limitations.
