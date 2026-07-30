# Memory Allocator & Tracking Subsystem

**Date:** 2026-07-30
**Status:** Approved design

## Motivation

AetherCore has no CPU-side allocator and effectively no CPU memory tracking. The only
existing piece is `src/engine/utils/MemoryTracker.cpp`, which overrides global
`operator new`/`delete` to forward straight to `std::malloc` and emit Tracy events — and it
compiles to nothing unless `TRACY_ENABLE` is defined. Shipping builds have zero tracking.

The GPU side is well served (`GpuMemoryTracker`, `DiagnosticEngine`, `render.memory`
budgets). The CPU side is greenfield.

The driving goal is **visibility and debuggability**, not a performance rescue — there is no
measured allocation hotspot today. Two questions must become answerable:

1. "What is using this memory?" — per-subsystem attribution, live and peak.
2. "Am I leaking?" — leak reports, snapshot diffing, double-free and corruption detection.

Both must be answerable by a human in the editor **and by an AI agent over MCP**, matching
the existing `render_stats` / `scene_stats` pattern. Agent-queryable memory data is a
first-class requirement, not a nice-to-have.

A speed improvement over the standard library comes along for free with the chosen backend,
and is treated as a measured acceptance criterion rather than an assumption.

## Scope decision

The full allocator framework (arenas, pools, frame allocators, tagged containers) is in
scope, chosen deliberately with the knowledge that it is **not currently justified by
measurement**. This is recorded as a risk (see Risks) with an explicit mitigation: tracking
lands first, allocators second, and adoption of tagged containers is incremental and driven
by tracking data rather than a blanket refactor.

## Backend: mimalloc

mimalloc is chosen not for raw throughput (rpmalloc benchmarks comparably) but because its
differentiating features *are* the requirements:

| Requirement | mimalloc facility |
| --- | --- |
| Per-tag / per-arena heap isolation | First-class heaps: `mi_heap_new`, `mi_heap_delete`, `mi_heap_destroy`, `mi_heap_set_default`, `mi_heap_get_backing` |
| Leak inspection / heap walking | `mi_heap_visit_blocks(heap, visit_blocks, visitor, arg)` enumerates every live block |
| Guard pages | `MI_SECURE=ON` (guard pages around metadata, encoded free lists, double-free detection, randomization); `MI_GUARDED=ON` for per-allocation guard pages |
| Heap corruption detection | Free-list pointer encoding with per-page keys |
| Drop-in override | `#include <mimalloc-new-delete.h>` in exactly one translation unit |
| Log integration | `mi_register_output(fn, arg)` routes mimalloc messages into `Logger` |
| Redirection sanity check | `mi_is_redirected()` |

MIT licensed, Microsoft-maintained, first-class MSVC/Windows support, consumed via the
existing CPM setup (`CMake/CPM.cmake`, `CMake/Dependencies.cmake`) as `mimalloc-static`.

`mi_stats_print` is deprecated and is **not** used. Per-tag numbers come from our own layer,
which is required anyway since mimalloc cannot know engine tags.

Rejected alternatives:

- **rpmalloc** — fast, tiny, public domain, but no heap-walk visitor, no secure/guard mode,
  weaker heap isolation. Every debugging feature would be written from scratch, which is the
  wrong trade for a debugging-motivated project.
- **Custom allocator** — total control and no dependency, but months of thread-cache and
  fragmentation tuning to approach mimalloc's throughput, and every heap-corruption bug
  becomes ours. The goal is finding leaks, not authoring an allocator.

## Attribution model — three layers

Attribution is layered because no single mechanism covers everything.

**Layer 1 — thread-local scope stack (primary).** `AE_MEM_SCOPE(MemTag::Physics)` at
subsystem entry points attributes *everything* allocated inside the scope, including
`std::vector` internals and third-party code, with zero refactoring. Implemented as a
`thread_local` current-tag value; the RAII object saves the previous tag and restores it on
destruction, so the C++ call stack is the tag stack — no heap allocation, no container.
This layer gives immediate whole-engine coverage.

