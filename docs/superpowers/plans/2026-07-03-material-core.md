# Material Core (①) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace AetherCore's jerry-rigged material path with a clean, content-addressed material registry: one authoring struct, one CPU→GPU packing path, opaque ref-counted handles, and predictable default/vertex-color behavior — engine-side only.

**Architecture:** A new `MaterialRegistry` owns slot allocation on top of a `IMaterialSlotSink` interface (`MaterialBuffer` is the production impl; a fake is used in tests). `MaterialAsset` is pure authoring data; `PackMaterial()` is the single packing function; `MaterialHandle` is an opaque `{index,generation}`. `MaterialComponent` stores the handle plus a cached `gpuSlot` for the render hot-loop. Material data stays on the Vulkan 1.4 BDA/descriptor-heap path — no descriptor sets.

**Tech Stack:** C++23/26, CMake + CPM, doctest (added here as the engine's first test target), entt ECS, Vulkan 1.4 (VK_EXT_descriptor_heap), Slang shaders.

**Spec:** `docs/superpowers/specs/2026-07-03-material-core-design.md`

**Refinement vs spec §4.C:** the spec said "WorldRenderer calls `ResolveSlot`". Because `WorldRenderer::Flush` has 4 callers, the plan instead **caches the resolved slot in `MaterialComponent`** (set at assign time via the registry) and the render loop reads it directly. Same clean authoring separation, no per-draw registry lookup, no 4-caller ripple.

---

## File Structure

**New files (engine — auto-globbed, no CMake edit):**
- `src/engine/material/MaterialAsset.hpp` — authoring struct (replaces `Material`).
- `src/engine/material/MaterialHandle.hpp` — opaque `{index,generation}` handle.
- `src/engine/material/IMaterialSlotSink.hpp` — slot-backend interface.
- `src/engine/material/MaterialPacking.hpp` / `.cpp` — `PackMaterial(const MaterialAsset&)`.
- `src/engine/material/MaterialRegistry.hpp` / `.cpp` — the registry.

**New files (tests):**
- `tests/CMakeLists.txt`, `tests/TestMain.cpp`, `tests/material/PackMaterialTests.cpp`, `tests/material/MaterialRegistryTests.cpp`, `tests/material/FakeSlotSink.hpp`.

**Modified:**
- `CMake/Dependencies.cmake` (+doctest), `CMakeLists.txt` (+`enable_testing()`/`add_subdirectory(tests)`).
- `src/engine/material/GpuMaterial.hpp` (+`kModulateVertexColor`).
- `src/engine/material/MaterialBuffer.hpp`/`.cpp` (implement `IMaterialSlotSink`).
- `src/engine/scene/Components.hpp` (`MaterialComponent` → handle+slot).
- `src/engine/rendering/WorldRenderer.cpp` (read cached slot).
- `src/engine/assets/AssetManager.hpp`/`.cpp` (own registry; `RegisterMaterial`→registry).
- `src/engine/assets/AssetSubsystem.hpp`/`.cpp` (own+expose `MaterialRegistry`).
- `src/app/scripting/modules/WorldModule.cpp` (default material via registry).
- `src/app/scripting/modules/EffectsModule.cpp` (effect material via registry).
- `shaders/gltf_mesh.slang` + `shaders/include/GpuMaterial.slangh` (vertex-color flag).
- Delete `src/engine/material/Material.hpp` at the end.

---

## Task 1: Test harness (doctest, engine's first test target)

**Files:**
- Modify: `CMake/Dependencies.cmake` (end of file)
- Modify: `CMakeLists.txt:46-49`
- Create: `tests/CMakeLists.txt`, `tests/TestMain.cpp`, `tests/material/PackMaterialTests.cpp` (temporary smoke)

- [x] **Step 1: Add doctest via CPM.** Append to `CMake/Dependencies.cmake`:

```cmake
# ── Unit test framework ───────────────────────────────────────────────────────
CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
    # CMake 4.x removed compat for doctest's cmake_minimum_required(VERSION 3.0);
    # this shim lets its old CMakeLists configure under CMake >= 4.
    OPTIONS "CMAKE_POLICY_VERSION_MINIMUM 3.5"
)
```

> If CPM `OPTIONS` does not propagate the policy var in your CMake, the robust
> fallback is to `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` on the line immediately
> before this `CPMAddPackage(doctest ...)` call (scoped narrowly to the doctest add).

- [x] **Step 2: Wire the tests subdirectory.** In `CMakeLists.txt`, after `add_subdirectory(src/app)` (line 46) add:

```cmake
option(AETHERCORE_BUILD_TESTS "Build the engine unit test target" ON)
if(AETHERCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [x] **Step 3: Create `tests/CMakeLists.txt`:**

```cmake
add_executable(EngineTests
    TestMain.cpp
    material/PackMaterialTests.cpp
)
target_link_libraries(EngineTests PRIVATE Engine doctest::doctest)
aethercore_target_defaults(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)
```

- [x] **Step 4: Create `tests/TestMain.cpp`:**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [x] **Step 5: Create a smoke test `tests/material/PackMaterialTests.cpp`:**

```cpp
#include <doctest/doctest.h>

TEST_CASE("test harness runs") {
    CHECK(1 + 1 == 2);
}
```

- [x] **Step 6: Configure + build the test target.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
Expected: builds `EngineTests.exe` (CMake re-configures to pull doctest first).

> If linking full `Engine` fails on unresolved Vulkan/main globals, fall back: replace `Engine` in `target_link_libraries` with direct compilation of the material `.cpp` files (`target_sources(EngineTests PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/material/MaterialPacking.cpp ${CMAKE_SOURCE_DIR}/src/engine/material/MaterialRegistry.cpp)` + `target_include_directories(EngineTests PRIVATE ${CMAKE_SOURCE_DIR}/src/engine)` and needed dep includes). Prefer linking `Engine` first.

- [x] **Step 7: Run the tests.**

Run: `ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: `EngineTests` passes (1 assertion).

- [x] **Step 8: Commit.**

```bash
git add CMake/Dependencies.cmake CMakeLists.txt tests/
git commit -m "test: add doctest test target (engine's first)"
```

---

## Task 2: `MaterialAsset` + `GpuMaterial` flag

**Files:**
- Create: `src/engine/material/MaterialAsset.hpp`
- Modify: `src/engine/material/GpuMaterial.hpp:28-30`

- [x] **Step 1: Add the vertex-color flag** to `GpuMaterial.hpp` after `kAlphaMask` (line 30):

```cpp
		static constexpr std::uint32_t kAlphaMask = 1u << 2;
		static constexpr std::uint32_t kModulateVertexColor = 1u << 3;
```

- [x] **Step 2: Create `src/engine/material/MaterialAsset.hpp`** (authoring only — no GPU slot):

```cpp
#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <limits>

namespace aether
{
	// Pure authoring description of a surface. No GPU slot — that lives in the
	// MaterialRegistry. Packed into a GpuMaterial by PackMaterial().
	struct MaterialAsset
	{
		static constexpr std::uint32_t kNoTexture = std::numeric_limits<std::uint32_t>::max();

		glm::vec4 baseColorFactor{1.0f};
		float metallicFactor{0.0f};
		float roughnessFactor{0.5f};
		float occlusionStrength{1.0f};
		float alphaCutoff{0.5f};
		glm::vec3 emissiveFactor{0.0f};

		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;
		// When true, the shader multiplies base color by the mesh's vertex color
		// (used by primitive meshes that carry meaningful vertex colors).
		bool modulateVertexColor = false;

		std::uint32_t albedoSlot = kNoTexture;
		std::uint32_t normalSlot = kNoTexture;
		std::uint32_t metallicRoughnessSlot = kNoTexture;
		std::uint32_t occlusionSlot = kNoTexture;
		std::uint32_t emissiveSlot = kNoTexture;
	};
} // namespace aether
```

- [x] **Step 3: Commit.**

```bash
git add src/engine/material/MaterialAsset.hpp src/engine/material/GpuMaterial.hpp
git commit -m "feat(material): add MaterialAsset authoring struct + vertex-color flag"
```

---

## Task 3: `PackMaterial` (TDD)

**Files:**
- Create: `src/engine/material/MaterialPacking.hpp`, `src/engine/material/MaterialPacking.cpp`
- Test: `tests/material/PackMaterialTests.cpp`

- [x] **Step 1: Write the failing test** — replace the smoke body in `tests/material/PackMaterialTests.cpp`:

```cpp
#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialPacking.hpp"

using namespace aether;

TEST_CASE("PackMaterial copies PBR factors verbatim") {
    MaterialAsset a;
    a.baseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    a.metallicFactor = 0.6f;
    a.roughnessFactor = 0.7f;
    a.occlusionStrength = 0.8f;
    a.alphaCutoff = 0.25f;
    a.emissiveFactor = {1.0f, 0.5f, 0.0f};

    GpuMaterial g = PackMaterial(a);

    CHECK(g.baseColorFactor.x == doctest::Approx(0.1f));
    CHECK(g.baseColorFactor.w == doctest::Approx(0.4f));
    CHECK(g.metallicFactor == doctest::Approx(0.6f));
    CHECK(g.roughnessFactor == doctest::Approx(0.7f));
    CHECK(g.occlusionStrength == doctest::Approx(0.8f));
    CHECK(g.alphaCutoff == doctest::Approx(0.25f));
    CHECK(g.emissiveFactor.x == doctest::Approx(1.0f));
    CHECK(g.emissiveFactor.z == doctest::Approx(0.0f));
}

TEST_CASE("PackMaterial packs bools into flags and copies texture slots") {
    MaterialAsset a;
    a.doubleSided = true;
    a.alphaMask = true;
    a.modulateVertexColor = true;
    a.alphaBlend = false;
    a.albedoSlot = 5;
    a.emissiveSlot = 9;

    GpuMaterial g = PackMaterial(a);

    CHECK((g.flags & GpuMaterial::kDoubleSided) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaMask) != 0u);
    CHECK((g.flags & GpuMaterial::kModulateVertexColor) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaBlend) == 0u);
    CHECK(g.albedoSlot == 5u);
    CHECK(g.emissiveSlot == 9u);
    CHECK(g.normalSlot == GpuMaterial::kNoTexture);
}
```

- [x] **Step 2: Add the test file to the target.** In `tests/CMakeLists.txt`, `PackMaterialTests.cpp` is already listed (Task 1). No change.

- [x] **Step 3: Run to verify it fails.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
Expected: FAIL — `MaterialPacking.hpp` not found.

- [x] **Step 4: Create `src/engine/material/MaterialPacking.hpp`:**

```cpp
#pragma once

#include "material/GpuMaterial.hpp"

namespace aether
{
	struct MaterialAsset;

	// The single source of truth for CPU authoring -> GPU record conversion.
	[[nodiscard]] GpuMaterial PackMaterial(const MaterialAsset& asset);
} // namespace aether
```

- [x] **Step 5: Create `src/engine/material/MaterialPacking.cpp`:**

```cpp
#include "material/MaterialPacking.hpp"

#include "material/MaterialAsset.hpp"

namespace aether
{
	GpuMaterial PackMaterial(const MaterialAsset& a)
	{
		GpuMaterial g{};
		g.baseColorFactor = a.baseColorFactor;
		g.metallicFactor = a.metallicFactor;
		g.roughnessFactor = a.roughnessFactor;
		g.occlusionStrength = a.occlusionStrength;
		g.alphaCutoff = a.alphaCutoff;
		g.emissiveFactor = glm::vec4(a.emissiveFactor, 0.0f);

		std::uint32_t flags = 0u;
		if (a.doubleSided) flags |= GpuMaterial::kDoubleSided;
		if (a.alphaBlend) flags |= GpuMaterial::kAlphaBlend;
		if (a.alphaMask) flags |= GpuMaterial::kAlphaMask;
		if (a.modulateVertexColor) flags |= GpuMaterial::kModulateVertexColor;
		g.flags = flags;

		g.albedoSlot = a.albedoSlot;
		g.normalSlot = a.normalSlot;
		g.metallicRoughnessSlot = a.metallicRoughnessSlot;
		g.occlusionSlot = a.occlusionSlot;
		g.emissiveSlot = a.emissiveSlot;
		return g;
	}
} // namespace aether
```

- [x] **Step 6: Run to verify it passes.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests && ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: PASS.

- [x] **Step 7: Commit.**

```bash
git add src/engine/material/MaterialPacking.hpp src/engine/material/MaterialPacking.cpp tests/material/PackMaterialTests.cpp
git commit -m "feat(material): PackMaterial single source of truth (TDD)"
```

---

## Task 4: `MaterialHandle` + `IMaterialSlotSink`; `MaterialBuffer` implements it

**Files:**
- Create: `src/engine/material/MaterialHandle.hpp`, `src/engine/material/IMaterialSlotSink.hpp`
- Modify: `src/engine/material/MaterialBuffer.hpp`, `src/engine/material/MaterialBuffer.cpp`

- [x] **Step 1: Create `src/engine/material/MaterialHandle.hpp`:**

```cpp
#pragma once

#include <cstdint>

namespace aether
{
	// Opaque, ref-counted material reference. index = GPU slot; generation guards
	// against use-after-release. Default-constructed handles are invalid.
	struct MaterialHandle
	{
		static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

		std::uint32_t index = kInvalidIndex;
		std::uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const { return index != kInvalidIndex; }

		friend bool operator==(const MaterialHandle& a, const MaterialHandle& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}
	};
} // namespace aether
```

- [x] **Step 2: Create `src/engine/material/IMaterialSlotSink.hpp`** (no Vulkan includes — keeps the registry unit-testable):

```cpp
#pragma once

#include <cstdint>

#include "material/GpuMaterial.hpp"

namespace aether
{
	// Backend that owns GPU material slots. MaterialBuffer is the production impl;
	// tests inject a fake. No Vulkan types leak through this interface.
	class IMaterialSlotSink
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		virtual ~IMaterialSlotSink() = default;

		[[nodiscard]] virtual std::uint32_t AllocateSlot() = 0;
		virtual void FreeSlot(std::uint32_t slot) = 0;
		virtual void Write(std::uint32_t slot, const GpuMaterial& material) = 0;
		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether
```

- [x] **Step 3: Make `MaterialBuffer` implement the interface.** In `MaterialBuffer.hpp`, add the include and base, and `override` + `Capacity()`:

Change `class MaterialBuffer` (line 22) region to:

```cpp
#include "material/IMaterialSlotSink.hpp"
// ... existing includes ...

	class MaterialBuffer final : public IMaterialSlotSink
	{
	public:
		static constexpr std::uint32_t kMaxMaterials = 4096;
		static constexpr std::uint32_t kInvalidSlot = IMaterialSlotSink::kInvalidSlot;
		// ... existing members ...

		[[nodiscard]] std::uint32_t AllocateSlot() override;
		void FreeSlot(std::uint32_t slot) override;
		void Write(std::uint32_t slot, const GpuMaterial& material) override;
		[[nodiscard]] std::uint32_t Capacity() const override { return kMaxMaterials; }
```

(Keep all existing members/methods; only add `: public IMaterialSlotSink`, the `override`s, and `Capacity()`.)

- [x] **Step 4: Compile-check the engine.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds (signatures already matched; only vtable added).

- [x] **Step 5: Commit.**

```bash
git add src/engine/material/MaterialHandle.hpp src/engine/material/IMaterialSlotSink.hpp src/engine/material/MaterialBuffer.hpp src/engine/material/MaterialBuffer.cpp
git commit -m "feat(material): MaterialHandle + IMaterialSlotSink; MaterialBuffer implements sink"
```

---

## Task 5: `MaterialRegistry` — Acquire + dedup (TDD)

**Files:**
- Create: `src/engine/material/MaterialRegistry.hpp`, `src/engine/material/MaterialRegistry.cpp`
- Create: `tests/material/FakeSlotSink.hpp`, `tests/material/MaterialRegistryTests.cpp`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Create the fake sink `tests/material/FakeSlotSink.hpp`:**

```cpp
#pragma once
#include <vector>
#include "material/IMaterialSlotSink.hpp"

// In-memory slot backend for registry tests. No GPU.
class FakeSlotSink final : public aether::IMaterialSlotSink
{
public:
	explicit FakeSlotSink(std::uint32_t capacity = 8) : m_slots(capacity) {}

	std::uint32_t AllocateSlot() override {
		for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
			if (!m_used[i]) { m_used[i] = true; ++allocCount; return i; }
		}
		return kInvalidSlot;
	}
	void FreeSlot(std::uint32_t slot) override {
		if (slot < m_slots.size()) { m_used[slot] = false; ++freeCount; }
	}
	void Write(std::uint32_t slot, const aether::GpuMaterial& m) override {
		if (slot < m_slots.size()) { m_slots[slot] = m; ++writeCount; }
	}
	std::uint32_t Capacity() const override { return static_cast<std::uint32_t>(m_slots.size()); }

	int allocCount = 0, freeCount = 0, writeCount = 0;
private:
	std::vector<aether::GpuMaterial> m_slots;
	std::vector<bool> m_used = std::vector<bool>(m_slots.size(), false);
};
```

> Note: initialize `m_used` in the ctor body to match capacity: add `m_used.assign(capacity, false);` as the first ctor statement and drop the in-class initializer if the compiler orders members awkwardly.

- [x] **Step 2: Write failing tests `tests/material/MaterialRegistryTests.cpp`:**

```cpp
#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "FakeSlotSink.hpp"

using namespace aether;

TEST_CASE("Acquire returns a valid handle and writes the slot") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);

    MaterialAsset a; a.baseColorFactor = {1,0,0,1};
    MaterialHandle h = reg.Acquire(a);

    CHECK(h.IsValid());
    CHECK(sink.writeCount == 1);
    CHECK(reg.ResolveSlot(h) == h.index);
}

TEST_CASE("Identical assets dedup to one slot; distinct assets do not") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);

    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialAsset red2; red2.baseColorFactor = {1,0,0,1};
    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};

    MaterialHandle h1 = reg.Acquire(red);
    MaterialHandle h2 = reg.Acquire(red2);
    MaterialHandle h3 = reg.Acquire(blue);

    CHECK(h1 == h2);            // deduped
    CHECK(h1.index != h3.index); // distinct
    CHECK(sink.allocCount == 2); // only 2 slots used for 3 acquires
}
```

- [x] **Step 3: Register the test file** — add to `tests/CMakeLists.txt` `add_executable(EngineTests ...)` list:

```cmake
    material/MaterialRegistryTests.cpp
```

- [x] **Step 4: Run to verify it fails.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
Expected: FAIL — `MaterialRegistry.hpp` not found.

- [x] **Step 5: Create `src/engine/material/MaterialRegistry.hpp`:**

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "material/GpuMaterial.hpp"
#include "material/MaterialHandle.hpp"

namespace aether
{
	class IMaterialSlotSink;
	struct MaterialAsset;

	// Content-addressed, ref-counted material table over a slot sink. Immutable:
	// identical assets share one slot; per-entity variation is a phase-2 concern.
	class MaterialRegistry
	{
	public:
		explicit MaterialRegistry(IMaterialSlotSink& sink);

		// Registers the fallback material returned by ResolveSlot for stale/invalid
		// handles. Call once after construction.
		void InitializeDefault(const MaterialAsset& defaultAsset);

		[[nodiscard]] MaterialHandle Acquire(const MaterialAsset& asset);
		void Release(MaterialHandle handle);
		[[nodiscard]] std::uint32_t ResolveSlot(MaterialHandle handle) const;
		[[nodiscard]] MaterialHandle DefaultHandle() const { return m_defaultHandle; }

	private:
		struct SlotEntry
		{
			GpuMaterial packed{};
			std::uint64_t hash = 0;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
		};

		IMaterialSlotSink& m_sink;
		mutable std::mutex m_mutex;
		std::vector<SlotEntry> m_slots;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToSlot;
		MaterialHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
	};
} // namespace aether
```

- [x] **Step 6: Create `src/engine/material/MaterialRegistry.cpp`:**

```cpp
#include "material/MaterialRegistry.hpp"

#include <cstring>

#include "material/IMaterialSlotSink.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialPacking.hpp"

namespace aether
{
	namespace
	{
		std::uint64_t HashBytes(const void* data, std::size_t n)
		{
			// FNV-1a 64-bit. Collisions are resolved by a byte-compare on hit,
			// so hash quality only affects dedup speed, not correctness.
			const auto* p = static_cast<const unsigned char*>(data);
			std::uint64_t h = 1469598103934665603ull;
			for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
			return h;
		}
	} // namespace

	MaterialRegistry::MaterialRegistry(IMaterialSlotSink& sink) : m_sink(sink)
	{
		m_slots.resize(sink.Capacity());
	}

	void MaterialRegistry::InitializeDefault(const MaterialAsset& defaultAsset)
	{
		const MaterialHandle h = Acquire(defaultAsset);
		std::scoped_lock lock(m_mutex); // set default fields under the lock (no torn read in ResolveSlot)
		m_defaultHandle = h;
		m_defaultSlot = h.index;
	}

	MaterialHandle MaterialRegistry::Acquire(const MaterialAsset& asset)
	{
		const GpuMaterial packed = PackMaterial(asset);
		const std::uint64_t hash = HashBytes(&packed, sizeof(packed));

		std::scoped_lock lock(m_mutex);

		auto range = m_hashToSlot.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			const std::uint32_t slot = it->second;
			SlotEntry& e = m_slots[slot];
			if (e.alive && std::memcmp(&e.packed, &packed, sizeof(packed)) == 0)
			{
				++e.refcount;
				return MaterialHandle{slot, e.generation};
			}
		}

		const std::uint32_t slot = m_sink.AllocateSlot();
		if (slot == IMaterialSlotSink::kInvalidSlot || slot >= m_slots.size())
		{
			// Sink full: invalid handle. ResolveSlot() falls back to the default
			// slot and Release() is a no-op, so this never touches the default
			// material's refcount. (Returning m_defaultHandle here would let the
			// caller's later Release() drive the default to zero and free it.)
			return MaterialHandle{};
		}

		m_sink.Write(slot, packed);
		SlotEntry& e = m_slots[slot];
		e.packed = packed;
		e.hash = hash;
		e.refcount = 1;
		e.alive = true;
		m_hashToSlot.emplace(hash, slot);
		return MaterialHandle{slot, e.generation};
	}

	void MaterialRegistry::Release(MaterialHandle handle)
	{
		if (!handle.IsValid() || handle.index >= m_slots.size()) return;

		std::scoped_lock lock(m_mutex);
		SlotEntry& e = m_slots[handle.index];
		if (!e.alive || e.generation != handle.generation) return; // stale/double-free
		if (--e.refcount > 0) return;

		auto range = m_hashToSlot.equal_range(e.hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second == handle.index) { m_hashToSlot.erase(it); break; }
		}
		e.alive = false;
		++e.generation; // invalidate outstanding handles to this slot
		m_sink.FreeSlot(handle.index);
	}

	std::uint32_t MaterialRegistry::ResolveSlot(MaterialHandle handle) const
	{
		std::scoped_lock lock(m_mutex);
		if (handle.IsValid() && handle.index < m_slots.size())
		{
			const SlotEntry& e = m_slots[handle.index];
			if (e.alive && e.generation == handle.generation) return handle.index;
		}
		return m_defaultSlot;
	}
} // namespace aether
```

- [x] **Step 7: Run to verify it passes.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests && ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: PASS (Acquire + dedup tests).

- [x] **Step 8: Commit.**

```bash
git add src/engine/material/MaterialRegistry.hpp src/engine/material/MaterialRegistry.cpp tests/material/
git commit -m "feat(material): MaterialRegistry Acquire + content-addressed dedup (TDD)"
```

---

## Task 6: `MaterialRegistry` — Release, generation, default (TDD)

**Files:**
- Modify: `tests/material/MaterialRegistryTests.cpp`

- [x] **Step 1: Add failing tests** to `MaterialRegistryTests.cpp`:

```cpp
TEST_CASE("Refcount: shared slot survives one release, frees at zero") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);
    MaterialAsset a; a.baseColorFactor = {1,0,0,1};

    MaterialHandle h1 = reg.Acquire(a);
    MaterialHandle h2 = reg.Acquire(a); // refcount 2, same slot
    reg.Release(h1);
    CHECK(sink.freeCount == 0);         // still referenced
    CHECK(reg.ResolveSlot(h2) == h2.index);
    reg.Release(h2);
    CHECK(sink.freeCount == 1);         // now freed
}

