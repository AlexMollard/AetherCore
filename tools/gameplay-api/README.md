# Gameplay API contract tooling

The C ABI source of truth is src/engine/scripting/GameplayAbi.h. The five-function legacy engine-call surface is described by engine-calls.json. The generator verifies those signatures against both existing native exports and C# imports, then emits thin declarations. The desktop safe SDK and native host build on this contract while preserving the C# pipeline.

Run from the repository root:

```powershell
python tools/gameplay-api/generate.py
python tools/gameplay-api/generate.py --check
python tools/gameplay-api/test_generate.py
```

Generated C declarations: src/engine/scripting/generated/GameplayEngineCalls.h.
Generated Rust: tools/gameplay-api/generated/gameplay_abi.rs and engine_calls.rs. The same canonical ABI is also emitted into rust/aethercore-abi/src/generated.rs, the standalone raw bindings crate shipped beside the editor. The generator's --check verifies both copies.
Do not edit generated files. The ABI generator handles the restricted v1 declaration grammar, not arbitrary C. Validate changes with the actual compiled Rust/C++ fixture, not only text generation.

See fixture/README.md for native and browser checks and docs/architecture/gameplay-abi.md for ownership, sizes, errors, capability discovery, caller buffers, report statuses and test commands. JSON Schema tests use requirements-test.txt installed into an ignored local tool environment. Existing C# exports remain unsafe/context-dependent raw FFI; safe SDK work follows the checked engine table and handle/session rules.

Check the dependency-free raw crate without network access:

```powershell
cargo test --manifest-path rust/aethercore-abi/Cargo.toml --offline --target-dir .temp/gameplay-api-cargo
```

New Rust projects depend on the safe aethercore crate, selected by AETHERCORE_RUST_GAMEPLAY_SDK (a Cargo.toml file), the bundled SDK or the source-tree build hint. AETHERCORE_RUST_SDK selects the underlying raw crate separately. Only Cargo.toml and Rust sources are staged; Cargo build outputs are never bundled. The separate aethercore crate implements the safe SDK and macro-generated script registration.

Run safe SDK unit/compile-fail tests and the real provider subprocess checks:

```powershell
cargo test --manifest-path rust/Cargo.toml --workspace --offline --target-dir artifacts/task5/sdk
cargo build --manifest-path projects/AgentPipelineRust/scripts/Cargo.toml --bin provider_probe --offline --target-dir artifacts/task5/fixture
cargo build --manifest-path projects/AgentPipelineRust/scripts/Cargo.toml --bin provider_probe --profile abort --offline --target-dir artifacts/task5/abort
python tools/gameplay-api/test_rust_provider_process.py
```

Set `AETHER_RUST_TEST_MODULE` to the fixture library and `AETHER_RUST_WRONG_ABI_MODULE` to a fixture built with `--features wrong-abi` before the native gauntlet to enable real loader tests. `AETHER_RUST_BUILD_TESTS=1` enables actual CMake/Cargo success and failed-rebuild tests (including non-ASCII paths and shell metacharacters). Without these inputs, only those optional platform integration cases are skipped; never treat them as qualified.
