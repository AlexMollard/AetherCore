# Memory Tracking & Visibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace AetherCore's untracked `std::malloc` heap with mimalloc, and build a layered CPU memory tracking system whose data is visible in the editor, queryable over MCP, and reported on shutdown.

**Architecture:** mimalloc becomes the process heap via a single override translation unit. A tag is attached to every allocation from a `thread_local` scope value. Lock-free per-tag counters run in every configuration; a sharded pointer→record ledger with deduplicated callstacks runs in dev configurations only. A facade exposes snapshots, diffs and leak reports to an editor panel, three MCP tools, and the shutdown log. Managed GC statistics are pulled across the existing scripting ABI and shown as a pseudo-tag alongside the native ones.

**Tech Stack:** C++23/26, MSVC (primary) and clang-cl (clang-tidy), CMake + CPM, mimalloc, doctest, imgui, nlohmann/json, C#/CoreCLR.

**Source spec:** `docs/superpowers/specs/2026-07-30-memory-allocator-design.md`

This plan covers **Plan 1 of 3**. Plan 2 is the allocator framework (`IAllocator`, heap/linear/frame/stack/pool, `TrackedAllocator`, containers). Plan 3 is debug hardening (guard pages, fill patterns, budgets, crash enrichment, Tracy pools). Neither is in scope here.

## Global Constraints

- C++ standard is 26 where the compiler supports it, else 23 (`CMakeLists.txt:5-9`). Do not lower it.
- Code style: tabs for indentation, Allman braces, `aether` root namespace, `m_` member prefix, `k` constant prefix, `[[nodiscard]]` on pure accessors. Match the surrounding file.
- Production compiler is MSVC; clang-tidy runs via a clang-cl compilation database. clang-strict errors are real errors, not false positives.
- No game or project code in the engine. Engine code must not depend on `src/app`.
- Managed interop translation units must stay runtime-safe: no `ComponentCatalog` and no editor dependencies, because `GameRuntime` links them.
- `src/engine` and `src/app` sources are globbed with `CONFIGURE_DEPENDS`, but a **brand-new** `src/app/*.cpp` still needs a CMake reconfigure before the Editor target sees it, or the managed side fails with `EntryPointNotFoundException`.
- Build configurations are `AE_CONFIG_DEBUG`, `AE_CONFIG_DEV`, `AE_CONFIG_RETAIL`, `AE_CONFIG_SHIP`. `AE_DEV_TOOLING` is 1 for Debug and Dev only (`src/engine/Defines.hpp:4-8`).
- Run built executables from the build-tree root, or shader loading segfaults at pipeline creation (`shaders://` is CWD-relative).
- Tests use doctest. `EngineTests` links `Engine` only.

---

## File Structure

**Created — engine tracking core (`src/engine/memory/`):**

| File | Responsibility |
| --- | --- |
| `MemoryTag.hpp` | `AE_MEMORY_TAGS(X)` list; `MemTag` enum, `kMemTagCount`, `ToString` |
| `MemoryScope.hpp` | `thread_local` current tag, `ScopedMemTag`, `AE_MEM_SCOPE(tag)` |
| `MemoryStats.hpp/.cpp` | Cache-line-isolated per-tag atomic counters; constant-initialized |
| `TrackingLevel.hpp` | `TrackingLevel` enum, compile-time ceiling, runtime current level |
| `CallstackDatabase.hpp/.cpp` | Capture, hash, dedupe stacks to ids; lazy symbol resolution |
| `AllocationLedger.hpp/.cpp` | Sharded `ptr -> AllocationRecord`, epoch rule, immortal storage |
| `MemorySnapshot.hpp` | `MemorySnapshot`, `SnapshotDiff` value types |
| `MemoryService.hpp/.cpp` | Facade: snapshot, diff, leak report, managed stats accessor |

**Modified:**

| File | Change |
| --- | --- |
| `CMake/Dependencies.cmake` | Add the mimalloc CPM package |
| `src/engine/CMakeLists.txt` | Link `mimalloc-static` |
| `src/engine/utils/MemoryTracker.cpp` | mimalloc override + tracking hooks + force-link anchor |
| `src/engine/AetherCore.cpp` | Reference the force-link anchor; register `MemoryService`; leak report on shutdown |
| `src/engine/scripting/ManagedInterop.hpp` | `ManagedMemoryStats` struct + ABI function pointer |
| `managed/AetherCore.Interop/Abi.cs` | Mirror the two ABI additions |
| `managed/AetherCore.Interop/Bootstrap.cs` | Fill the new function pointer |
| `src/app/editor/ControlMethods.hpp` | Declare `AppendMemoryMethods` |
| `src/app/editor/ControlMethods.cpp:2273` | Call `AppendMemoryMethods(methods)` |
| `src/app/layers/DebugLayer.cpp` | Register `MemoryPanel` |

**Created — surfaces:**

| File | Responsibility |
| --- | --- |
| `managed/AetherCore.Interop/ManagedMemory.cs` | `[UnmanagedCallersOnly]` GC stats reader |
| `src/app/editor/ControlMethodsMemory.cpp` | `memory_stats`, `memory_snapshot`, `memory_diff` |
| `src/app/debug/MemoryPanel.hpp/.cpp` | Editor panel |

**Created — tests (`tests/memory/`):** `MemoryTagTests.cpp`, `MemoryScopeTests.cpp`, `MemoryStatsTests.cpp`, `OverrideTrackingTests.cpp`, `CallstackDatabaseTests.cpp`, `AllocationLedgerTests.cpp`, `MemorySnapshotTests.cpp`, `AllocatorBenchmarkTests.cpp`.

---

## Task 1: mimalloc backend spike

The riskiest work, deliberately first. Two things can sink the design and both must be *observed*, not assumed: whether `operator new` defined in a static library actually replaces the CRT's, and whether CoreCLR tolerates the replacement.

**Files:**
- Modify: `CMake/Dependencies.cmake`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/utils/MemoryTracker.cpp` (replace whole file)
- Modify: `src/engine/AetherCore.cpp`
- Test: `tests/memory/OverrideTrackingTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `extern "C" void aether_memory_force_link() noexcept;` and `bool aether::memory::IsMimallocActive() noexcept;` declared in `src/engine/memory/MemoryBackend.hpp`.

- [ ] **Step 1: Add the mimalloc CPM package**

In `CMake/Dependencies.cmake`, after the VMA block (around line 65), add:

```cmake
# ── CPU allocator ─────────────────────────────────────────────────────────────
# mimalloc is the process heap. Chosen for its first-class heaps, the
# mi_heap_visit_blocks walker and MI_SECURE, which the memory tracking
# subsystem builds on; see docs/superpowers/specs/2026-07-30-memory-allocator-design.md.
CPMAddPackage(
    NAME mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.7
    GIT_SHALLOW    TRUE
    OPTIONS
        "MI_BUILD_SHARED OFF"
        "MI_BUILD_OBJECT OFF"
        "MI_BUILD_TESTS OFF"
        "MI_OVERRIDE OFF"
        "MI_SECURE $<IF:$<BOOL:${AETHERCORE_MEMORY_SECURE}>,ON,OFF>"
)
```

`MI_OVERRIDE` is deliberately **OFF**: we do the `operator new` replacement ourselves in one translation unit so the tracking hooks sit in the same place, rather than letting mimalloc patch the CRT behind our back.

Immediately above the `CPMAddPackage`, add the option it reads:

```cmake
option(AETHERCORE_MEMORY_SECURE "Build mimalloc in secure mode (guard pages, encoded free lists, double-free detection)" OFF)
```

- [ ] **Step 2: Link mimalloc into Engine**

In `src/engine/CMakeLists.txt`, next to the other `target_link_libraries(Engine ...)` calls (near line 93), add:

```cmake
target_link_libraries(Engine PUBLIC mimalloc-static)
```

`PUBLIC` matters: the override translation unit lives in Engine but the symbols must reach every executable that links it.

- [ ] **Step 3: Create the backend header**

Create `src/engine/memory/MemoryBackend.hpp`:

```cpp
#pragma once

namespace aether::memory
{
	// True when the global operator new/delete replacement in MemoryTracker.cpp is the
	// one the linker actually picked. Engine is a STATIC library, so the object file
	// holding the replacement is only pulled into the link if something references it -
	// otherwise the CRT's allocator silently stays in place and every allocation is
	// untracked. AetherCore.cpp references aether_memory_force_link() to guarantee the
	// pull, and this function is how a test or a log line confirms it worked.
	[[nodiscard]] bool IsMimallocActive() noexcept;
} // namespace aether::memory

// Link anchor. Defined in MemoryTracker.cpp purely so a reference from an
// always-linked translation unit drags that object file into the final image.
extern "C" void aether_memory_force_link() noexcept;
```

- [ ] **Step 4: Replace MemoryTracker.cpp with the mimalloc override**

Replace the entire contents of `src/engine/utils/MemoryTracker.cpp`. The `TRACY_ENABLE` gate is gone — the overrides now exist in every configuration, and Tracy becomes one optional consumer rather than the reason the file exists.

