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

Remaining tasks in this plan, to be written next: tracking levels and settings (Task 4), wiring the counters into the override (Task 5), the callstack database (Task 6), the sharded ledger with the epoch rule (Task 7), ledger wiring and double-free detection (Task 8), the `MemoryService` facade with snapshots and diffs (Task 9), the shutdown leak report (Task 10), managed GC statistics across the ABI (Task 11), the three MCP tools (Task 12), the editor panel (Task 13), and the benchmark that verifies the acceptance criteria (Task 14).