**Layer 2 — explicit tagged allocators.** `ae::vector<T, MemTag::Rendering>` and friends for
structures where precise, scope-independent attribution matters. Compile-time, no runtime
attribution cost.

**Layer 3 — callstack capture (dev, opt-in).** Deduplicated stack captures so a leak report
names the exact allocation site. Symbols resolved lazily at report time only.

Layer 2 alone was rejected: it structurally cannot see allocations from `std` internals,
imgui, toml++, or Box2D — those would all land in `Unknown`. Layer 3 alone was rejected:
categories would be heuristics derived from symbol names rather than authoritative.

## Module layout

New folder `src/engine/memory/`:

| File | Responsibility |
| --- | --- |
| `MemoryTag.hpp` | `AE_MEMORY_TAGS(X)` macro list driving the enum, names, and iteration — adding a tag is one line, mirroring the `AE_COMPONENT` reflection philosophy |
| `MemoryScope.hpp` | `AE_MEM_SCOPE(tag)`, RAII save/restore over a `thread_local` tag |
| `MemoryStats.{hpp,cpp}` | Per-tag atomic counters: current bytes, peak bytes, live count, total allocations, total frees. Cache-line padded per tag to avoid false sharing. Lock-free. |
| `AllocationLedger.{hpp,cpp}` | Dev-only `ptr -> {size, tag, callstackId, frameIndex, serial, epoch}`. Sharded by pointer bits with a mutex per shard. |
| `CallstackDatabase.{hpp,cpp}` | Capture, hash, and deduplicate stacks to ids; lazy symbol resolution reusing existing `utils/Backtrace.cpp` |
| `MemoryService.{hpp,cpp}` | Facade: `Snapshot`, `DiffSnapshots`, `ReportLeaks`, `WalkHeap`, per-tag queries |
| `IAllocator.hpp` | `Allocate(size, align)`, `Deallocate(ptr, size, align)`, `Name()` |
| `HeapAllocator.{hpp,cpp}` | `mi_heap_t`-backed; optionally one heap per tag |
| `LinearAllocator.{hpp,cpp}` | Bump pointer, plus `FrameAllocator` covering N frames in flight with `Reset()` |
| `StackAllocator.{hpp,cpp}` | LIFO with `Marker Mark()` / `Release(marker)` |
| `PoolAllocator.hpp` | Fixed-size block free list, O(1); for ECS components, particles, physics proxies |
| `TrackedAllocator.hpp` | std-compatible allocator adapter carrying a tag, plus a `std::pmr::memory_resource` bridge |
| `Containers.hpp` | `ae::vector<T, Tag>`, `ae::string`, `ae::unordered_map` aliases |

`src/engine/utils/MemoryTracker.cpp` remains the single override translation unit and gains
`#include <mimalloc-new-delete.h>` plus the tracking hooks. It is no longer gated on
`TRACY_ENABLE`; Tracy events become one optional consumer of the hook rather than its reason
for existing.

### Two mandatory correctness details

- **Recursion guard.** A `thread_local bool` set while inside the tracker, so the tracker's
  own bookkeeping allocations are never themselves tracked.
- **Ledger self-allocation.** The ledger allocates from a dedicated raw `mi_heap_t`, never
  through the tracked path, for the same reason.

## Lifetime: the tracker cannot live in ServiceContainer

Global `operator new` runs during static initialization — before any engine object exists —
and again after `main` returns. `ServiceContainer` is documented as not thread-safe and is
constructed during engine startup, so it cannot own the tracker.

Therefore:

- The **core** (stats + ledger) is an immortal singleton in never-destructed aligned static
  storage, guarded by an atomic ready flag.
- Allocations occurring before the core is ready are counted into `Unknown` rather than
  crashing, and **frees of such pointers must be tolerated silently**.
- `MemoryService` is a thin facade over the core and is the only part registered in
  `ServiceContainer`, for panel and MCP consumption.

This is the same class of ordering hazard as the existing
`DiagnosticEngine`/`ResourceRegistry` teardown-order rule.

## Debugging features