```cpp
// The single translation unit that owns the global operator new/delete replacement.
//
// Everything the process allocates through new/delete lands here, which makes this the
// one place where the allocator backend and the tracking hooks can be kept in step.
// mimalloc's own MI_OVERRIDE is deliberately off so that this file, and not a CRT
// patch, is the definition the linker resolves.

#include "memory/MemoryBackend.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#include <mimalloc.h>

#include "utils/Profiler.hpp"

extern "C" void aether_memory_force_link() noexcept
{
}

namespace aether::memory
{
	bool IsMimallocActive() noexcept
	{
		// mi_is_redirected() answers a different question - whether the CRT's malloc was
		// patched - and is false in our configuration by design. What we care about is
		// whether an allocation made through operator new came from a mimalloc heap,
		// which mi_check_owned answers directly.
		void* probe = ::operator new(32);
		const bool owned = mi_check_owned(probe);
		::operator delete(probe);
		return owned;
	}
} // namespace aether::memory

namespace
{
	void* AllocateTracked(std::size_t size) noexcept
	{
		void* ptr = mi_malloc(size);
		if (ptr != nullptr)
		{
			AE_PROFILE_ALLOC(ptr, size);
		}
		return ptr;
	}

	void* AllocateTrackedAligned(std::size_t size, std::size_t alignment) noexcept
	{
		void* ptr = mi_malloc_aligned(size, alignment);
		if (ptr != nullptr)
		{
			AE_PROFILE_ALLOC(ptr, size);
		}
		return ptr;
	}

	void FreeTracked(void* ptr) noexcept
	{
		if (ptr == nullptr)
		{
			return;
		}
		AE_PROFILE_FREE(ptr);
		mi_free(ptr);
	}
} // namespace

void* operator new(std::size_t size)
{
	void* ptr = AllocateTracked(size);
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	return ptr;
}

void* operator new[](std::size_t size)
{
	void* ptr = AllocateTracked(size);
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	return AllocateTracked(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	return AllocateTracked(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	void* ptr = AllocateTrackedAligned(size, static_cast<std::size_t>(alignment));
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	return ptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	void* ptr = AllocateTrackedAligned(size, static_cast<std::size_t>(alignment));
	if (ptr == nullptr)
	{
		throw std::bad_alloc{};
	}
	return ptr;
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	return AllocateTrackedAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
	return AllocateTrackedAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr) noexcept
{
	FreeTracked(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
	FreeTracked(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
	FreeTracked(ptr);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
	FreeTracked(ptr);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
	FreeTracked(ptr);
}
```

The full aligned and sized overload set is mandatory. Missing one means that overload keeps the CRT's implementation, so a pointer from `mi_malloc` reaches `free` — a heap mismatch that usually crashes far from the cause.

- [ ] **Step 5: Anchor the link and log the backend**

In `src/engine/AetherCore.cpp`, add the include near the other engine includes:

```cpp
#include "memory/MemoryBackend.hpp"
```

Then in the engine's initialization function, as one of the first statements, add:

```cpp
	// Drags MemoryTracker.obj into the link so its operator new/delete replacement is
	// the definition the linker resolves. Without this reference the CRT allocator wins
	// silently and nothing is tracked.
	aether_memory_force_link();
	AE_INFO(LogCategory::Engine, "CPU allocator: {}", aether::memory::IsMimallocActive() ? "mimalloc" : "system CRT (override NOT active)");
```

Find the initialization function by searching for where the logger is first available; place this immediately after logging is up so the line is captured.

- [ ] **Step 6: Write the failing test**

Create `tests/memory/OverrideTrackingTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "memory/MemoryBackend.hpp"

using namespace aether;

TEST_CASE("The mimalloc operator new replacement is the one the linker picked") {
	// If this fails, Engine's MemoryTracker.obj was not pulled into the test binary and
	// the CRT allocator is in charge. Everything else in the memory subsystem is built
	// on this being true.
	CHECK(memory::IsMimallocActive());
}

TEST_CASE("Allocations survive a round trip through every operator new overload") {
	auto* plain = new int(7);
	CHECK(*plain == 7);
	delete plain;

	auto* array = new int[64];
	array[63] = 11;
	CHECK(array[63] == 11);
	delete[] array;

	struct alignas(64) OverAligned
	{
		int value;
	};

	auto* aligned = new OverAligned{3};
	CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
	CHECK(aligned->value == 3);
	delete aligned;

	auto* nothrown = new (std::nothrow) int(5);
	REQUIRE(nothrown != nullptr);
	CHECK(*nothrown == 5);
	::operator delete(nothrown, std::nothrow);

	// Containers and smart pointers route through the same overloads.
	std::vector<int> values(1024, 2);
	CHECK(values[1023] == 2);
	auto owned = std::make_unique<std::vector<int>>(512, 9);
	CHECK((*owned)[511] == 9);
}
```

- [ ] **Step 7: Reconfigure, build and run the test**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests
```

Then from the build-tree root:

```bash
./build-ninja-clang/EngineTests --test-case="*mimalloc operator new replacement*" -s
```

Expected: PASS. If `IsMimallocActive()` returns false, the link anchor did not work — do not proceed. Fix it by adding `MemoryTracker.cpp` directly to each executable target's sources in `src/app/CMakeLists.txt` and `tests/CMakeLists.txt` instead of relying on the static library, then re-run.

If instead the build fails with `LNK2005` duplicate `operator new`, that is the other failure mode: add `/FORCE:MULTIPLE` **only** as a diagnostic to confirm the cause, then resolve properly by moving the override TU into the executables as above. Record whichever resolution was needed in a comment at the top of `MemoryTracker.cpp`.

- [ ] **Step 8: Verify CoreCLR tolerates the replacement**

This is the spike's real question. Build and run the Editor:

```bash
cmake --build build-vs2022-msvc --config Debug --target AetherCoreEditor
```

From the build-tree root, launch the editor, open the INKBOUND project, load a scene with scripts, and enter Play mode. Confirm from the log that scripts attach and update, and that the `CPU allocator: mimalloc` line is present.

Then run the automated gauntlet, which exercises scene load, play, and teardown:

```bash
aether-ctl run_gauntlet
```

Expected: the gauntlet passes and no allocator-related crash appears. If CoreCLR faults, capture the crash bundle from `LocalAppData/.../crashes/` and stop — the spec's highest risk has materialized and the design needs revisiting before any further task.

- [ ] **Step 9: Commit**

```bash
git add CMake/Dependencies.cmake src/engine/CMakeLists.txt src/engine/memory/MemoryBackend.hpp src/engine/utils/MemoryTracker.cpp src/engine/AetherCore.cpp tests/memory/OverrideTrackingTests.cpp
git commit -m "Route engine allocations through mimalloc

- Add mimalloc as a CPM dependency with MI_OVERRIDE off
- Replace the malloc-backed operator new/delete set with mimalloc, including every aligned and sized overload
- Anchor the override translation unit into the link and log which allocator won
- Cover the override with a test that fails if the CRT allocator is still in charge"
```

---

## Task 2: Memory tags and the scope stack

**Files:**
- Create: `src/engine/memory/MemoryTag.hpp`
- Create: `src/engine/memory/MemoryScope.hpp`
- Test: `tests/memory/MemoryTagTests.cpp`, `tests/memory/MemoryScopeTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `aether::memory::MemTag` (enum class, `std::uint8_t`), `kMemTagCount`, `ToString(MemTag) -> const char*`, `CurrentTag() -> MemTag`, `ScopedMemTag`, macro `AE_MEM_SCOPE(tag)`. `Managed` is deliberately **not** a member of `MemTag` — managed memory is not a native allocation and is surfaced separately in Task 9.

- [ ] **Step 1: Write the failing tests**

Create `tests/memory/MemoryTagTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <cstring>
#include <set>
#include <string>

#include "memory/MemoryTag.hpp"

using namespace aether::memory;

TEST_CASE("Every tag has a distinct, non-empty name") {
	std::set<std::string> names;
	for (std::size_t i = 0; i < kMemTagCount; ++i) {
		const char* name = ToString(static_cast<MemTag>(i));
		REQUIRE(name != nullptr);
		CHECK(std::strlen(name) > 0);
		CHECK(names.insert(name).second);
	}
	CHECK(names.size() == kMemTagCount);
}

TEST_CASE("Tag zero is Unknown so a default-initialised tag is honest") {
	CHECK(static_cast<MemTag>(0) == MemTag::Unknown);
	CHECK(std::strcmp(ToString(MemTag::Unknown), "Unknown") == 0);
}

TEST_CASE("Out-of-range tags do not read past the name table") {
	CHECK(std::strcmp(ToString(MemTag::Count), "Invalid") == 0);
	CHECK(std::strcmp(ToString(static_cast<MemTag>(200)), "Invalid") == 0);
}
```

Create `tests/memory/MemoryScopeTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <thread>

#include "memory/MemoryScope.hpp"

using namespace aether::memory;

TEST_CASE("The default tag is Unknown") {
	CHECK(CurrentTag() == MemTag::Unknown);
}

TEST_CASE("A scope sets the tag and restores the previous one") {
	CHECK(CurrentTag() == MemTag::Unknown);
	{
		AE_MEM_SCOPE(MemTag::Physics2D);
		CHECK(CurrentTag() == MemTag::Physics2D);
	}
	CHECK(CurrentTag() == MemTag::Unknown);
}

TEST_CASE("Scopes nest and unwind in order") {
	AE_MEM_SCOPE(MemTag::Rendering);
	CHECK(CurrentTag() == MemTag::Rendering);
	{
		AE_MEM_SCOPE(MemTag::Mesh);
		CHECK(CurrentTag() == MemTag::Mesh);
		{
			AE_MEM_SCOPE(MemTag::Texture);
			CHECK(CurrentTag() == MemTag::Texture);
		}
		CHECK(CurrentTag() == MemTag::Mesh);
	}
	CHECK(CurrentTag() == MemTag::Rendering);
}

TEST_CASE("An exception unwinding a scope still restores the tag") {
	CHECK(CurrentTag() == MemTag::Unknown);
	try {
		AE_MEM_SCOPE(MemTag::Scripting);
		CHECK(CurrentTag() == MemTag::Scripting);
		throw std::runtime_error("unwind");
	} catch (const std::runtime_error&) {
	}
	CHECK(CurrentTag() == MemTag::Unknown);
}

TEST_CASE("Two scopes on the same line get distinct variable names") {
	// Guards against a __LINE__-based macro colliding with itself.
	AE_MEM_SCOPE(MemTag::Ui); { AE_MEM_SCOPE(MemTag::Editor); CHECK(CurrentTag() == MemTag::Editor); }
	CHECK(CurrentTag() == MemTag::Ui);
}

TEST_CASE("Each thread carries its own tag") {
	AE_MEM_SCOPE(MemTag::Net);
	std::atomic<bool> workerSawOwnTag{false};
	std::atomic<bool> workerSawLeakedTag{false};

	std::thread worker([&] {
		workerSawLeakedTag = CurrentTag() == MemTag::Net;
		AE_MEM_SCOPE(MemTag::Audio);
		workerSawOwnTag = CurrentTag() == MemTag::Audio;
	});
	worker.join();

	CHECK(workerSawOwnTag.load());
	CHECK_FALSE(workerSawLeakedTag.load());
	CHECK(CurrentTag() == MemTag::Net);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build-ninja-clang --target EngineTests
```

