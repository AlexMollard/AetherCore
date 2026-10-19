# Browser runtime build boundary

Task 8 adds the `webgpu-browser` CMake preset and a linked runtime foundation.
It is not yet a playable browser export: renderer, browser input, audio and storage
remain unavailable until Tasks 9–10. Its machine-readable runtime result reports
those capabilities as false. No feasibility renderer is substituted for the engine.

The foundation compiles the engine's real `World`, system registry, transform/handle
access and gameplay ABI validation. It creates an ECS entity, moves it through the
ABI, steps Box2D 3.1.1 for 60 fixed updates with zero workers, and validates the
header of a package produced by the native AssetPacker. The native Slang compiler
also generates WGSL from the previously validated sprite source. Loading, cooking,
rendering and playing the complete reference game are later gates.

## Build and check

```powershell
./scripts/Run-WebRuntimeChecks.ps1 `
    -EmsdkPath .temp/toolchains/emsdk-main `
    -SlangCompiler C:/VulkanSDK/1.4.350.0/Bin/slangc.exe
```

This builds AssetPacker through the native CMake preset before activating Emscripten.
`-SkipHostBuild` reuses existing verified tools. `-AssetPackerPath` can select a
different native build. The script writes its report and command logs under
`logs/web-runtime/` by default. Emscripten and Slang versions come from the Task 1
toolchain manifest. AssetPacker and Slang must be native binaries that pass identity
checks; a Wasm module renamed to `.exe`, JavaScript or a missing executable fails
before cross-build packaging.

For direct CMake use, activate the pinned Emscripten SDK, set
`AETHER_WEB_ASSETPACKER` and `AETHER_WEB_SLANGC` to absolute native executable paths,
then run `cmake --preset webgpu-browser` and
`cmake --build --preset webgpu-browser`. Outputs live under
`build/webgpu-browser/web/`. The linked program can be checked with the SDK's Node
from that output directory, where its preloaded `.data` file resides. Node execution
qualifies the link/runtime foundation, not browser graphics or browser API behavior.

`WebRuntimeAudit` runs after linking. It parses the binary Wasm import and memory
sections, rejects native graphics/.NET/socket/thread creation imports and shared
memory, and audits actual `compile_commands.json` sources. Its reports list imports
and the full compiled source closure. Desktop package discovery is bypassed before
including `Dependencies`, `DotNet` or `SlangShaders`; the browser check explicitly
disables Vulkan package discovery to verify that separation.

The Windows `runtime-only-native` preset separately configures with Editor, Launcher
and native tests disabled and builds GameRuntime. Linux can use the existing
`linux-clang` preset with those three cache switches disabled. Existing native
presets retain Vulkan, .NET, Editor and Launcher behavior.

## Sources and worker audit

`CMake/PortableDependencies.cmake` holds shared GLM, EnTT and Box2D versions.
`CMake/WebSources.cmake` explicitly lists browser engine translation units; adding
a native `.cpp` elsewhere cannot silently enter the browser build. Native globs
exclude browser-only translation units. World no longer imports the Vulkan-backed
animation allocator merely to store an `AnimationDatabase*`; native consumers that
use that type include its definition directly.

| Native facility | Foundation policy |
| --- | --- |
| Vulkan, VMA, Aftermath, GLFW | Not discovered, linked or compiled |
| CoreCLR, hostfxr, nethost, managed assemblies | Not discovered, staged or linked |
| Editor/Launcher, ENet/plum and native sockets | Excluded |
| RenderThread and coroutine executors | Excluded; no browser frame loop yet |
| IOThread and native VFS backends | Excluded; only Emscripten's preloaded virtual files are read |
| AssetManager decode/upload queues | Excluded; native tools produce the foundation's input assets |
| Jolt and its job workers | Excluded |
| Box2D | Pinned C library, explicit workerCount=0, no enqueue/finish worker callbacks |
| Physics2DSystem tile-asset/VFS integration | Deferred to the browser asset milestone; no fake asset store |
| Native logger/file rotation thread | Replaced by a browser console sink |
| Gameplay access mutex/owner-thread checks | Shared code, no thread creation or shared Wasm memory |
| miniaudio device threads, native input/window, native save files | Excluded; corresponding browser capabilities remain false |

The no-exceptions build preserves `Expected<T>` returns and compiler-supported
native callback catch barriers. In a compiler configuration without exceptions,
invoking the explicitly fatal `Throw` helper logs and aborts; it never silently
turns a failed `Expected` into success. Rust panic/ABI integration remains governed
by the earlier pinned abort policy; this target does not yet link a Rust game.

## Scope of evidence

The Task 8 foundation verifies binary/tool/source boundaries, portable engine ABI
execution, single-thread physics and host-produced package/shader inputs. It does
not certify reference-game parity, browser GPU pixels, browser save persistence,
audio after a gesture, or asynchronous browser lifecycle. Those are the explicit
Tasks 9–11 gates. CI remains manual-only.

Task 8 was qualified on Windows with the pinned Emscripten 6.0.12 and Slang
2026.8 tools: native default and runtime-only builds succeeded; the linked Wasm
foundation executed with 42 compiled sources, 15 imports and no shared memory.
Nine audit tests passed, including rejection of disguised Wasm host tools and
thread flags in either compilation database representation. CMake also rejected
an actual Wasm output supplied as AssetPacker.

The detailed native suite passed all 1,454 cases after an earlier gauntlet run
reported one failure that did not reproduce. That earlier result is retained in
`logs/task8/native-gauntlet.json`; the passing detail is in
`logs/task8/native-unit-detail.log`. Both hidden native GPU validation smokes
reported zero findings. C# and Rust desktop reference games passed, and the hidden
Launcher UiShell answered its control endpoint and closed cleanly. clangd and
graphify were regenerated. Native Linux and real browser graphics are not
qualified by these Windows/Node checks.
