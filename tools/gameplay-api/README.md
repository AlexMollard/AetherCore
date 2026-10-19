# Gameplay API contract tooling

The C ABI source of truth is src/engine/scripting/GameplayAbi.h. The five-function legacy engine-call surface is described by engine-calls.json. The generator verifies those signatures against both existing native exports and C# imports, then emits thin declarations. It does not change the C# pipeline or create a Rust gameplay runtime.

Run from the repository root:

```powershell
python tools/gameplay-api/generate.py
python tools/gameplay-api/generate.py --check
python tools/gameplay-api/test_generate.py
```

Generated C declarations: src/engine/scripting/generated/GameplayEngineCalls.h.
Generated Rust: tools/gameplay-api/generated/gameplay_abi.rs and engine_calls.rs.
Do not edit generated files. The ABI generator handles the restricted v1 declaration grammar, not arbitrary C. Validate changes with the actual compiled Rust/C++ fixture, not only text generation.

See fixture/README.md for native and browser checks and docs/architecture/gameplay-abi.md for ownership, sizes, errors, capability discovery, caller buffers, report statuses and test commands. JSON Schema tests use requirements-test.txt installed into an ignored local tool environment. Existing C# exports remain unsafe/context-dependent raw FFI; safe SDK work follows the checked engine table and handle/session rules.