- **Guard pages.** `MI_SECURE=ON` in Debug/Dev builds. Additionally an opt-in
  `GuardedAllocator` using `VirtualAlloc` with `PAGE_NOACCESS` pages either side and the
  allocation flushed right against the trailing guard, selectable **per tag** via settings —
  so a single suspect subsystem can be armed without paying engine-wide.
- **Fill patterns.** `0xCD` on allocate, `0xDD` on free, and on `LinearAllocator::Reset`.
  Turns use-after-free into an obviously wrong value rather than stale-but-plausible data.
- **Double-free / invalid-free detection.** Ledger lookup on every free in dev builds. An
  unknown or already-dead pointer logs with both the allocating and the freeing callstack,
  subject to the epoch rule below.
- **Leak report on shutdown.** Ledger dump grouped by tag then callstack, sorted by bytes,
  written alongside the crash bundles in `LocalAppData/.../crashes/<exe>/`, plus a per-tag
  summary line in the log.
- **Crash enrichment.** The per-tag table is pushed through `CrashHandler::SetContext`, so
  any minidump carries the memory state at the moment of death.
- **Budgets.** Soft per-tag budgets from settings. The first breach logs a warning with the
  top contributing callstacks and optionally breaks into the debugger. Fires exactly once
  per tag per run. Mirrors the existing `render.memory` pattern.

## Surfaces

1. **`MemoryPanel`** (editor) — per-tag table of current / peak / budget / live count, a
   history sparkline per tag, a managed GC section, `Snapshot` and `Diff` buttons, and a
   top-N callstacks list for the selected tag. Self-windowing with a `VisiblePtr` per the
   existing editor shell pattern. Live state is not persisted to `debug.toml`.
2. **MCP tools** in a new `src/app/editor/ControlMethodsMemory.cpp`, alongside the existing
   `ControlMethods.cpp` / `ControlMethods2D.cpp` / `ControlMethodsPixel.cpp`:
   - `memory_stats` — per-tag current/peak/live plus the managed GC block
   - `memory_snapshot` — take a named snapshot, return a handle
   - `memory_diff` — delta between two snapshots, grouped by tag and callstack

   This enables an agent-driven leak hunt: snapshot, load a scene ten times, diff, report
   what grew and never came back.
3. **Tracy** — `AE_PROFILE_ALLOC_N` and `AE_PROFILE_PLOT` already exist in `Profiler.hpp`
   but are unused. Wire a named pool per tag and a plot per tag.
4. **Log and console** — a `memory.report` command plus a shutdown summary.

## Managed (C#) heap

Native tracking is structurally blind to the CoreCLR GC heap, which is where gameplay leaks
will actually live since all game logic is C#.

`ManagedMemoryStats` is polled on the managed side from `GC.GetGCMemoryInfo()` (heap size,
committed bytes, per-generation info including LOH and POH), `GC.GetTotalAllocatedBytes()`,
and `GC.CollectionCount(0..2)`, then pushed across a new
`src/app/scripting/interop/MemoryExports.cpp` as a flat struct. It is surfaced as a
`Managed` pseudo-tag in the same editor panel and the same MCP payload.

**Constraint:** the exports file must stay runtime-safe — no `ComponentCatalog` or editor
dependencies — so `GameRuntime` links it. Note also that a brand-new `src/app/*.cpp`
requires a CMake reconfigure before the Editor target picks it up, or the managed side fails
with `EntryPointNotFoundException`.

Per-type managed object breakdown (via EventPipe or a GC heap walk) is explicitly **out of
scope** for this design.

## Cost tiers

A compile-time floor per build configuration, with runtime movement permitted inside that
floor. This tiering exists specifically because an always-on dev-build layer is what caused
the previous "fps gets worse over time" regression.