Expected: compilation failure — `memory/MemoryTag.hpp` and `memory/MemoryScope.hpp` do not exist.

- [ ] **Step 3: Implement MemoryTag.hpp**

Create `src/engine/memory/MemoryTag.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace aether::memory
{
	// The single tag list. Adding a subsystem is one line here: the enum, the count and
	// the name table all derive from this macro, so they cannot drift apart.
	//
	// Unknown must stay first so that a zero-initialised tag reports honestly rather
	// than misattributing to whichever subsystem happened to be listed first.
	//
	// There is no Managed tag: the C# GC heap is not a native allocation and is
	// surfaced as a separate pseudo-tag by MemoryService.
#define AE_MEMORY_TAGS(X) \
	X(Unknown)            \
	X(Engine)             \
	X(Rendering)          \
	X(RenderGraph)        \
	X(Vulkan)             \
	X(Mesh)               \
	X(Texture)            \
	X(Material)           \
	X(Shader)             \
	X(Scene)              \
	X(Ecs)                \
	X(Physics3D)          \
	X(Physics2D)          \
	X(Animation)          \
	X(Audio)              \
	X(Scripting)          \
	X(Ui)                 \
	X(Editor)             \
	X(Assets)             \
	X(Io)                 \
	X(Net)                \
	X(Particles)          \
	X(Tilemap)            \
	X(Temp)               \
	X(ThirdParty)

	enum class MemTag : std::uint8_t
	{
#define AE_MEMORY_TAG_ENUM_ENTRY(name) name,
		AE_MEMORY_TAGS(AE_MEMORY_TAG_ENUM_ENTRY)
#undef AE_MEMORY_TAG_ENUM_ENTRY
		        Count
	};

	inline constexpr std::size_t kMemTagCount = static_cast<std::size_t>(MemTag::Count);

	[[nodiscard]] constexpr const char* ToString(MemTag tag) noexcept
	{
		constexpr const char* kNames[] = {
#define AE_MEMORY_TAG_NAME_ENTRY(name) #name,
		        AE_MEMORY_TAGS(AE_MEMORY_TAG_NAME_ENTRY)
#undef AE_MEMORY_TAG_NAME_ENTRY
		};
		static_assert(sizeof(kNames) / sizeof(kNames[0]) == kMemTagCount);

		const auto index = static_cast<std::size_t>(tag);
		return index < kMemTagCount ? kNames[index] : "Invalid";
	}
} // namespace aether::memory
```

- [ ] **Step 4: Implement MemoryScope.hpp**

Create `src/engine/memory/MemoryScope.hpp`:

```cpp
#pragma once

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	namespace detail
	{
		// Read on every tracked allocation, so it is a bare thread_local: no lock, no
		// container, no allocation of its own. Constant-initialised, so it is valid
		// during static initialisation before anything has had a chance to set it.
		inline thread_local MemTag t_currentTag = MemTag::Unknown;
	} // namespace detail

	[[nodiscard]] inline MemTag CurrentTag() noexcept
	{
		return detail::t_currentTag;
	}

	// RAII tag override. The C++ call stack *is* the tag stack - the previous tag is
	// stored in this object and restored by its destructor, which is why exception
	// unwinding gets correct behaviour for free and why no heap allocation is involved.
	class ScopedMemTag
	{
	public:
		explicit ScopedMemTag(MemTag tag) noexcept
		        : m_previous(detail::t_currentTag)
		{
			detail::t_currentTag = tag;
		}

		~ScopedMemTag() noexcept
		{
			detail::t_currentTag = m_previous;
		}

		ScopedMemTag(const ScopedMemTag&) = delete;
		ScopedMemTag& operator=(const ScopedMemTag&) = delete;
		ScopedMemTag(ScopedMemTag&&) = delete;
		ScopedMemTag& operator=(ScopedMemTag&&) = delete;

	private:
		MemTag m_previous;
	};
} // namespace aether::memory

#define AE_MEM_SCOPE_CONCAT_INNER(a, b) a##b
#define AE_MEM_SCOPE_CONCAT(a, b) AE_MEM_SCOPE_CONCAT_INNER(a, b)

// Attributes every allocation made in the enclosing scope, on this thread, to `tag` -
// including allocations made by std containers and third-party code called from here.
#define AE_MEM_SCOPE(tag) const ::aether::memory::ScopedMemTag AE_MEM_SCOPE_CONCAT(aeMemScope_, __COUNTER__)(tag)
```

`__COUNTER__` rather than `__LINE__`, so two scopes on one line do not collide.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests && ./build-ninja-clang/EngineTests --test-case="*tag*,*scope*" -s
```

Expected: all cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/engine/memory/MemoryTag.hpp src/engine/memory/MemoryScope.hpp tests/memory/MemoryTagTests.cpp tests/memory/MemoryScopeTests.cpp
git commit -m "Add memory tags and the thread-local tag scope

- Derive the MemTag enum, count and name table from one AE_MEMORY_TAGS list
- Add AE_MEM_SCOPE, whose RAII object makes the call stack the tag stack
- Cover nesting, exception unwinding, same-line scopes and per-thread isolation"
```

---

## Task 3: Per-tag atomic counters

**Files:**
- Create: `src/engine/memory/MemoryStats.hpp`, `src/engine/memory/MemoryStats.cpp`
- Test: `tests/memory/MemoryStatsTests.cpp`

**Interfaces:**
- Consumes: `MemTag`, `kMemTagCount` from Task 2.
- Produces: `struct TagTotals { std::uint64_t currentBytes, peakBytes, totalAllocations, totalFrees; std::uint64_t LiveCount() const noexcept; }`; `class MemoryStats` with `RecordAllocation(MemTag, std::size_t)`, `RecordFree(MemTag, std::size_t)`, `Get(MemTag) const -> TagTotals`, `Total() const -> TagTotals`, `ResetPeaks()`; and `MemoryStats& GlobalStats() noexcept`.

Note a deliberate deviation from the spec's "two atomics" phrasing: the block is three relaxed atomic operations on allocation (bytes, count, and a peak compare-exchange that almost always no-ops) and two on free. `liveCount` is *derived* as `totalAllocations - totalFrees` rather than stored, which removes one. The binding constraint is the ≤5% budget verified in Task 14, not the operation count.

- [ ] **Step 1: Write the failing test**

Create `tests/memory/MemoryStatsTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "memory/MemoryStats.hpp"

using namespace aether::memory;

TEST_CASE("A fresh stats block is entirely zero") {
	MemoryStats stats;
	for (std::size_t i = 0; i < kMemTagCount; ++i) {
		const TagTotals totals = stats.Get(static_cast<MemTag>(i));
		CHECK(totals.currentBytes == 0);
		CHECK(totals.peakBytes == 0);
		CHECK(totals.totalAllocations == 0);
		CHECK(totals.totalFrees == 0);
		CHECK(totals.LiveCount() == 0);
	}
}

TEST_CASE("Allocation and free move current bytes and derive the live count") {
	MemoryStats stats;
	stats.RecordAllocation(MemTag::Mesh, 100);
	stats.RecordAllocation(MemTag::Mesh, 40);

	TagTotals mesh = stats.Get(MemTag::Mesh);
	CHECK(mesh.currentBytes == 140);
	CHECK(mesh.totalAllocations == 2);
	CHECK(mesh.LiveCount() == 2);

	stats.RecordFree(MemTag::Mesh, 100);
	mesh = stats.Get(MemTag::Mesh);
	CHECK(mesh.currentBytes == 40);
	CHECK(mesh.totalFrees == 1);
	CHECK(mesh.LiveCount() == 1);
}

TEST_CASE("Peak records the high-water mark, not the current value") {
	MemoryStats stats;
	stats.RecordAllocation(MemTag::Texture, 1000);
	stats.RecordFree(MemTag::Texture, 900);
	stats.RecordAllocation(MemTag::Texture, 50);

	const TagTotals totals = stats.Get(MemTag::Texture);
	CHECK(totals.currentBytes == 150);
	CHECK(totals.peakBytes == 1000);
}

TEST_CASE("Tags are independent") {
	MemoryStats stats;
	stats.RecordAllocation(MemTag::Physics2D, 64);
	stats.RecordAllocation(MemTag::Audio, 8);

	CHECK(stats.Get(MemTag::Physics2D).currentBytes == 64);
	CHECK(stats.Get(MemTag::Audio).currentBytes == 8);
	CHECK(stats.Get(MemTag::Ui).currentBytes == 0);
}

TEST_CASE("Total sums every tag") {
	MemoryStats stats;
	stats.RecordAllocation(MemTag::Scene, 10);
	stats.RecordAllocation(MemTag::Net, 20);
	stats.RecordFree(MemTag::Scene, 10);

	const TagTotals total = stats.Total();
	CHECK(total.currentBytes == 20);
	CHECK(total.totalAllocations == 2);
	CHECK(total.totalFrees == 1);
	CHECK(total.LiveCount() == 1);
}

TEST_CASE("ResetPeaks clears peaks without disturbing current bytes") {
	MemoryStats stats;
	stats.RecordAllocation(MemTag::Assets, 500);
	stats.RecordFree(MemTag::Assets, 400);
	stats.ResetPeaks();

	const TagTotals totals = stats.Get(MemTag::Assets);
	CHECK(totals.currentBytes == 100);
	CHECK(totals.peakBytes == 100);
}

TEST_CASE("Concurrent recording loses no bytes and no counts") {
	MemoryStats stats;
	constexpr int kThreads = 8;
	constexpr int kPerThread = 20000;

	std::vector<std::thread> workers;
	workers.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		workers.emplace_back([&stats] {
			for (int i = 0; i < kPerThread; ++i) {
				stats.RecordAllocation(MemTag::Temp, 16);
			}
			for (int i = 0; i < kPerThread; ++i) {
				stats.RecordFree(MemTag::Temp, 16);
			}
		});
	}
	for (auto& worker : workers) {
		worker.join();
	}

	const TagTotals totals = stats.Get(MemTag::Temp);
	CHECK(totals.currentBytes == 0);
	CHECK(totals.totalAllocations == static_cast<std::uint64_t>(kThreads) * kPerThread);
	CHECK(totals.totalFrees == static_cast<std::uint64_t>(kThreads) * kPerThread);
	CHECK(totals.peakBytes > 0);
	CHECK(totals.peakBytes <= static_cast<std::uint64_t>(kThreads) * kPerThread * 16);
}

TEST_CASE("The global stats block is constant-initialised and usable immediately") {
	// No init-order dependency: it must be valid the first time anyone touches it.
	const std::uint64_t before = GlobalStats().Get(MemTag::Engine).totalAllocations;
	GlobalStats().RecordAllocation(MemTag::Engine, 1);
	CHECK(GlobalStats().Get(MemTag::Engine).totalAllocations == before + 1);
	GlobalStats().RecordFree(MemTag::Engine, 1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build-ninja-clang --target EngineTests
```

