# Gameplay API contract tooling

The C ABI source of truth is src/engine/scripting/GameplayAbi.h. The five-function legacy engine-call surface is described by engine-calls.json. The generator verifies those signatures against both existing native exports and C# imports, then emits thin declarations. It does not change the C# pipeline or create a Rust gameplay runtime.

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

Generated Rust projects resolve this crate through AETHERCORE_RUST_SDK (a Cargo.toml file), the source-tree build hint, then data/sdk/rust/aethercore-abi/Cargo.toml beside the executable. Only Cargo.toml and Rust sources are staged; Cargo build outputs are never bundled. This raw crate does not yet implement the safe gameplay SDK or script registration.
