# AetherCore code conventions

Canonical conventions for first-party C++ under `src/` and `tools/`. Agent-facing project instructions live in [AGENTS.md](../AGENTS.md), which links here.

## Naming

| Element | Style | Example |
|---|---|---|
| Namespaces | `snake_case` | `aether::coro` |
| Classes / structs | `PascalCase` | `RenderFramePacket` |
| Member functions | `PascalCase` | `BeginFrame()` |
| Member variables | `m_snake_case` | `m_frameIndex` |
| Static members | `s_snake_case` | `s_setObjectNameFn` |
| Parameters | `camelCase` | `frameIndex` |
| Locals | `camelCase` | `scaledDt` |
| Enum classes and values | `PascalCase` | `GpuFormat::R16G16B16A16Sfloat` |
| Constants | `kPascalCase` | `kMaxFramesInFlight` |
| Macros | `AE_UPPER_CASE` | `AE_TRY` |

## Headers and includes

- Use `#pragma once`, never include guards.
- First include in every `.cpp` is its matching `.hpp`.
- Then STL, third-party, and project headers, grouped manually.
- `.clang-format` has `SortIncludes: Never`; include order is intentional.

## Formatting

- Tabs for indentation; spaces for alignment.
- Brace wrapping on classes, functions, namespaces, structs, enums, control flow, `else`, and `catch`.
- Column limit is 250.
- Always use braces for `if`/`for`/`while`.
- No Doxygen. Use `//` comments where useful.
- Class-level comments should describe purpose and thread-safety.

## Error handling

- `Expected<T>` is `std::expected<T, AetherError>`.
- Use `AE_TRY(var, expr)` to unwrap `Expected` in init/error paths.
- Use `AE_EXPECT_OR_THROW(var, expr)` when exceptions are expected by the caller.
- Use `AE_UNEXPECTED(err)` for `return std::unexpected(err)`.
- `AE_ASSERT` is Debug-only; `AE_ASSERT_ALWAYS` ships and aborts.
- Vulkan calls return `Expected<T>` or are translated into `AetherError::Vulkan`.

## Logging

```cpp
AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless capacity: {}", capacity);
AE_WARN(LogCategory::App, "Could not query refresh rate.");
AE_ERROR(LogCategory::Vulkan, "Vulkan error: {}", error.what());
```

Known categories include `Engine`, `Vulkan`, `Asset`, `Render`, `Scene`, `Camera`, `UI`, `Input`, `Window`, `FileSystem`, `Animation`, `App`, `Validation`, `Std`, and `Unknown`.

## Ownership

- Use `std::unique_ptr` for exclusive subsystem/resource ownership.
- Use `std::shared_ptr` only where existing code already requires shared lifetime, such as coroutine shared state.
- GPU resource wrappers are move-only; follow the local deleted-copy/noexcept-move pattern.

---

<!-- headroom:rtk-instructions -->
# RTK (Rust Token Killer) - Token-Optimized Commands

When running shell commands, **always prefix with `rtk`**. This reduces context
usage by 60-90% with zero behavior change. If rtk has no filter for a command,
it passes through unchanged � so it is always safe to use.

## Key Commands
```bash
# Git (59-80% savings)
rtk git status          rtk git diff            rtk git log

# Files & Search (60-75% savings)
rtk ls <path>           rtk read <file>         rtk grep <pattern>
rtk find <pattern>      rtk diff <file>

# Test (90-99% savings) � shows failures only
rtk pytest tests/       rtk cargo test          rtk test <cmd>

# Build & Lint (80-90% savings) � shows errors only
rtk tsc                 rtk lint                rtk cargo build
rtk prettier --check    rtk mypy                rtk ruff check

# Analysis (70-90% savings)
rtk err <cmd>           rtk log <file>          rtk json <file>
rtk summary <cmd>       rtk deps                rtk env

# GitHub (26-87% savings)
rtk gh pr view <n>      rtk gh run list         rtk gh issue list

# Infrastructure (85% savings)
rtk docker ps           rtk kubectl get         rtk docker logs <c>

# Package managers (70-90% savings)
rtk pip list            rtk pnpm install        rtk npm run <script>
```

## Rules
- In command chains, prefix each segment: `rtk git add . && rtk git commit -m "msg"`
- For debugging, use raw command without rtk prefix
- `rtk proxy <cmd>` runs command without filtering but tracks usage
<!-- /headroom:rtk-instructions -->