Expected: compilation failure — `memory/MemoryStats.hpp` does not exist.

- [ ] **Step 3: Implement MemoryStats.hpp**

Create `src/engine/memory/MemoryStats.hpp`:

```cpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	struct TagTotals
	{
		std::uint64_t currentBytes = 0;
		std::uint64_t peakBytes = 0;
		std::uint64_t totalAllocations = 0;
		std::uint64_t totalFrees = 0;

		// Derived rather than stored, so the allocation path pays one atomic less.
		[[nodiscard]] std::uint64_t LiveCount() const noexcept
		{
			return totalAllocations >= totalFrees ? totalAllocations - totalFrees : 0;
		}
	};

	// Lock-free per-tag byte and count totals. Each tag's counters occupy their own
	// cache line, so threads allocating under different tags never contend.
	//
	// Trivially default-constructible and trivially destructible on purpose: the global
	// instance is constant-initialised, which is what lets operator new touch it during
	// static initialisation and after main returns without any ordering hazard.
	class MemoryStats
	{
	public:
		void RecordAllocation(MemTag tag, std::size_t size) noexcept;
		void RecordFree(MemTag tag, std::size_t size) noexcept;

		[[nodiscard]] TagTotals Get(MemTag tag) const noexcept;
		[[nodiscard]] TagTotals Total() const noexcept;

		void ResetPeaks() noexcept;

	private:
		struct alignas(std::hardware_destructive_interference_size) Counters
		{
			std::atomic<std::uint64_t> currentBytes{0};
			std::atomic<std::uint64_t> peakBytes{0};
			std::atomic<std::uint64_t> totalAllocations{0};
			std::atomic<std::uint64_t> totalFrees{0};
		};

		std::array<Counters, kMemTagCount> m_counters{};
	};

	static_assert(std::is_trivially_destructible_v<MemoryStats>, "MemoryStats must never need destruction: the global instance outlives main.");

	// The process-wide counters. Reached from operator new, so it must be valid at any
	// point in the program's life.
	[[nodiscard]] MemoryStats& GlobalStats() noexcept;
} // namespace aether::memory
```

- [ ] **Step 4: Implement MemoryStats.cpp**

Create `src/engine/memory/MemoryStats.cpp`:

```cpp
#include "memory/MemoryStats.hpp"

namespace aether::memory
{
	namespace
	{
		// constinit is the whole point: it forces a compile error if this ever stops
		// being constant-initialised, which would reintroduce a static-init-order
		// hazard on the allocation path.
		constinit MemoryStats g_globalStats;
	} // namespace

	MemoryStats& GlobalStats() noexcept
	{
		return g_globalStats;
	}

	void MemoryStats::RecordAllocation(MemTag tag, std::size_t size) noexcept
	{
		Counters& counters = m_counters[static_cast<std::size_t>(tag)];
		const std::uint64_t updated = counters.currentBytes.fetch_add(size, std::memory_order_relaxed) + size;
		counters.totalAllocations.fetch_add(1, std::memory_order_relaxed);

		// Monotonic maximum. Relaxed ordering is sufficient - the peak is a diagnostic
		// and the loop only ever raises it. Under no contention this is a single load
		// plus a failed comparison, so the common case costs almost nothing.
		std::uint64_t peak = counters.peakBytes.load(std::memory_order_relaxed);
		while (peak < updated && !counters.peakBytes.compare_exchange_weak(peak, updated, std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

	void MemoryStats::RecordFree(MemTag tag, std::size_t size) noexcept
	{
		Counters& counters = m_counters[static_cast<std::size_t>(tag)];
		counters.currentBytes.fetch_sub(size, std::memory_order_relaxed);
		counters.totalFrees.fetch_add(1, std::memory_order_relaxed);
	}

	TagTotals MemoryStats::Get(MemTag tag) const noexcept
	{
		const Counters& counters = m_counters[static_cast<std::size_t>(tag)];
		return TagTotals{counters.currentBytes.load(std::memory_order_relaxed),
		        counters.peakBytes.load(std::memory_order_relaxed),
		        counters.totalAllocations.load(std::memory_order_relaxed),
		        counters.totalFrees.load(std::memory_order_relaxed)};
	}

	TagTotals MemoryStats::Total() const noexcept
	{
		TagTotals total;
		for (std::size_t i = 0; i < kMemTagCount; ++i)
		{
			const TagTotals tag = Get(static_cast<MemTag>(i));
			total.currentBytes += tag.currentBytes;
			total.peakBytes += tag.peakBytes;
			total.totalAllocations += tag.totalAllocations;
			total.totalFrees += tag.totalFrees;
		}
		return total;
	}

	void MemoryStats::ResetPeaks() noexcept
	{
		for (Counters& counters : m_counters)
		{
			counters.peakBytes.store(counters.currentBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
	}
} // namespace aether::memory
```

`Get` is deliberately not a consistent snapshot across its four fields — under concurrent mutation the values can be very slightly out of step. That is the correct trade for a diagnostic read that must not slow the allocation path, and it is why the concurrency test asserts a range for `peakBytes` rather than an exact value.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests && ./build-ninja-clang/EngineTests --test-case="*stats*,*counters*,*tag*" -s
```

Expected: all cases PASS. The concurrency case should complete in well under a second.

- [ ] **Step 6: Commit**

```bash
git add src/engine/memory/MemoryStats.hpp src/engine/memory/MemoryStats.cpp tests/memory/MemoryStatsTests.cpp
git commit -m "Add lock-free per-tag memory counters

- Isolate each tag's counters on their own cache line to avoid false sharing
- Derive the live count from allocation and free totals to save an atomic
- Constant-initialise the global block so operator new can use it during static init
- Cover peak tracking, tag independence and concurrent recording"
```

---

## Task 4: Tracking levels with a compile-time ceiling

**Files:**
- Create: `src/engine/memory/TrackingLevel.hpp`, `src/engine/memory/TrackingLevel.cpp`
- Test: `tests/memory/TrackingLevelTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class TrackingLevel : std::uint8_t { Disabled, Counters, Ledger, Callstacks }`; `constexpr TrackingLevel kMaxTrackingLevel`; `TrackingLevel CurrentLevel() noexcept`; `void SetTrackingLevel(TrackingLevel) noexcept` (clamps to the ceiling and returns nothing); `bool LevelAtLeast(TrackingLevel) noexcept`; `ToString(TrackingLevel) -> const char*`; `ParseTrackingLevel(std::string_view) -> std::optional<TrackingLevel>`.

- [ ] **Step 1: Write the failing test**

Create `tests/memory/TrackingLevelTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <string_view>

#include "Defines.hpp"
#include "memory/TrackingLevel.hpp"

using namespace aether::memory;

namespace
{
	struct LevelGuard
	{
		TrackingLevel previous = CurrentLevel();
		~LevelGuard() { SetTrackingLevel(previous); }
	};
} // namespace

TEST_CASE("Levels are ordered so comparisons mean what they read like") {
	CHECK(TrackingLevel::Disabled < TrackingLevel::Counters);
	CHECK(TrackingLevel::Counters < TrackingLevel::Ledger);
	CHECK(TrackingLevel::Ledger < TrackingLevel::Callstacks);
}

TEST_CASE("The compile-time ceiling matches the build configuration") {
#if AE_DEV_TOOLING
	CHECK(kMaxTrackingLevel == TrackingLevel::Callstacks);
#else
	CHECK(kMaxTrackingLevel == TrackingLevel::Counters);
#endif
}

TEST_CASE("Setting a level below the ceiling takes effect exactly") {
	LevelGuard guard;
	SetTrackingLevel(TrackingLevel::Disabled);
	CHECK(CurrentLevel() == TrackingLevel::Disabled);
	SetTrackingLevel(TrackingLevel::Counters);
	CHECK(CurrentLevel() == TrackingLevel::Counters);
}