TEST_CASE("Generation invalidates stale handles after free+realloc") {
    FakeSlotSink sink(1); // single slot forces reuse
    MaterialRegistry reg(sink);

    MaterialAsset red; red.baseColorFactor = {1,0,0,1};
    MaterialHandle stale = reg.Acquire(red);
    reg.Release(stale); // slot 0 freed, generation bumped

    MaterialAsset blue; blue.baseColorFactor = {0,0,1,1};
    MaterialHandle fresh = reg.Acquire(blue); // reuses slot 0, new generation

    CHECK(fresh.IsValid());
    CHECK(fresh.index == stale.index);
    CHECK(fresh.generation != stale.generation);
    CHECK(reg.ResolveSlot(stale) == reg.ResolveSlot(reg.DefaultHandle()));
    CHECK(reg.ResolveSlot(fresh) == fresh.index);
}

TEST_CASE("Default handle resolves; invalid handle falls back to default") {
    FakeSlotSink sink(8);
    MaterialRegistry reg(sink);
    MaterialAsset def; def.baseColorFactor = {0.85f,0.85f,0.82f,1.0f};
    reg.InitializeDefault(def);

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(MaterialHandle{}) == reg.DefaultHandle().index);
}
```

- [x] **Step 2: Run — verify pass** (Release/generation/default already implemented in Task 5).

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests && ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: PASS. If the stale-handle test fails because `m_defaultSlot` is unset (no `InitializeDefault` in the first two cases), note ResolveSlot returns `0xFFFFFFFF` there — that is the intended "no default configured" sentinel; the third test exercises the configured default.

- [x] **Step 3: Commit.**

```bash
git add tests/material/MaterialRegistryTests.cpp
git commit -m "test(material): registry refcount, generation, default fallback"
```

---

## Task 7: Own the registry (AssetSubsystem) + default material

**Files:**
- Modify: `src/engine/assets/AssetSubsystem.hpp`, `src/engine/assets/AssetSubsystem.cpp`
- Modify: `src/engine/AetherCore.cpp:95` (service registration)

- [x] **Step 1: Add a `MaterialRegistry` member to `AssetSubsystem.hpp`** (next to `m_materialBuffer`, line 72), constructed from the buffer, plus a getter:

```cpp
#include "material/MaterialRegistry.hpp"
// ...
		[[nodiscard]] MaterialRegistry& GetMaterialRegistry() { return m_materialRegistry; }
