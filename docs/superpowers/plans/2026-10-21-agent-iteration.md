# Measured agent iteration improvements

> **For agentic workers:** Use superpowers:executing-plans to implement and verify this scoped follow-on.

**Goal:** Reduce measured agent rebuild/run/relaunch cost without weakening verification or changing gameplay state semantics.

**Scope:** Task 15's agent-throughput evaluation, authorized by the user's choice to measure and improve agent workflows. Keep C#, Rust WebGPU exports and native builds working. Existing scene/configuration restoration remains the relaunch contract; arbitrary runtime migration, native hot reload, 3D and networking are outside this work.

**Approach:** Measure actual sequential CLI operations for both reference projects. Inspect compiler logs to distinguish compilation, launch and verification costs. Improve the largest justified source of repeated work. Shared compiler intermediates must remain separate from job-private runnable modules; serialize compilation and staging together so concurrent jobs cannot stage another build's output. Preserve deadlines, cancellation and failed-build diagnostics.

- [x] Record three baseline samples each for Rust/C# build, reference test and relaunch, with wall-clock and reported durations.
- [x] Reproduce redundant compilation and add failing regression checks for shared-cache staging, compile failure, concurrent jobs and cancellation.
- [x] Implement the measured optimization through the existing CMake-owned native build path, preserving job-private outputs.
- [x] Repeat the same measurements; verify a semantic edit, failed build and repair use the right module.
- [x] Run affected native, C#, Rust and browser regressions; perform one fresh-context review and document evidence and limits.

## Results

Three samples per operation: median Rust build/test/relaunch improved from 3480/6342/6278 ms to 1829/4351/4451 ms; C# improved from 5329/8192/8050 ms to 2807/5498/5594 ms. The public benchmark tool also completed a separate six-operation run.

The single fresh review identified stale cached artifacts after manifest changes, Visual Studio SDK-copy invalidation, and an independent lock timeout. Fixes validate Cargo/MSBuild's current artifact metadata, stage both SDK helpers through StageAppBundle, and leave deadline enforcement with the parent process. Actual Cargo library and C# assembly renames now reject stale cached output; source edits, failed builds and repair passed for both languages. Compiler/cache cancellation and concurrent staging passed four Python tests across both helpers.

Full native gauntlet: 1463 cases, both GPU smokes, zero validation findings. Browser: all 41 cases, owned browser/server/child cleanup confirmed. Rust and C# Web publishes passed relocated package/iframe verification. Project tests passed 39 cases with one opt-in skip; MCP tests passed 22 cases. clangd and graphify were refreshed. Evidence: `logs/agent-iteration/`, with usage and limitations in [agent iteration](../../agent-iteration.md).

**Git:** Reuse the established checkout. The user explicitly authorized committing the verified workflow improvements after qualification.