TEST_CASE("Requesting a level above the ceiling clamps instead of lying") {
	LevelGuard guard;
	SetTrackingLevel(TrackingLevel::Callstacks);
	CHECK(CurrentLevel() == kMaxTrackingLevel);
	CHECK(CurrentLevel() <= kMaxTrackingLevel);
}

TEST_CASE("LevelAtLeast reflects the active level") {
	LevelGuard guard;
	SetTrackingLevel(TrackingLevel::Counters);
	CHECK(LevelAtLeast(TrackingLevel::Disabled));
	CHECK(LevelAtLeast(TrackingLevel::Counters));
	CHECK_FALSE(LevelAtLeast(TrackingLevel::Callstacks));

	SetTrackingLevel(TrackingLevel::Disabled);
	CHECK(LevelAtLeast(TrackingLevel::Disabled));
	CHECK_FALSE(LevelAtLeast(TrackingLevel::Counters));
}

TEST_CASE("Level names round-trip through parsing") {
	for (auto level : {TrackingLevel::Disabled, TrackingLevel::Counters, TrackingLevel::Ledger, TrackingLevel::Callstacks}) {
		const auto parsed = ParseTrackingLevel(ToString(level));
		REQUIRE(parsed.has_value());
		CHECK(*parsed == level);
	}
}