// ...
		MaterialBuffer m_materialBuffer;
		MaterialRegistry m_materialRegistry{m_materialBuffer};
```

(Member order: `m_materialRegistry` MUST be declared after `m_materialBuffer` so the reference binds to a constructed buffer.)

- [x] **Step 2: Initialize the default material** in `AssetSubsystem.cpp` after `m_materialBuffer.Initialize();` (line 34):

```cpp
		MaterialAsset defaultAsset;
		defaultAsset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
		defaultAsset.roughnessFactor = 0.6f;
		defaultAsset.metallicFactor = 0.0f;
		m_materialRegistry.InitializeDefault(defaultAsset);
```

Add `#include "material/MaterialAsset.hpp"` at the top of `AssetSubsystem.cpp`.

- [x] **Step 3: Register the service** in `AetherCore.cpp` next to the MaterialBuffer registration (line 95):

```cpp
		m_services.Register<MaterialRegistry>(assetsSub.GetMaterialRegistry());
```

Add `#include "material/MaterialRegistry.hpp"` near the MaterialBuffer include (line 32).

- [x] **Step 4: Compile-check.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds.

- [x] **Step 5: Commit.**

```bash
git add src/engine/assets/AssetSubsystem.hpp src/engine/assets/AssetSubsystem.cpp src/engine/AetherCore.cpp
git commit -m "feat(material): AssetSubsystem owns MaterialRegistry + default material"
```

