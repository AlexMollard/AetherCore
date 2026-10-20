# C# WebGPU interop experiment

This is Task 14's prerequisite, not an enabled C# Web exporter. It publishes a
trimmed, interpreted .NET 10.0.9 application linked with native C++ in one Wasm
memory. It checks scalar calls, length-aware UTF-8 with an embedded NUL, the
engine ABI's vector struct in both directions, native-to-managed callbacks,
60 managed updates, caught managed exceptions and preserved reflected fields.

```powershell
python tools/experiments/csharp-webgpu/run.py --mode stock --report logs/task14/stock.json
python tools/experiments/csharp-webgpu/run.py --mode engine --report logs/task14/engine.json
python -m unittest discover -s tools/experiments/csharp-webgpu -p test_probe.py
```

The stock mode uses .NET's Emscripten 3.1.56 pack. The engine mode uses the
existing engine Emscripten 6.0.12 SDK, selected with `--emsdk` when necessary.
Both require installed SDK 10.0.303, Mono browser-wasm 10.0.9 and workload
manifests 10.0.109. A process-local manifest overlay selects those existing
packs without changing the machine's SDK installation. No workloads or tools
are installed automatically. The shared deadline is `--timeout-seconds 1..1200`.

The Emscripten 6 experiment uses JavaScript exception handling because the
stock Mono Wasm-EH archive references an old unwind ABI. Its small tracked
startup compatibility prelude restores Module.ready, declares the old module
path name, and delays Mono preInit until the exported runtime helpers exist.
Heap views are explicitly exported so Mono sees current views after memory
growth. System/third-party files are never patched. Debug stripping by the old
workload Binaryen is disabled; Emscripten 6 owns native linking/optimization.

Reports distinguish this ABI prerequisite from actual gameplay qualification:
`gameplayQualified` remains false. A passing probe does not prove WebGPU,
scene/SDK behavior, publishing, AOT, iframe behavior or M6. C# Web capability
must remain disabled until those separate checks pass. Desktop hostfxr and
collectible assembly loading are not used by this experiment.

The optional --aot flag runs a separate experimental build cache. On the pinned
engine toolchain it fails with a Target-incompatible DataLayout backend error.
This does not block trimmed interpretation, which is the supported C# export
mode. See [C# browser support](../../../docs/csharp-web.md) for actual gameplay
qualification and the selected SDK surface.