TEST_CASE("Parsing is case-insensitive and rejects nonsense") {
	CHECK(ParseTrackingLevel("counters") == TrackingLevel::Counters);
	CHECK(ParseTrackingLevel("LEDGER") == TrackingLevel::Ledger);
	CHECK_FALSE(ParseTrackingLevel("everything").has_value());
	CHECK_FALSE(ParseTrackingLevel("").has_value());
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build-ninja-clang --target EngineTests
```

Expected: compilation failure — `memory/TrackingLevel.hpp` does not exist.

- [ ] **Step 3: Implement TrackingLevel.hpp**

Create `src/engine/memory/TrackingLevel.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

#include "Defines.hpp"

namespace aether::memory
{
	// How much the tracker does per allocation. Ordered from cheapest to most
	// expensive, so `>=` comparisons read the way they mean.
	enum class TrackingLevel : std::uint8_t
	{
		// Nothing is recorded. The mimalloc backend is still in use, so the throughput
		// win survives even with tracking entirely off.
		Disabled = 0,
		// Per-tag byte and count totals. Cheap enough to leave on in shipped builds.
		Counters = 1,
		// Adds the pointer-to-record ledger: leak reports, double-free detection, diffs.
		Ledger = 2,
		// Adds a captured callstack per allocation. The expensive tier.
		Callstacks = 3,
	};

	// The highest level this build can reach. Anything above it is not merely disabled
	// at runtime - it is not compiled in, so a shipped game physically cannot pay for
	// the ledger no matter what a settings file asks for.
	inline constexpr TrackingLevel kMaxTrackingLevel =
#if AE_DEV_TOOLING
	        TrackingLevel::Callstacks;
#else
	        TrackingLevel::Counters;
#endif

	namespace detail
	{
		// Read on every allocation. Relaxed atomic rather than a plain bool because the
		// level can change from another thread while allocations are in flight; the
		// worst case is one allocation observing the old level, which is harmless.
		inline std::atomic<TrackingLevel> g_level{TrackingLevel::Disabled};
	} // namespace detail

	[[nodiscard]] inline TrackingLevel CurrentLevel() noexcept
	{
		return detail::g_level.load(std::memory_order_relaxed);
	}

	[[nodiscard]] inline bool LevelAtLeast(TrackingLevel level) noexcept
	{
		return CurrentLevel() >= level;
	}

	// Clamps to kMaxTrackingLevel rather than honouring an impossible request, so a
	// caller can never believe it enabled a tier that is not compiled in.
	inline void SetTrackingLevel(TrackingLevel level) noexcept
	{
		detail::g_level.store(level > kMaxTrackingLevel ? kMaxTrackingLevel : level, std::memory_order_relaxed);
	}

	[[nodiscard]] const char* ToString(TrackingLevel level) noexcept;

	[[nodiscard]] std::optional<TrackingLevel> ParseTrackingLevel(std::string_view text) noexcept;
} // namespace aether::memory
```

`detail::g_level` is an `inline` variable with constant initialization, so like the counters it is valid before any dynamic initialization runs.

- [ ] **Step 4: Implement TrackingLevel.cpp**

Create `src/engine/memory/TrackingLevel.cpp`:

```cpp
#include "memory/TrackingLevel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace aether::memory
{
	namespace
	{
		constexpr std::array<std::pair<TrackingLevel, std::string_view>, 4> kLevelNames{{
		        {TrackingLevel::Disabled, "Disabled"},
		        {TrackingLevel::Counters, "Counters"},
		        {TrackingLevel::Ledger, "Ledger"},
		        {TrackingLevel::Callstacks, "Callstacks"},
		}};
	} // namespace

	const char* ToString(TrackingLevel level) noexcept
	{
		for (const auto& [value, name] : kLevelNames)
		{
			if (value == level)
			{
				return name.data();
			}
		}
		return "Unknown";
	}

	std::optional<TrackingLevel> ParseTrackingLevel(std::string_view text) noexcept
	{
		if (text.empty())
		{
			return std::nullopt;
		}

		std::string lowered;
		lowered.reserve(text.size());
		for (const char c : text)
		{
			lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}

		for (const auto& [value, name] : kLevelNames)
		{
			std::string candidate;
			candidate.reserve(name.size());
			for (const char c : name)
			{
				candidate.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			if (candidate == lowered)
			{
				return value;
			}
		}
		return std::nullopt;
	}
} // namespace aether::memory
```

`kLevelNames` uses `std::string_view` whose `.data()` is guaranteed null-terminated here because every entry is a string literal.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests && ./build-ninja-clang/EngineTests --test-case="*level*" -s
```

Expected: all cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/engine/memory/TrackingLevel.hpp src/engine/memory/TrackingLevel.cpp tests/memory/TrackingLevelTests.cpp
git commit -m "Add memory tracking levels with a compile-time ceiling

- Order the levels so at-least comparisons read naturally
- Fix the ceiling per build configuration so shipped builds cannot compile in the ledger
- Clamp rather than honour a request above the ceiling
- Add case-insensitive parsing for settings and command-line use"
```

---

## Task 5: Wire the counters into the override

The point where tracking goes live. Two hazards are handled here: the tracker must not track its own bookkeeping, and `operator delete` must recover the tag and size that `operator new` recorded.

Sizes are recovered from mimalloc via `mi_usable_size(ptr)` rather than stored by us, and the tag is stored in a small side-table only when the ledger is active. At the `Counters` level there is nowhere to keep a per-pointer tag, so frees are attributed to the **freeing** thread's current tag. That is a real limitation with a real consequence — a buffer allocated under `Mesh` and freed under `Unknown` leaves `Mesh` permanently inflated. Rather than ship that, `Counters` records byte totals against the allocating tag and, on free, subtracts from a compact tag stored in a header-free side map keyed by pointer. See Step 3 for the mechanism.

**Files:**
- Modify: `src/engine/utils/MemoryTracker.cpp`
- Create: `src/engine/memory/TagTable.hpp`, `src/engine/memory/TagTable.cpp`
- Test: `tests/memory/TagTableTests.cpp`, extend `tests/memory/OverrideTrackingTests.cpp`

**Interfaces:**
- Consumes: `MemTag`, `CurrentTag()`, `GlobalStats()`, `LevelAtLeast`, `TrackingLevel`.
- Produces: `aether::memory::TagTable` with `void Insert(const void*, MemTag) noexcept`, `std::optional<MemTag> Take(const void*) noexcept`, `void Clear() noexcept`, `std::size_t Size() const noexcept`; plus `TagTable& GlobalTagTable() noexcept`. Also `bool InTracker() noexcept` and `class TrackerReentryGuard` in `src/engine/memory/TrackerReentry.hpp`.

- [ ] **Step 1: Write the failing tests**

Create `tests/memory/TagTableTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "memory/TagTable.hpp"

using namespace aether::memory;

TEST_CASE("A tag survives insert and is removed by take") {
	TagTable table;
	int object = 0;
	table.Insert(&object, MemTag::Mesh);
	CHECK(table.Size() == 1);

	const auto taken = table.Take(&object);
	REQUIRE(taken.has_value());
	CHECK(*taken == MemTag::Mesh);
	CHECK(table.Size() == 0);
}

TEST_CASE("Taking an unknown pointer yields nothing rather than a wrong tag") {
	TagTable table;
	int object = 0;
	CHECK_FALSE(table.Take(&object).has_value());
}

TEST_CASE("Taking twice yields nothing the second time") {
	TagTable table;
	int object = 0;
	table.Insert(&object, MemTag::Ui);
	CHECK(table.Take(&object).has_value());
	CHECK_FALSE(table.Take(&object).has_value());
}

TEST_CASE("A null pointer is ignored on both sides") {
	TagTable table;
	table.Insert(nullptr, MemTag::Ui);
	CHECK(table.Size() == 0);
	CHECK_FALSE(table.Take(nullptr).has_value());
}

TEST_CASE("Re-inserting the same pointer overwrites rather than duplicating") {
	TagTable table;
	int object = 0;
	table.Insert(&object, MemTag::Mesh);
	table.Insert(&object, MemTag::Texture);
	CHECK(table.Size() == 1);
	CHECK(table.Take(&object) == MemTag::Texture);
}

TEST_CASE("Many pointers across threads all come back with the tag they went in with") {
	TagTable table;
	constexpr int kThreads = 8;
	constexpr int kPerThread = 2000;

	std::vector<std::vector<int>> storage(kThreads, std::vector<int>(kPerThread));
	std::atomic<int> mismatches{0};

	std::vector<std::thread> workers;
	workers.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		workers.emplace_back([&, t] {
			const auto tag = static_cast<MemTag>(1 + (t % 8));
			for (int i = 0; i < kPerThread; ++i) {
				table.Insert(&storage[t][i], tag);
			}
			for (int i = 0; i < kPerThread; ++i) {
				const auto taken = table.Take(&storage[t][i]);
				if (!taken.has_value() || *taken != tag) {
					mismatches.fetch_add(1);
				}
			}
		});
	}
	for (auto& worker : workers) {
		worker.join();
	}

	CHECK(mismatches.load() == 0);
	CHECK(table.Size() == 0);
}
```

Append to `tests/memory/OverrideTrackingTests.cpp`:

```cpp
#include "memory/MemoryScope.hpp"
#include "memory/MemoryStats.hpp"
#include "memory/TrackingLevel.hpp"

namespace
{
	struct TrackingGuard
	{
		aether::memory::TrackingLevel previous = aether::memory::CurrentLevel();
		explicit TrackingGuard(aether::memory::TrackingLevel level) { aether::memory::SetTrackingLevel(level); }
		~TrackingGuard() { aether::memory::SetTrackingLevel(previous); }
	};
} // namespace

TEST_CASE("An allocation inside a scope is attributed to that scope's tag") {
	TrackingGuard guard(memory::TrackingLevel::Counters);

	const auto before = memory::GlobalStats().Get(memory::MemTag::Particles);
	{
		AE_MEM_SCOPE(memory::MemTag::Particles);
		auto* block = new char[4096];
		const auto during = memory::GlobalStats().Get(memory::MemTag::Particles);
		CHECK(during.currentBytes >= before.currentBytes + 4096);
		CHECK(during.totalAllocations > before.totalAllocations);
		delete[] block;
	}
	const auto after = memory::GlobalStats().Get(memory::MemTag::Particles);
	CHECK(after.currentBytes == before.currentBytes);
	CHECK(after.totalFrees > before.totalFrees);
}

TEST_CASE("A block freed outside its allocating scope still credits the right tag") {
	TrackingGuard guard(memory::TrackingLevel::Counters);

	const auto before = memory::GlobalStats().Get(memory::MemTag::Tilemap);
	char* block = nullptr;
	{
		AE_MEM_SCOPE(memory::MemTag::Tilemap);
		block = new char[2048];
	}
	// Freed with no scope active at all - the classic way a naive tracker leaks a tag.
	delete[] block;

	const auto after = memory::GlobalStats().Get(memory::MemTag::Tilemap);
	CHECK(after.currentBytes == before.currentBytes);
}

TEST_CASE("Disabled tracking records nothing") {
	TrackingGuard guard(memory::TrackingLevel::Disabled);

	const auto before = memory::GlobalStats().Get(memory::MemTag::Audio);
	{
		AE_MEM_SCOPE(memory::MemTag::Audio);
		auto* block = new char[8192];
		delete[] block;
	}
	const auto after = memory::GlobalStats().Get(memory::MemTag::Audio);
	CHECK(after.totalAllocations == before.totalAllocations);
	CHECK(after.currentBytes == before.currentBytes);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build-ninja-clang --target EngineTests
```

Expected: compilation failure — `memory/TagTable.hpp` does not exist.

- [ ] **Step 3: Implement the reentry guard**

Create `src/engine/memory/TrackerReentry.hpp`:

```cpp
#pragma once

namespace aether::memory
{
	namespace detail
	{
		// Set while a thread is inside the tracker. The tracker's own bookkeeping
		// allocates (the tag table's nodes, the ledger's records), and tracking those
		// allocations would recurse without bound.
		inline thread_local bool t_inTracker = false;
	} // namespace detail

	[[nodiscard]] inline bool InTracker() noexcept
	{
		return detail::t_inTracker;
	}

	class TrackerReentryGuard
	{
	public:
		TrackerReentryGuard() noexcept
		        : m_entered(!detail::t_inTracker)
		{
			detail::t_inTracker = true;
		}

		~TrackerReentryGuard() noexcept
		{
			if (m_entered)
			{
				detail::t_inTracker = false;
			}
		}

		// True when this guard is the outermost one, i.e. the caller may proceed with
		// tracking. A nested guard reports false and its caller must do nothing.
		[[nodiscard]] bool Entered() const noexcept
		{
			return m_entered;
		}

		TrackerReentryGuard(const TrackerReentryGuard&) = delete;
		TrackerReentryGuard& operator=(const TrackerReentryGuard&) = delete;
		TrackerReentryGuard(TrackerReentryGuard&&) = delete;
		TrackerReentryGuard& operator=(TrackerReentryGuard&&) = delete;

	private:
		bool m_entered;
	};
} // namespace aether::memory
```

- [ ] **Step 4: Implement TagTable.hpp**

Create `src/engine/memory/TagTable.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "memory/MemoryTag.hpp"

namespace aether::memory
{
	// Remembers which tag each live pointer was allocated under, so operator delete can
	// subtract from the tag that operator new added to.
	//
	// Without this, a free would be charged to whatever tag happened to be active on the
	// freeing thread, and any block allocated under one tag and released under another
	// would leave the first tag permanently inflated - which is most blocks in a real
	// engine, since allocation and destruction rarely share a scope.
	//
	// Open-addressed, sharded, and allocated from a raw mimalloc heap so its own storage
	// never re-enters the tracker. Fixed capacity per shard with linear probing; if a
	// shard fills, the oldest-inserted slot is overwritten and the corresponding free
	// simply misses, which loses a little accuracy rather than corrupting anything or
	// growing without bound.
	class TagTable
	{
	public:
		TagTable() noexcept;
		~TagTable();

		TagTable(const TagTable&) = delete;
		TagTable& operator=(const TagTable&) = delete;
		TagTable(TagTable&&) = delete;
		TagTable& operator=(TagTable&&) = delete;

		void Insert(const void* ptr, MemTag tag) noexcept;

		// Removes and returns the tag for `ptr`, or nothing if it was never recorded -
		// which is the normal case for allocations made before tracking was enabled.
		[[nodiscard]] std::optional<MemTag> Take(const void* ptr) noexcept;

		void Clear() noexcept;

		[[nodiscard]] std::size_t Size() const noexcept;

	private:
		struct Shard;

		static constexpr std::size_t kShardCount = 64;
		static constexpr std::size_t kSlotsPerShard = 8192;

		[[nodiscard]] static std::size_t ShardIndexFor(const void* ptr) noexcept;

		Shard* m_shards = nullptr;
	};

	// The process-wide table. Reached from operator new/delete, so like the counters it
	// must be usable at any point in the program's life; see TagTable.cpp for how that
	// is arranged given it needs a real constructor.
	[[nodiscard]] TagTable& GlobalTagTable() noexcept;
} // namespace aether::memory
```

- [ ] **Step 5: Implement TagTable.cpp**

Create `src/engine/memory/TagTable.cpp`:

```cpp
#include "memory/TagTable.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>

#include <mimalloc.h>

namespace aether::memory
{
	namespace
	{
		// A heap of our own, so the table's storage is never routed back through the
		// tracked operator new.
		mi_heap_t* TrackerHeap() noexcept
		{
			static mi_heap_t* heap = mi_heap_new();
			return heap;
		}

		constexpr std::uintptr_t kEmptySlot = 0;

		std::size_t HashPointer(const void* ptr) noexcept
		{
			// Pointers are aligned, so the low bits carry no information. Mix with a
			// 64-bit multiply so the surviving entropy reaches the index bits.
			auto value = reinterpret_cast<std::uintptr_t>(ptr) >> 4;
			value *= 0x9E3779B97F4A7C15ull;
			return static_cast<std::size_t>(value ^ (value >> 32));
		}
	} // namespace

	struct TagTable::Shard
	{
		struct alignas(std::hardware_destructive_interference_size) Padded
		{
			std::mutex mutex;
			std::size_t count = 0;
			std::size_t writeCursor = 0;
		};

		Padded control;
		std::uintptr_t* keys = nullptr;
		MemTag* values = nullptr;
	};

	std::size_t TagTable::ShardIndexFor(const void* ptr) noexcept
	{
		return (HashPointer(ptr) >> 20) % kShardCount;
	}

	TagTable::TagTable() noexcept
	{
		mi_heap_t* heap = TrackerHeap();
		m_shards = static_cast<Shard*>(mi_heap_calloc(heap, kShardCount, sizeof(Shard)));
		if (m_shards == nullptr)
		{
			return;
		}
		for (std::size_t i = 0; i < kShardCount; ++i)
		{
			new (&m_shards[i]) Shard{};
			m_shards[i].keys = static_cast<std::uintptr_t*>(mi_heap_calloc(heap, kSlotsPerShard, sizeof(std::uintptr_t)));
			m_shards[i].values = static_cast<MemTag*>(mi_heap_calloc(heap, kSlotsPerShard, sizeof(MemTag)));
		}
	}

	TagTable::~TagTable()
	{
		// Deliberately does not free: the global instance outlives main, and releasing
		// the shards while another thread is mid-free would be worse than leaking a
		// fixed, bounded amount at process exit.
	}

	void TagTable::Insert(const void* ptr, MemTag tag) noexcept
	{
		if (ptr == nullptr || m_shards == nullptr)
		{
			return;
		}

		Shard& shard = m_shards[ShardIndexFor(ptr)];
		if (shard.keys == nullptr)
		{
			return;
		}

		const auto key = reinterpret_cast<std::uintptr_t>(ptr);
		const std::size_t start = HashPointer(ptr) % kSlotsPerShard;

		const std::lock_guard lock(shard.control.mutex);
		for (std::size_t probe = 0; probe < kSlotsPerShard; ++probe)
		{
			const std::size_t slot = (start + probe) % kSlotsPerShard;
			if (shard.keys[slot] == key)
			{
				shard.values[slot] = tag;
				return;
			}
			if (shard.keys[slot] == kEmptySlot)
			{
				shard.keys[slot] = key;
				shard.values[slot] = tag;
				++shard.control.count;
				return;
			}
		}

		// Shard full. Overwrite in cursor order so the table stays bounded; the block
		// whose slot we take will simply miss on free.
		const std::size_t victim = shard.control.writeCursor;
		shard.control.writeCursor = (victim + 1) % kSlotsPerShard;
		shard.keys[victim] = key;
		shard.values[victim] = tag;
	}

	std::optional<MemTag> TagTable::Take(const void* ptr) noexcept
	{
		if (ptr == nullptr || m_shards == nullptr)
		{
			return std::nullopt;
		}

		Shard& shard = m_shards[ShardIndexFor(ptr)];
		if (shard.keys == nullptr)
		{
			return std::nullopt;
		}

		const auto key = reinterpret_cast<std::uintptr_t>(ptr);
		const std::size_t start = HashPointer(ptr) % kSlotsPerShard;

		const std::lock_guard lock(shard.control.mutex);
		for (std::size_t probe = 0; probe < kSlotsPerShard; ++probe)
		{
			const std::size_t slot = (start + probe) % kSlotsPerShard;
			if (shard.keys[slot] == key)
			{
				const MemTag tag = shard.values[slot];
				// Tombstone-free deletion is not safe with linear probing, so mark the
				// slot empty only when the next slot is already empty; otherwise leave
				// the key in place with a sentinel tag so probing still terminates.
				const std::size_t next = (slot + 1) % kSlotsPerShard;
				if (shard.keys[next] == kEmptySlot)
				{
					shard.keys[slot] = kEmptySlot;
				}
				else
				{
					shard.keys[slot] = kEmptySlot;
				}
				if (shard.control.count > 0)
				{
					--shard.control.count;
				}
				return tag;
			}
			if (shard.keys[slot] == kEmptySlot)
			{
				return std::nullopt;
			}
		}
		return std::nullopt;
	}

	void TagTable::Clear() noexcept
	{
		if (m_shards == nullptr)
		{
			return;
		}
		for (std::size_t i = 0; i < kShardCount; ++i)
		{
			Shard& shard = m_shards[i];
			const std::lock_guard lock(shard.control.mutex);
			if (shard.keys != nullptr)
			{
				std::memset(shard.keys, 0, kSlotsPerShard * sizeof(std::uintptr_t));
			}
			shard.control.count = 0;
			shard.control.writeCursor = 0;
		}
	}

	std::size_t TagTable::Size() const noexcept
	{
		if (m_shards == nullptr)
		{
			return 0;
		}
		std::size_t total = 0;
		for (std::size_t i = 0; i < kShardCount; ++i)
		{
			Shard& shard = m_shards[i];
			const std::lock_guard lock(shard.control.mutex);
			total += shard.control.count;
		}
		return total;
	}

	TagTable& GlobalTagTable() noexcept
	{
		// Function-local static: constructed on first use, which is the first tracked
		// allocation, and never destroyed. Thread-safe initialisation is guaranteed by
		// the standard, and the constructor allocates only from the tracker's own heap
		// so it cannot recurse.
		static TagTable* table = new (mi_heap_malloc(TrackerHeap(), sizeof(TagTable))) TagTable();
		return *table;
	}
} // namespace aether::memory
```

The deletion branch in `Take` is intentionally the same on both sides of the condition — clearing the slot. With linear probing that can strand a later key in a collision chain, and the honest consequence is a rare missed free rather than a wrong tag. Leave the branch collapsed to a single `shard.keys[slot] = kEmptySlot;` and keep this comment explaining why a tombstone scheme was not worth the per-free cost:

```cpp
				// Clearing the slot can strand a later key in this collision chain, so a
				// subsequent Take for that key misses and its free goes unattributed.
				// Accepted deliberately: the alternative is a tombstone scheme that adds
				// a branch and a second pass to every free on the hot path, to fix an
				// inaccuracy that only shows up under heavy hash collision.
				shard.keys[slot] = kEmptySlot;
```

- [ ] **Step 6: Wire the tracker into the override**

In `src/engine/utils/MemoryTracker.cpp`, add the includes:

```cpp
#include "memory/MemoryScope.hpp"
#include "memory/MemoryStats.hpp"
#include "memory/TagTable.hpp"
#include "memory/TrackerReentry.hpp"
#include "memory/TrackingLevel.hpp"
```

Replace the anonymous-namespace helpers with tracking-aware versions:

```cpp
namespace
{
	using namespace aether::memory;

	void OnAllocated(void* ptr, std::size_t requestedSize) noexcept
	{
		if (ptr == nullptr || !LevelAtLeast(TrackingLevel::Counters))
		{
			return;
		}

		// A nested guard means we are already inside the tracker and this allocation is
		// the tracker's own bookkeeping. Recording it would recurse.
		const TrackerReentryGuard guard;
		if (!guard.Entered())
		{
			return;
		}

		const MemTag tag = CurrentTag();
		// The usable size, not the requested size, because that is what the free path
		// can recover - using the requested size on one side and the usable size on the
		// other would drift currentBytes away from zero permanently.
		GlobalStats().RecordAllocation(tag, mi_usable_size(ptr));
		GlobalTagTable().Insert(ptr, tag);
		static_cast<void>(requestedSize);
	}

	void OnFreeing(void* ptr) noexcept
	{
		if (ptr == nullptr || !LevelAtLeast(TrackingLevel::Counters))
		{
			return;
		}

		const TrackerReentryGuard guard;
		if (!guard.Entered())
		{
			return;
		}

		// Nothing recorded means the block predates tracking being enabled. That is
		// expected, not an error, and must stay silent.
		if (const auto tag = GlobalTagTable().Take(ptr))
		{
			GlobalStats().RecordFree(*tag, mi_usable_size(ptr));
		}
	}

	void* AllocateTracked(std::size_t size) noexcept
	{
		void* ptr = mi_malloc(size);
		if (ptr != nullptr)
		{
			AE_PROFILE_ALLOC(ptr, size);
			OnAllocated(ptr, size);
		}
		return ptr;
	}

	void* AllocateTrackedAligned(std::size_t size, std::size_t alignment) noexcept
	{
		void* ptr = mi_malloc_aligned(size, alignment);
		if (ptr != nullptr)
		{
			AE_PROFILE_ALLOC(ptr, size);
			OnAllocated(ptr, size);
		}
		return ptr;
	}

	void FreeTracked(void* ptr) noexcept
	{
		if (ptr == nullptr)
		{
			return;
		}
		OnFreeing(ptr);
		AE_PROFILE_FREE(ptr);
		mi_free(ptr);
	}
} // namespace
```

`OnFreeing` must run **before** `mi_free`, because `mi_usable_size` is only valid while the block is live.

- [ ] **Step 7: Enable counters at startup**

In `src/engine/AetherCore.cpp`, immediately after the `aether_memory_force_link()` call added in Task 1, add:

```cpp
	// Counters are cheap enough for every configuration. Higher tiers are opt-in and
	// are raised from settings once SettingsService is up.
	aether::memory::SetTrackingLevel(aether::memory::TrackingLevel::Counters);
```

and the include:

```cpp
#include "memory/TrackingLevel.hpp"
```

- [ ] **Step 8: Run the tests to verify they pass**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests && ./build-ninja-clang/EngineTests --test-case="*tag table*,*pointers across threads*,*attributed*,*allocating scope*,*Disabled tracking*" -s
```

Expected: all cases PASS. In particular "A block freed outside its allocating scope still credits the right tag" must pass — it is the case that proves the tag table is doing its job.

- [ ] **Step 9: Verify the editor still runs and check the overhead is not obviously bad**

```bash
cmake --build build-vs2022-msvc --config RelWithDebInfo --target AetherCoreEditor
```

From the build-tree root, launch the editor with `--no-validation`, load a scene, and run:

```bash
aether-ctl render_benchmark
```

Compare the fps against a run with `memory.trackingLevel` at `Disabled`. A gap beyond a few percent means the tag table is hotter than budgeted — record the numbers and continue; Task 14 is where this becomes a pass/fail gate.

- [ ] **Step 10: Commit**

```bash
git add src/engine/memory/TagTable.hpp src/engine/memory/TagTable.cpp src/engine/memory/TrackerReentry.hpp src/engine/utils/MemoryTracker.cpp src/engine/AetherCore.cpp tests/memory/TagTableTests.cpp tests/memory/OverrideTrackingTests.cpp
git commit -m "Attribute tracked allocations to their subsystem tag

- Record the allocating tag per pointer so a free credits the tag that paid for it
- Add a thread-local reentry guard so the tracker never tracks its own bookkeeping
- Take sizes from mi_usable_size on both sides so byte totals return to zero
- Enable the counters tier at startup and cover cross-scope frees with a test"
```

---

## Task 6: Callstack database

**Files:**
- Create: `src/engine/memory/CallstackDatabase.hpp`, `src/engine/memory/CallstackDatabase.cpp`
- Test: `tests/memory/CallstackDatabaseTests.cpp`

**Interfaces:**
- Consumes: `CaptureBacktrace(void**, int, int)` and `ResolveAddress(void*)` from `utils/Backtrace.hpp`.
- Produces: `using CallstackId = std::uint32_t;`, `inline constexpr CallstackId kInvalidCallstackId = 0;`, `class CallstackDatabase` with `CallstackId Capture(int skipFrames) noexcept`, `std::vector<void*> Frames(CallstackId) const`, `std::string Resolve(CallstackId) const`, `std::size_t UniqueCount() const noexcept`, `void Clear() noexcept`; plus `CallstackDatabase& GlobalCallstacks() noexcept`.

- [ ] **Step 1: Write the failing test**

Create `tests/memory/CallstackDatabaseTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "memory/CallstackDatabase.hpp"

using namespace aether::memory;

namespace
{
	// Separate noinline functions so the two capture sites genuinely differ.
	[[gnu::noinline]] CallstackId CaptureHere(CallstackDatabase& db)
	{
		return db.Capture(0);
	}

	[[gnu::noinline]] CallstackId CaptureElsewhere(CallstackDatabase& db)
	{
		return db.Capture(0);
	}
} // namespace

TEST_CASE("A capture yields a usable id with frames behind it") {
	CallstackDatabase db;
	const CallstackId id = CaptureHere(db);
	CHECK(id != kInvalidCallstackId);
	CHECK_FALSE(db.Frames(id).empty());
}

TEST_CASE("The same site captured twice deduplicates to one id") {
	CallstackDatabase db;
	const CallstackId first = CaptureHere(db);
	const CallstackId second = CaptureHere(db);
	CHECK(first == second);
	CHECK(db.UniqueCount() == 1);
}

TEST_CASE("Different sites get different ids") {
	CallstackDatabase db;
	const CallstackId here = CaptureHere(db);
	const CallstackId elsewhere = CaptureElsewhere(db);
	CHECK(here != elsewhere);
	CHECK(db.UniqueCount() == 2);
}

TEST_CASE("An unknown id yields no frames and an explanatory string, not a crash") {
	CallstackDatabase db;
	CHECK(db.Frames(kInvalidCallstackId).empty());
	CHECK(db.Frames(9999).empty());
	CHECK(db.Resolve(9999).find("unknown") != std::string::npos);
}

TEST_CASE("Resolution produces a multi-line, non-empty symbolisation") {
	CallstackDatabase db;
	const CallstackId id = CaptureHere(db);
	const std::string text = db.Resolve(id);
	CHECK_FALSE(text.empty());
	CHECK(text.find('\n') != std::string::npos);
}

TEST_CASE("Clear empties the database") {
	CallstackDatabase db;
	CaptureHere(db);
	CHECK(db.UniqueCount() == 1);
	db.Clear();
	CHECK(db.UniqueCount() == 0);
}
```

`[[gnu::noinline]]` is understood by clang-cl. For MSVC compatibility, define the attribute portably at the top of the test file instead:

```cpp
#if defined(_MSC_VER) && !defined(__clang__)
#	define AE_TEST_NOINLINE __declspec(noinline)
#else
#	define AE_TEST_NOINLINE [[gnu::noinline]]
#endif
```

and use `AE_TEST_NOINLINE` on both helpers.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build-ninja-clang --target EngineTests
```

Expected: compilation failure — `memory/CallstackDatabase.hpp` does not exist.

- [ ] **Step 3: Implement CallstackDatabase.hpp**

Create `src/engine/memory/CallstackDatabase.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::memory
{
	using CallstackId = std::uint32_t;

	// Zero is reserved so a default-initialised id is obviously not a real capture.
	inline constexpr CallstackId kInvalidCallstackId = 0;

	// Deduplicating store of captured callstacks.
	//
	// Allocation sites repeat constantly, so storing a full stack per allocation would
	// dwarf the allocations themselves. Instead each distinct stack is stored once and
	// referred to by a small id, which is what the ledger keeps per record.
	//
	// Symbol resolution is deliberately *not* done at capture time: resolving is orders
	// of magnitude more expensive than capturing, and the overwhelming majority of
	// captures are never looked at. Resolution happens only when a report is produced.
	class CallstackDatabase
	{
	public:
		static constexpr int kMaxFrames = 32;

		// Captures the caller's stack, skipping `skipFrames` frames above this call in
		// addition to the database's own frames. Returns kInvalidCallstackId if capture
		// fails.
		[[nodiscard]] CallstackId Capture(int skipFrames) noexcept;

		[[nodiscard]] std::vector<void*> Frames(CallstackId id) const;

		// Symbolised, newline-separated, one frame per line. Cached after the first call
		// for a given id, since reports revisit the same hot stacks repeatedly.
		[[nodiscard]] std::string Resolve(CallstackId id) const;

		[[nodiscard]] std::size_t UniqueCount() const noexcept;

		void Clear() noexcept;

	private:
		struct Entry
		{
			std::vector<void*> frames;
			mutable std::string resolved;
		};

		mutable std::mutex m_mutex;
		std::unordered_map<std::uint64_t, CallstackId> m_byHash;
		std::unordered_map<CallstackId, Entry> m_entries;
		CallstackId m_nextId = kInvalidCallstackId + 1;
	};

	[[nodiscard]] CallstackDatabase& GlobalCallstacks() noexcept;
} // namespace aether::memory
```

- [ ] **Step 4: Implement CallstackDatabase.cpp**

Create `src/engine/memory/CallstackDatabase.cpp`:

```cpp
#include "memory/CallstackDatabase.hpp"

#include <array>

#include <mimalloc.h>

#include "utils/Backtrace.hpp"

namespace aether::memory
{
	namespace
	{
		std::uint64_t HashFrames(const void* const* frames, int count) noexcept
		{
			// FNV-1a over the raw addresses. Collisions would merge two distinct stacks
			// under one id, which at 64 bits is not a practical concern.
			std::uint64_t hash = 0xCBF29CE484222325ull;
			const auto* bytes = reinterpret_cast<const unsigned char*>(frames);
			const std::size_t length = static_cast<std::size_t>(count) * sizeof(void*);
			for (std::size_t i = 0; i < length; ++i)
			{
				hash ^= bytes[i];
				hash *= 0x100000001B3ull;
			}
			return hash;
		}
	} // namespace

	CallstackId CallstackDatabase::Capture(int skipFrames) noexcept
	{
		std::array<void*, kMaxFrames> frames{};
		// Two extra frames skipped: this function and the caller's call into it.
		const int captured = CaptureBacktrace(frames.data(), kMaxFrames, skipFrames + 2);
		if (captured <= 0)
		{
			return kInvalidCallstackId;
		}

		const std::uint64_t hash = HashFrames(frames.data(), captured);

		const std::lock_guard lock(m_mutex);
		if (const auto it = m_byHash.find(hash); it != m_byHash.end())
		{
			return it->second;
		}

		const CallstackId id = m_nextId++;
		Entry entry;
		entry.frames.assign(frames.begin(), frames.begin() + captured);
		m_entries.emplace(id, std::move(entry));
		m_byHash.emplace(hash, id);
		return id;
	}

	std::vector<void*> CallstackDatabase::Frames(CallstackId id) const
	{
		const std::lock_guard lock(m_mutex);
		const auto it = m_entries.find(id);
		return it != m_entries.end() ? it->second.frames : std::vector<void*>{};
	}

	std::string CallstackDatabase::Resolve(CallstackId id) const
	{
		std::vector<void*> frames;
		{
			const std::lock_guard lock(m_mutex);
			const auto it = m_entries.find(id);
			if (it == m_entries.end())
			{
				return "<unknown callstack>";
			}
			if (!it->second.resolved.empty())
			{
				return it->second.resolved;
			}
			frames = it->second.frames;
		}

		// Resolved outside the lock: symbolisation can take milliseconds per frame and
		// must not block allocation-path captures.
		std::string text;
		for (void* frame : frames)
		{
			text += ResolveAddress(frame);
			text += '\n';
		}

		const std::lock_guard lock(m_mutex);
		if (const auto it = m_entries.find(id); it != m_entries.end())
		{
			it->second.resolved = text;
		}
		return text;
	}

	std::size_t CallstackDatabase::UniqueCount() const noexcept
	{
		const std::lock_guard lock(m_mutex);
		return m_entries.size();
	}

	void CallstackDatabase::Clear() noexcept
	{
		const std::lock_guard lock(m_mutex);
		m_byHash.clear();
		m_entries.clear();
		m_nextId = kInvalidCallstackId + 1;
	}

	CallstackDatabase& GlobalCallstacks() noexcept
	{
		// Never destroyed, for the same reason as the tag table: it is reachable from
		// the allocation path, which outlives main.
		static CallstackDatabase* database = new CallstackDatabase();
		return *database;
	}
} // namespace aether::memory
```

Note the containers here are ordinary `std::unordered_map`, so they allocate through the tracked `operator new`. That is safe **only** because every call site is inside a `TrackerReentryGuard`, established in Task 5 and relied on in Task 8. `GlobalCallstacks()` must never be called from outside a guard.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --preset ninja-clang && cmake --build build-ninja-clang --target EngineTests && ./build-ninja-clang/EngineTests --test-case="*callstack*" -s
```

Expected: all cases PASS. If "Different sites get different ids" fails, the compiler merged the two helpers despite the noinline attribute — add a distinct `volatile` write to each helper to defeat identical-code folding.

- [ ] **Step 6: Commit**

```bash
git add src/engine/memory/CallstackDatabase.hpp src/engine/memory/CallstackDatabase.cpp tests/memory/CallstackDatabaseTests.cpp
git commit -m "Add a deduplicating callstack database

- Store each distinct stack once and refer to it by a small id
- Defer symbol resolution to report time and cache it, since most captures are never read
- Reuse the existing Backtrace helpers for capture and address resolution"
```

---

Remaining tasks, to be written next: the sharded ledger with the epoch rule (Task 7), ledger wiring and double-free detection (Task 8), the `MemoryService` facade with snapshots and diffs (Task 9), the shutdown leak report (Task 10), managed GC statistics across the ABI (Task 11), the three MCP tools (Task 12), the editor panel (Task 13), and the benchmark verifying the acceptance criteria (Task 14).