---

## Task 8: `MaterialComponent` holds handle + cached slot; leak-proof release

**Files:**
- Modify: `src/engine/scene/Components.hpp:31-35`
- Modify: `src/engine/rendering/WorldRenderer.cpp:5,47-51`
- Create: `src/engine/material/MaterialSystem.hpp`, `src/engine/material/MaterialSystem.cpp` (assign helper + entt on_destroy hook)

- [x] **Step 1: Change `MaterialComponent`** in `Components.hpp` (replace the `Material material{}` body):

```cpp
#include "material/MaterialHandle.hpp"
// ...
	// Reference to a registry material + the cached GPU slot for the render loop.
	// The handle is authoritative for lifetime; gpuSlot is refreshed on assignment.
	struct MaterialComponent
	{
		MaterialHandle handle{};
		std::uint32_t gpuSlot = 0xFFFFFFFFu;
	};
```

Remove the now-unused `#include "material/Material.hpp"` from `Components.hpp`.

- [x] **Step 2: Create `src/engine/material/MaterialSystem.hpp`:**

```cpp
#pragma once

#include "scene/Entity.hpp"

namespace aether
{
	class World;
	class MaterialRegistry;
	struct MaterialAsset;

	namespace MaterialSystem
	{
		// Connect the entt on_destroy hook so material handles are released when a
		// MaterialComponent is destroyed or the world is cleared. Call once at setup.
		void ConnectLifecycle(World& world, MaterialRegistry& registry);

		// Assign a material to an entity: release any previous handle, acquire the
		// new asset, store handle + cached slot. Emplaces the component if absent.
		void AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, const MaterialAsset& asset);
	} // namespace MaterialSystem
} // namespace aether
```

