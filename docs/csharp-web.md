# C# browser support

C# games can publish to the existing WebGPU 2D player through the editor, CLI or MCP `target: "web", operation: "publish"`. Use the same [Web publishing command](web-export.md) with `projects/AgentPipelineCSharp` or a selected C# project. Rust exports and native C# compilation, collectible reload, serialization and desktop publishing keep their existing paths. No project language is changed automatically.

## Qualified configuration

The browser build pins .NET SDK **10.0.303**, Mono/WebAssembly SDK **10.0.9**, installed workload manifests **10.0.109**, and engine Emscripten **6.0.12**. All must already be installed. A process-local copy of those installed manifests selects compatible packs even when the machine's default SDK is newer. The browser folder's `global.json` does not change the desktop SDK. Missing prerequisites fail with `WEB_MANAGED_TOOLCHAIN`; no workload is installed automatically.

CMake owns the build. `AETHERCORE_WEB_MANAGED=ON` builds the existing C++ browser engine and C# gameplay adapter into `AetherHost.a`, with reflected component registrations linked as objects. Its `GameRuntime` target invokes `dotnet_wasm.py`, which publishes the selected gameplay project and links the native engine into Mono's Wasm module. CLR, engine and gameplay share one linear memory and the existing C ABI. Desktop hostfxr, CoreCLR loading and assembly unloading are absent from this source closure. The final native module undergoes the same forbidden-import/shared-memory audit as Rust.

The supported mode is **trimmed interpretation**, with prebuilt gameplay. `AetherGame` script types, constructors, fields and callbacks are preserved, along with base callback metadata and typed component-reference constructors/properties. Browser scene fields use the existing shared gameplay host and serializer. Browser Roslyn compilation and collectible assembly reload are unavailable; agents rebuild and republish. Globalization is invariant. Third-party dependencies need their own browser compatibility checks.

The measured Emscripten compatibility prelude is deliberately small and tracked. It restores startup hooks expected by this Mono version; JavaScript exception fallback, exported heap views, assertions and native linker settings are explicit. Do not remove these settings based solely on a successful desktop build. The isolated [interop probe](../tools/experiments/csharp-webgpu/README.md) tests UTF-8 including NUL, structures by value, unmanaged callbacks, managed exceptions and 60 script updates on both stock and engine toolchains.

AOT was tested separately with `python tools/experiments/csharp-webgpu/run.py --mode engine --aot --report logs/task14/aot/interop.json`. The installed Mono AOT output fails in engine Clang with `Target-incompatible DataLayout`; it is **not a supported export mode**. The interpreter succeeds with the same ABI. AOT needs a separately qualified compiler/runtime combination before enabling it.

## Supported gameplay surface

The proven C# subset includes script discovery/properties, attach/update/detach, entity validity/destruction, position, keyboard input, scene find/load, prefab instantiation, UI text, Box2D linear velocity and collision/trigger callbacks, default one-shot Sfx audio, and durable browser saves. Script callback failures become `BROWSER_CSHARP` diagnostics; initialization failures become `BROWSER_CSHARP_STARTUP`. Unsupported audio volume/pitch/loop/bus options fail explicitly rather than silently changing behavior.

This is a scoped SDK bridge, not full desktop SDK parity. Other native SDK imports fail during linking and retain `managed-publish.log`; managed APIs that require unavailable browser services fail at startup with diagnostics. External native imports, networking and editor tools are rejected in preflight. 3D physics callbacks are explicitly unsupported. Native networking/editor behavior is unchanged. The underlying WebGPU content profile still supports its documented authored 2D sprites, UI, physics and assets; this does not imply every C# wrapper has a browser implementation.

Exports include nested `_framework` resources and a relative module loader. ZIP verification checks their hashes and runs the actual extracted game at a nested URL and in a sandboxed cross-origin iframe. Failed compilation or verification preserves the previous export. Browser saves continue using the existing asynchronous browser storage contract, including UTF-8/NUL values. External hosting, Firefox, Safari and non-Windows browser qualification remain pending.

## Reproduce qualification

`python tools/project/distribution.py --help` describes the bounded aggregate runner. It creates fresh Rust and C# games, publishes and semantically edits both, checks serialized C# float/vector/string/enum/entity/component-reference fields after trimming, exercises the same physics/audio/save/scene scenario directly and in an iframe, and records comparable measurements. The C# measurements are reported separately; the existing small Rust fixture's performance ceilings are not silently widened.

Evidence is retained under `logs/task14/`, including the aggregate `distribution-final.json`, ABI probes, normal CLI publishing, native regressions, and managed baseline comparison. The matched fresh-project run measured:

| Measurement | Rust | C# interpreter |
| --- | ---: | ---: |
| ZIP bytes | 1,106,219 | 2,151,472 |
| Cold / warm interactive navigation | 525.0 / 489.8 ms | 381.0 / 437.6 ms |
| Wasm linear memory | 17,563,648 bytes | 33,554,432 bytes |
| p95 observed frame interval | 16.8 ms | 16.8 ms |
| Semantic edit to verified export | 17.8 s | 19.2 s |
| Fresh Web publish | 119.1 s | 141.5 s |

Startup varies between runs; these single samples do not establish a language ordering. These measurements describe one small fixture on this Windows/hardware-Chrome host, not general game performance. Standalone desktop managed tests returned 137 passed and the exact same 29 pre-existing host-import failures as the previous baseline; the comparison is recorded separately from the passing native/browser acceptance checks. Trim analysis still emits warnings for reflection paths; the explicit roots and actual serialized-field checks qualify this profile, not arbitrary third-party reflection.