| Tier | Added cost | Compiled in | Default at runtime |
| --- | --- | --- | --- |
| `Disabled` | Zero. The override still routes to mimalloc, so the throughput win is retained. | All configurations | — |
| `Counters` | Two atomics plus a TLS read per allocation. Gives per-tag current/peak/live. | All configurations | On everywhere, including Shipping |
| `Ledger` | Sharded pointer→record map. Enables leak reports, double-free detection, snapshot diffs. | Debug / Dev only (`AE_DEV_TOOLING`) | On in Debug / Dev |
| `Callstacks` | Stack capture per allocation. The expensive tier. | Debug / Dev only (`AE_DEV_TOOLING`) | Off, even in Debug — opt-in |

The highest tier a configuration can reach is fixed at compile time: Shipping physically
cannot pay for the ledger because it is not compiled in. Within that ceiling the level moves
at runtime, so Shipping can drop to `Disabled` and Debug can escalate to `Callstacks`.

**Epoch rule.** Enabling the ledger mid-run means already-live pointers are absent from it,
so freeing them would falsely look like an invalid-free. The ledger carries an epoch serial
and reports invalid-free **only** for pointers allocated after the current epoch began.

Settings live under `memory.*` in `SettingsService` (tracking level, callstack capture,
fill patterns, guarded tag list, per-tag budgets), following the existing three-layer
cascade and its reflection-driven field registration.

## Acceptance criteria

These are measured, not assumed:

- The `Disabled` and `Counters` tiers must beat `std::malloc` on an allocation microbenchmark,
  single- and multi-threaded. mimalloc makes this likely, not certain — it must be shown.
- `Counters` overhead must be ≤5% on that microbenchmark, measured against the `Disabled`
  tier on the same build (not against `std::malloc`).
- No editor frame-rate regression, verified with `render_benchmark` and `run_gauntlet`
  before and after.
- A deliberately leaked allocation is reported at shutdown **and correctly attributed** to
  its tag and callstack.
- A deliberate double-free is detected and reported with both callstacks.

## Testing

Unit tests in EngineTests:

- Each allocator: alignment including over-aligned types, exhaustion behaviour, reset,
  LIFO marker ordering for `StackAllocator`, pool reuse and pool exhaustion.
- Tag scopes: nesting, restoration when an exception unwinds the scope, independence across
  threads.
- Ledger: insert, erase, shard distribution, epoch behaviour on mid-run enable.
- Snapshot diff arithmetic.
- Budget breach fires exactly once.
- A deliberate leak is found and correctly attributed.
- A deliberate double-free is caught.
- `GuardedAllocator` faults on a buffer overrun (death test).
- A soak test asserting the ledger does not grow unboundedly under churn.

EngineTests already forces `/INCREMENTAL:NO`, so the historic stale-ILK crash mode does not
apply.

## Risks

1. **CoreCLR interaction with the global new/delete override — highest risk.** CoreCLR uses
   its own internal allocators, so overriding `new`/`delete` in our modules should be safe,
   but this must be verified. It gets a spike at the **front** of the implementation plan,
   not discovered at the end.
2. **The allocator framework is speculative.** With no measured allocation pain, the arenas,
   pools, and tagged containers may go largely unused. Mitigation: land tracking first,
   allocators second, and let tracking data determine where `ae::vector<T, Tag>` is worth
   adopting. **No big-bang container migration** — adoption stays opt-in and incremental.
3. **Third-party attribution is follow-on, not core.** imgui
   (`ImGui::SetAllocatorFunctions`), Box2D, toml++, and Vulkan driver host allocations
   (`VkAllocationCallbacks`) each expose allocator hooks. Cheap wins, deliberately deferred.
4. **Module boundaries.** A pointer allocated in one module and freed in another must reach
   the same allocator. Static override per module, verified with `mi_is_redirected()`.
5. **Static-init ordering.** Handled by the immortal singleton, but pre-ready allocations
   land in `Unknown` and their frees must be tolerated silently.
6. **Dev-build soak cost.** The ledger must be O(1) amortized with no unbounded growth
   (sharded map with a reused free list) and must be soak-tested, precisely because the
   validation layer previously became the dominant dev-build cost.

## Out of scope

- Per-type managed object breakdown (EventPipe / GC heap walk).
- Routing third-party library allocations through tags (listed as follow-on).
- Replacing any existing engine container usage wholesale.
