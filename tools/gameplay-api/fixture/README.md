# Gameplay ABI foreign-consumer fixture

This is a contract test module, not the production Rust scripting host. It consumes
the generated Rust declarations from `../generated/gameplay_abi.rs`, implements
the actual `aether_gameplay_query` symbol, and links with a C++ caller and the
production `GameplayAbiValidation.cpp`. No ECS, GPU, editor, or .NET runtime is
needed.

From the repository root:

```powershell
./tools/gameplay-api/fixture/Run-Fixture.ps1 -Target All `
    -PlaywrightModule <absolute-path-to-playwright-package>
```

The default Emscripten SDK is `.temp/toolchains/emsdk-main` and Rust Wasm toolchain
is `nightly-2026-10-18`, matching the feasibility experiment. Rust standard
libraries are rebuilt against that SDK. The runner does not install tools.
Native verification currently selects Windows x64/MSVC ABI; the browser case
selects Wasm32 in installed Chrome. Supply `-BrowserExecutable` to test another
Chromium executable. Outputs go to ignored `artifacts/gameplay-api/fixture` and
reports to `logs/gameplay-api/fixture`.

Coverage includes all record sizes, alignments, and field offsets; accepted and
rejected query negotiation; every mandatory module callback; engine callback
validation and invocation; UTF-8 descriptors and caller-owned string buffers;
lifecycle ordering and failed attach cleanup; script status propagation; and
unsupported optional events. Strings include non-ASCII UTF-8 and an embedded NUL.

The browser fixture enables Emscripten C++ exception catching with `-fexceptions`
so the production query/event validators can contain throwing C++ callbacks.
Rust uses `panic=abort` on both targets. This does **not** demonstrate recovering
from a Rust panic, nor throwing exceptions across a Rust frame: both are outside
the ABI contract. The production provider must contain errors before returning.

The page publishes its terminal JSON result as `globalThis.gameplayAbiResult`.
The browser runner fails on page errors, missing terminal result, failed checks,
or an unexpected target width. Each build/run stage has a process-tree deadline;
native results and browser results report check counts and pointer width.