- [x] **Step 3: Create `src/engine/material/MaterialSystem.cpp`:**

```cpp
#include "material/MaterialSystem.hpp"

#include <entt/entt.hpp>

#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		MaterialRegistry* g_registry = nullptr;

		void OnMaterialDestroyed(entt::registry& r, entt::entity e)
		{
			if (g_registry == nullptr) return;
			const MaterialComponent& mc = r.get<MaterialComponent>(e);
			g_registry->Release(mc.handle);
		}
	} // namespace

	void MaterialSystem::ConnectLifecycle(World& world, MaterialRegistry& registry)
	{
		g_registry = &registry;
		world.GetRegistry().on_destroy<MaterialComponent>().connect<&OnMaterialDestroyed>();
	}

	void MaterialSystem::AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, const MaterialAsset& asset)
	{
		auto& r = world.GetRegistry();
		const entt::entity e = World::ToEntt(entity);
		if (auto* existing = r.try_get<MaterialComponent>(e))
		{
			registry.Release(existing->handle);
		}
		const MaterialHandle h = registry.Acquire(asset);
		r.emplace_or_replace<MaterialComponent>(e, MaterialComponent{h, registry.ResolveSlot(h)});
	}
} // namespace aether
```

> `emplace_or_replace` fires `on_destroy` for the replaced component, so the old handle is released by the hook; the explicit `Release` above covers the pre-read of the previous handle. If double-release is observed in testing, drop the explicit `Release` and rely solely on the hook. (The registry's `Release` is generation-guarded, so a redundant release of an already-freed handle is a safe no-op.)

- [x] **Step 4: Update `WorldRenderer.cpp`** — replace lines 5 and 47-51.

Replace `#include "material/Material.hpp"` with `#include "scene/Components.hpp"` (already included) and use the cached slot:

```cpp
			std::uint32_t materialIndex = 0xFFFFFFFFu;
			if (const auto material = world.GetRegistry().try_get<MaterialComponent>(enttEntity))
			{
				materialIndex = material->gpuSlot;
			}
```

- [x] **Step 5: Connect the lifecycle hook.** In `AssetManager::Initialize` (`src/engine/assets/AssetManager.cpp:263-268`, which already receives `World& world` and the buffer), after storing members add:

```cpp
		MaterialSystem::ConnectLifecycle(world, m_materialRegistry);
```

(This requires `AssetManager` to hold a `MaterialRegistry*` — added in Task 9 Step 1. If sequencing here, defer this step's line to Task 9.)

- [x] **Step 6: Compile-check.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds (AssetManager/WorldModule still reference old paths — expect those specific errors, fixed in Tasks 9-10; build just the files touched here by temporarily building `EngineTests` is not possible, so proceed to Task 9 before a full Engine build).

- [x] **Step 7: Commit.**

```bash
git add src/engine/scene/Components.hpp src/engine/rendering/WorldRenderer.cpp src/engine/material/MaterialSystem.hpp src/engine/material/MaterialSystem.cpp
git commit -m "feat(material): MaterialComponent holds handle+slot; entt release hook"
```

---

## Task 9: Migrate `AssetManager` (GLTF / preset / binary) to the registry

**Files:**
- Modify: `src/engine/assets/AssetManager.hpp` (hold `MaterialRegistry*`)
- Modify: `src/engine/assets/AssetManager.cpp` (`RegisterMaterial` bodies + callers)

- [x] **Step 1: Give `AssetManager` the registry.** In `AssetManager.hpp` add a member `MaterialRegistry* m_materialRegistry = nullptr;` and a param to `Initialize`. In `AssetManager.cpp:263`, change the signature to also accept `MaterialRegistry& materialRegistry` and store it; update the caller in `AssetSubsystem.cpp` (pass `m_materialRegistry`). Add `#include "material/MaterialRegistry.hpp"` and `#include "material/MaterialSystem.hpp"`.

- [x] **Step 2: Replace the material type in AssetManager.** Every `Material` becomes `MaterialAsset`; every `RegisterMaterial(mat)` (which returned via `mat.materialSlot`) becomes `const MaterialHandle h = m_materialRegistry->Acquire(mat);` and the code that consumed `materialSlot` now consumes the handle. Concretely, the GLTF path that builds a `Material` per primitive and stored `materialSlot` on the mesh entity must instead call `MaterialSystem::AssignMaterial(world, entity, *m_materialRegistry, asset)` (or store the handle where the mesh spawns the entity). Replace the three `RegisterMaterial(...)` call sites (`AssetManager.cpp:466,544,753`) accordingly.

- [x] **Step 3: Delete `AssetManager::RegisterMaterial` / `UnregisterMaterial`** (`AssetManager.cpp:311-365`) — superseded by the registry. Update the header declarations.

- [x] **Step 4: Compile-check.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds (WorldModule/EffectsModule still reference old paths — fixed in Task 10).

- [x] **Step 5: Commit.**

```bash
git add src/engine/assets/AssetManager.hpp src/engine/assets/AssetManager.cpp src/engine/assets/AssetSubsystem.cpp
git commit -m "refactor(material): AssetManager uses MaterialRegistry (no smuggled slot)"
```

---

## Task 10: Migrate das default material + effects; delete `Material`

**Files:**
- Modify: `src/app/scripting/modules/WorldModule.cpp:401-440`
- Modify: `src/app/scripting/modules/EffectsModule.cpp:15-73`
- Modify: `src/engine/scene/EcsHelpers.hpp` (if it references `Material`)
- Delete: `src/engine/material/Material.hpp`

- [x] **Step 1: WorldModule default material.** The scene context currently holds a `Material defaultMaterial`. Replace with a cached `MaterialAsset defaultAsset` (set `modulateVertexColor = true` so primitives keep their vertex colors) and, in `das_add_mesh` (line 423-441), assign via the registry:

```cpp
		// entry no longer stores a Material; it stores a MaterialAsset.
		aether::MaterialSystem::AssignMaterial(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), entry.materialAsset);
```

Update `SceneContext::CachedMesh` to hold `MaterialAsset materialAsset` instead of `Material material`, and drop the lazy `RegisterMaterial` block (the registry's default now covers the fallback).

- [x] **Step 2: EffectsModule.** `apply_effect` sets a `MaterialComponent{.material = effect->material}` (line 33) and `set_effect_color/speed/intensity` mutate material fields (lines 43-73). Convert the effect's stored `Material` to a `MaterialAsset`, and route assignment through `MaterialSystem::AssignMaterial`. For the mutating setters, rebuild the asset with the new field and re-assign (immutable model → re-acquire). This preserves behavior; the deeper effects/material-shader unification is ③.

- [x] **Step 3: Delete `Material.hpp`** and fix any remaining includes (search: `grep -rn "material/Material.hpp" src`). Replace with `MaterialAsset.hpp`.

- [x] **Step 4: Full build.**

Run: `cmake --build build-vs2022-msvc --config Debug`
Expected: builds clean (Engine + App).

- [x] **Step 5: Run unit tests.**

Run: `ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: PASS.

- [x] **Step 6: Commit.**

```bash
git add -A
git commit -m "refactor(material): migrate das default + effects to registry; remove Material"
```

---

## Task 11: Shader — honor `kModulateVertexColor`

**Files:**
- Modify: `shaders/include/GpuMaterial.slangh:6-9`
- Modify: `shaders/gltf_mesh.slang:65-71`

- [x] **Step 1: Add the flag** to `GpuMaterial.slangh` after `kFlagAlphaMask` (line 9):

```hlsl
static const uint kFlagModulateVertexColor = 8u;
```

- [x] **Step 2: Use the flag** in `gltf_mesh.slang`. Replace the vertex-color block (lines 65-71):

```hlsl
    // Vertex color modulates base color only when the material opts in
    // (primitive meshes with meaningful vertex colors). Painted materials clear it.
    float3 vertexColor = float3(1.0, 1.0, 1.0);
    if (hasMaterial && (mat.flags & kFlagModulateVertexColor) != 0u)
    {
        vertexColor = input.vertexColor;
    }
    else if (!hasMaterial)
    {
        vertexColor = input.vertexColor;
    }
    if (any(vertexColor < 0.0) || any(vertexColor > 1.0) || dot(vertexColor, vertexColor) < 0.0025)
    {
        vertexColor = float3(1.0, 1.0, 1.0);
    }
    baseColor.rgb *= vertexColor;
```

- [x] **Step 3: Compile shaders.**

Run: `cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders`
Expected: `.spv` recompiles without errors.

- [x] **Step 4: Commit.**

```bash
git add shaders/include/GpuMaterial.slangh shaders/gltf_mesh.slang
git commit -m "feat(material): shader honors kModulateVertexColor flag"
```

---

## Task 12: Final build, cleanup, hand-off checklist

- [x] **Step 1: Grep for stragglers.**

Run: `grep -rn "materialSlot\|material/Material.hpp\|\.material\b" src`
Expected: no references to the deleted `Material`/`materialSlot`. Fix any found.

- [x] **Step 2: Full clean build + tests.**

Run: `cmake --build build-vs2022-msvc --config Debug && ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`
Expected: Engine + App build clean; EngineTests pass.

- [x] **Step 3: Commit.**

```bash
git add -A
git commit -m "chore(material): remove material-slot stragglers"
```

- [ ] **Step 4: Runtime hand-off checklist (user runs the GPU app):**
  - Existing scenes render unchanged: rainbow cubes still rainbow (vertex-color flag), GLTF models (Fox/Human) correct, plasma effect intact.
  - Paint many entities the same explicit color → only one material slot consumed (no "MaterialBuffer is full" warning); different colors → different slots.
  - Hot-reload / reload the scene repeatedly → no growth in used material slots over time (no handle leak).
  - Toggle the debug material inspector (if present) → default material reads 0.85 gray, roughness 0.6.

---

## Self-Review

- **Spec coverage:** §4.A → Tasks 2-4; §4.B → Tasks 5-7; §4.C → Task 8; §4.D → Task 11; §4.E → Tasks 9-10; §4.F → Task 1 (+ TDD in 3,5,6). All covered.
- **Refinement noted:** §4.C resolve-at-draw → cached slot on component (documented in header + Task 8).
- **Type consistency:** `MaterialHandle{index,generation}`, `MaterialAsset`, `IMaterialSlotSink{AllocateSlot,FreeSlot,Write,Capacity}`, `MaterialRegistry{Acquire,Release,ResolveSlot,InitializeDefault,DefaultHandle}`, `MaterialComponent{handle,gpuSlot}` used consistently across tasks.
- **Open risk flagged in-plan:** double-release in `AssignMaterial` (Task 8 Step 3 note) — mitigated by generation-guarded `Release`.
