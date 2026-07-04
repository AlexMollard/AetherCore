# Texture Asset Layer (④) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give textures the same handle-based, ref-counted, deduplicated ownership model phase ① gave materials. Introduce `TextureHandle` + `TextureRegistry` (mirroring `MaterialHandle` / `MaterialRegistry`), thread it through `PackMaterial` so texture identity is a handle at authoring time and a raw bindless heap index only at pack time, and replace the two keep-alive vectors (`LoadedModel::textures`, `LoadMaterialPreset`'s `outTextures`) with registry ownership. Engine-side only. **Async/streaming is out of scope (④b)** — v1 acquires blocking.

**Architecture:** A new `TextureRegistry` owns `Texture` objects behind opaque `TextureHandle`s over an `ITextureSlotSink` interface. `Acquire(path)` resolves the VFS path via the sink, hashes the *resolved* string (FNV-1a, same as `MaterialRegistry`), and either bumps a refcount (dedup) or loads + stores a new entry. `Release` decrements; at zero it destroys the `Texture`, which frees the bindless slot through the existing kMaxFramesInFlight-deferred path. `ResolveSlot(handle)` returns the entry's live bindless heap index, or the magenta default slot for a stale handle, or `GpuMaterial::kNoTexture` for a default-constructed (optional-map) handle. `MaterialAsset` stores five `TextureHandle`s instead of five raw `uint32` slots. `MaterialRegistry::Acquire`/`Release` cascade texture refcounts (materials ref-count textures), and the entt `on_destroy<MaterialComponent>` hook is the backstop.

**CRITICAL INVARIANT — GPU/shader ABI frozen.** `GpuMaterial` stays 80 bytes with every `static_assert` offset intact (`GpuMaterial.hpp:48-61`). `PackMaterial` remains the single CPU→GPU packing path and still emits a **raw bindless heap-index `uint32`** into every `*Slot` field — texture handles resolve to indices at pack time. `gltf_mesh.slang` and `shaders/include/GpuMaterial.slangh` are **not modified**.

**Tech Stack:** C++23/26, CMake + CPM, doctest (`tests/material/`), entt ECS, Vulkan 1.4 (VK_EXT_descriptor_heap), Slang shaders. Depends on ① (merged).

**Spec:** `docs/superpowers/specs/2026-07-04-texture-asset-layer-design.md`

---

## Refinements vs spec (grounded in current code)

The spec was drafted against slightly older line numbers; the following reconcile it with the tree as it stands. None change the design, only where the seam lands.

1. **§4.C sink return type (the spec left this as an open choice).** The spec's `ITextureSlotSink::Load` returns `Expected<Texture>`, but `Texture` owns a live `gpu::TextureHandle` a fake sink cannot mint. **Decision: option (b)** — the sink returns a move-only `TextureResource` that the `Entry` stores. Production wraps a real `Texture`; `FakeTextureSink` wraps a plain `uint32` slot. `TextureResource::GetBindlessSlot()` is the one method the registry calls, so the registry stays 100% GPU-free and unit-testable, exactly like `MaterialRegistry` over `IMaterialSlotSink`. (Option (a), a test-only `Texture` factory that pokes `m_bindlessSlot`, is rejected — it leaks a GPU-owning type into tests.)

2. **§5 cascade seam.** The spec says "`MaterialRegistry::Acquire`/`Release` bump/drop texture refs." In this tree the material acquire/release discipline actually lives in **two** places that must both cascade: `MaterialRegistry::Acquire`/`Release` themselves (so dedup-hit and refcount-zero paths are covered), storing the five `TextureHandle`s on the material `SlotEntry` (`MaterialRegistry.hpp:33-40`). The entt `on_destroy<MaterialComponent>` hook (`MaterialSystem.cpp:14-17`) already funnels through `MaterialRegistry::Release`, so it inherits the cascade for free — no separate hook edit.

3. **`LoadedModel` keep-alive is already half-gone.** `LoadedModelPrimitive` already stores a `MaterialAsset material` (authoring data, `LoadedModel.hpp:19`) and `SpawnModel` already acquires a registry handle per spawned entity via `ecs::SpawnMesh(..., *m_materialRegistry, primitive.material, ...)` (`AssetManager.cpp:745`). Only `LoadedModel::textures` (`LoadedModel.hpp:27`) and its populate site in `FinaliseModelLoad` (`AssetManager.cpp:623`) remain to be deleted.

4. **`LoadMaterialPreset` already returns `Expected<MaterialAsset>`** (`AssetManager.hpp:59`) — only the `std::vector<Texture>& outTextures` out-param and the two loader lambdas need rewiring. Verified: no in-tree caller passes `outTextures` (grep hits are the declaration + definition only), so dropping it is a clean signature change.

5. **`BindlessManager::AdvanceFrame` is already pumped once per frame** at `AetherCore.cpp:570` and `:685`, so texture slot frees ride the existing clock with **no new per-frame pump** (spec §4.D). The only new teardown concern is ordering the registry's destruction before `MaterialBuffer::Shutdown()` and flushing the deferred ring at shutdown (Task 8).

---

## File Structure

**New files (engine — auto-globbed, no CMake edit):**
- `src/engine/material/TextureHandle.hpp` — opaque `{index,generation}` handle (mirrors `MaterialHandle.hpp`).
- `src/engine/material/ITextureSlotSink.hpp` — sink interface + `TextureResource` move-only slot holder.
- `src/engine/material/TextureRegistry.hpp` / `.cpp` — the registry.
- `src/engine/material/AssetTextureSink.hpp` / `.cpp` — production sink backed by `AssetManager`/`Texture::LoadFromFile`.

**New files (tests):**
- `tests/material/FakeTextureSink.hpp`, `tests/material/TextureRegistryTests.cpp`.

**Modified:**
- `src/engine/material/MaterialAsset.hpp:29-33` (five `uint32` slots → five `TextureHandle`).
- `src/engine/material/MaterialPacking.hpp:10` / `.cpp:24-28` (add `const TextureRegistry&`; resolve handles → heap index).
- `src/engine/material/MaterialRegistry.hpp:21,27-28,33-40` / `.cpp` (forward `TextureRegistry&`; cascade texture refs; store handles on `SlotEntry`).
- `src/engine/material/MaterialInstance*` / typed setters (`MaterialSystem.cpp`) — recompiled against handle-typed `MaterialAsset` (no field access to `*Slot` uints remains).
- `src/engine/assets/AssetManager.hpp` / `.cpp` — `FinaliseModelLoad` (`:584-624,674-695`), `LoadMaterialPreset` (`:314,376-390,424-457,479-488`), own the sink + registry, wire cascade.
- `src/engine/assets/AssetSubsystem.hpp:85-96` / `.cpp:36-42,98-121` (own `AssetTextureSink` + `TextureRegistry` **before** `MaterialRegistry`; teardown + shutdown flush).
- `src/engine/scene/LoadedModel.hpp:27` (delete `std::vector<Texture> textures`).
- `tests/CMakeLists.txt:1-9` (register `TextureRegistryTests.cpp`).

**Untouched (ABI freeze — verify at the end):** `src/engine/material/GpuMaterial.hpp`, `shaders/gltf_mesh.slang`, `shaders/include/GpuMaterial.slangh`.

---

## Task 1: `TextureHandle` + `ITextureSlotSink` + `TextureResource`

**Files:**
- Create: `src/engine/material/TextureHandle.hpp`
- Create: `src/engine/material/ITextureSlotSink.hpp`

- [ ] **Step 1: Create `src/engine/material/TextureHandle.hpp`** — a direct copy of `MaterialHandle.hpp`'s shape. `index` is a `TextureRegistry` entry index, **never** a bindless heap index:

```cpp
#pragma once

#include <cstdint>

namespace aether
{
	// Opaque, ref-counted texture reference. index = TextureRegistry ENTRY index
	// (never a bindless heap slot); generation guards against use-after-release.
	// Default-constructed handles are invalid (the "optional map not set" case).
	struct TextureHandle
	{
		static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

		std::uint32_t index = kInvalidIndex;
		std::uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const { return index != kInvalidIndex; }

		friend bool operator==(const TextureHandle& a, const TextureHandle& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}
	};
} // namespace aether
```

- [ ] **Step 2: Create `src/engine/material/ITextureSlotSink.hpp`** — the test seam. No Vulkan includes. `TextureResource` is a move-only holder so the registry stores something GPU-free in tests (Refinement 1):

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "material/Texture.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	// Move-only holder the TextureRegistry Entry owns. Production wraps a real
	// Texture (owns a gpu::TextureHandle + bindless slot); the test fake wraps a
	// bare slot. The registry only ever calls GetBindlessSlot(), so it never
	// needs a live GPU. Mirrors how MaterialRegistry stores a plain slot index.
	class TextureResource
	{
	public:
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		TextureResource() = default;
		explicit TextureResource(Texture&& texture) : m_texture(std::move(texture)), m_slot(m_texture.GetBindlessSlot()) {}
		// Test/GPU-free construction: a bare slot with no backing Texture.
		explicit TextureResource(std::uint32_t slot) : m_slot(slot) {}

		TextureResource(const TextureResource&) = delete;
		TextureResource& operator=(const TextureResource&) = delete;
		TextureResource(TextureResource&&) noexcept = default;
		TextureResource& operator=(TextureResource&&) noexcept = default;

		[[nodiscard]] std::uint32_t GetBindlessSlot() const { return m_slot; }

	private:
		Texture m_texture{};                 // empty (no-op Destroy) for the bare-slot path
		std::uint32_t m_slot = kInvalidSlot;
	};

	// Backend that resolves + loads textures for the registry. AssetTextureSink is
	// the production impl; tests inject a fake. No Vulkan types leak through.
	class ITextureSlotSink
	{
	public:
		virtual ~ITextureSlotSink() = default;

		// Resolve the VFS path to the canonical string the sink will actually load
		// (after .texture-sibling + mount normalization). The registry hashes and
		// dedups on THIS string, not the raw argument (spec §4.C - CRITICAL).
		[[nodiscard]] virtual std::string ResolvePath(std::string_view path) const = 0;

		// Blocking (v1): decode + upload; returns a resident TextureResource that
		// owns its bindless slot, or an error. Called with the already-resolved path.
		[[nodiscard]] virtual Expected<TextureResource> Load(std::string_view resolvedPath) = 0;

		// Max number of live entries the sink can back (bindless capacity).
		[[nodiscard]] virtual std::uint32_t Capacity() const = 0;
	};
} // namespace aether
```

> `Texture{}` default-constructs with an invalid `gpu::TextureHandle`, so its destructor is a no-op (`Texture.cpp:313-321`) — the bare-slot path holds no GPU resource. This keeps `TextureResource` move-only and correct in both production and tests.

- [ ] **Step 3: Sync LSP + commit.**

Run: `/sync-lsp` (new headers)

```bash
git add src/engine/material/TextureHandle.hpp src/engine/material/ITextureSlotSink.hpp
git commit -m "feat(material): add TextureHandle + ITextureSlotSink + TextureResource"
```

---

## Task 2: `TextureRegistry` — Acquire + path dedup (TDD)

**Files:**
- Create: `src/engine/material/TextureRegistry.hpp`, `src/engine/material/TextureRegistry.cpp`
- Create: `tests/material/FakeTextureSink.hpp`, `tests/material/TextureRegistryTests.cpp`
- Modify: `tests/CMakeLists.txt:1-9`

- [ ] **Step 1: Create the fake sink `tests/material/FakeTextureSink.hpp`** (mirrors `FakeSlotSink.hpp`):

```cpp
#pragma once
#include <string>
#include <unordered_map>
#include "material/ITextureSlotSink.hpp"

// In-memory texture backend for registry tests. No GPU. ResolvePath() maps two
// spellings ("a.png", "a.texture") to one canonical string so dedup can be tested.
class FakeTextureSink final : public aether::ITextureSlotSink
{
public:
	explicit FakeTextureSink(std::uint32_t capacity = 8) : m_capacity(capacity) {}

	std::string ResolvePath(std::string_view path) const override
	{
		// Collapse a ".png"/".texture" pair to the same canonical stem, mirroring
		// Texture.cpp's .texture-sibling resolution.
		std::string s(path);
		const auto dot = s.find_last_of('.');
		if (dot != std::string::npos)
		{
			return s.substr(0, dot) + ".texture";
		}
		return s;
	}

	aether::Expected<aether::TextureResource> Load(std::string_view resolvedPath) override
	{
		if (m_live >= m_capacity)
		{
			return std::unexpected(aether::AetherError::Engine("FakeTextureSink: at capacity"));
		}
		++loadCount;
		++m_live;
		return aether::TextureResource{m_nextSlot++};
	}

	std::uint32_t Capacity() const override { return m_capacity; }

	int loadCount = 0;
	std::uint32_t m_nextSlot = 0;
private:
	std::uint32_t m_capacity;
	std::uint32_t m_live = 0;
};
```

> Note: the fake never reclaims `m_live`/`m_nextSlot` on release (the registry frees the `TextureResource`, not the sink) — fine for these tests, which assert entry-level dedup/refcount, not slot recycling. If a capacity-after-release test is added later, have `TextureRegistry::Release` call an optional `sink.Free(slot)` hook; not needed for v1.

- [ ] **Step 2: Write failing tests `tests/material/TextureRegistryTests.cpp`:**

```cpp
#include <doctest/doctest.h>
#include "material/TextureRegistry.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("Acquire returns a valid handle and loads once") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle h = reg.Acquire("brick.png");
    CHECK(h.IsValid());
    CHECK(sink.loadCount == 1);
    CHECK(reg.ResolveSlot(h) != TextureResource::kInvalidSlot);
}

TEST_CASE("Same resolved path dedups; two spellings still dedup (the §4.C test)") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("brick.png");     // exact same string
    TextureHandle c = reg.Acquire("brick.texture");  // different spelling, same resolved path

    CHECK(a == b);
    CHECK(a == c);                 // deduped because ResolvePath runs BEFORE hashing
    CHECK(sink.loadCount == 1);    // loaded exactly once
    CHECK(reg.ResolveSlot(a) == reg.ResolveSlot(c));
}

TEST_CASE("Distinct paths get distinct entries and slots") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("moss.png");

    CHECK(a.index != b.index);
    CHECK(reg.ResolveSlot(a) != reg.ResolveSlot(b));
    CHECK(sink.loadCount == 2);
}
```

- [ ] **Step 3: Register the test file** — add to `tests/CMakeLists.txt` `add_executable(EngineTests ...)` list (after `material/MaterialAuthoringTests.cpp`):

```cmake
    material/TextureRegistryTests.cpp
```

- [ ] **Step 4: Run to verify it fails.**

Run: `cmake --build --preset vs2022-msvc --target EngineTests`
Expected: FAIL — `TextureRegistry.hpp` not found.

- [ ] **Step 5: Create `src/engine/material/TextureRegistry.hpp`** (mirrors `MaterialRegistry.hpp:33-48`; the `Entry` owns a `TextureResource` and remembers its resolved path):

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "material/ITextureSlotSink.hpp"
#include "material/TextureHandle.hpp"

namespace aether
{
	// Ref-counted, PATH-addressed, deduplicated texture table over a slot sink.
	// The entry owns the TextureResource (which owns the bindless slot);
	// handle.index is the ENTRY index; ResolveSlot returns the live heap slot.
	// Diverges from MaterialRegistry (content-addressed) because a texture's
	// identity is its VFS path - you cannot hash the pixels before paying for
	// the decode you are deduping away (spec §3).
	//
	// Thread-safe: all public methods lock m_mutex. ResourceRegistry /
	// BindlessManager lock internally.
	class TextureRegistry
	{
	public:
		explicit TextureRegistry(ITextureSlotSink& sink);

		// Acquire a magenta 1x1 fallback once; its slot is returned by ResolveSlot
		// for STALE handles (visible error, not silently untextured).
		void InitializeDefault(std::string_view magentaPath);

		[[nodiscard]] TextureHandle Acquire(std::string_view path); // blocking (v1)
		void Release(TextureHandle handle);
		[[nodiscard]] std::uint32_t ResolveSlot(TextureHandle handle) const;
		[[nodiscard]] TextureHandle DefaultHandle() const { return m_defaultHandle; }

	private:
		struct Entry
		{
			std::string resolvedPath;   // dedup key (post-resolution)
			std::uint64_t hash = 0;
			TextureResource texture{};  // owns the bindless slot
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
		};

		ITextureSlotSink& m_sink;
		mutable std::mutex m_mutex;
		std::vector<Entry> m_entries;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_hashToEntry;
		TextureHandle m_defaultHandle{};
		std::uint32_t m_defaultSlot = 0xFFFFFFFFu;
	};
} // namespace aether
```

- [ ] **Step 6: Create `src/engine/material/TextureRegistry.cpp`** (mirrors `MaterialRegistry.cpp` exactly; resolve-before-hash is the load-bearing difference):

```cpp
#include "material/TextureRegistry.hpp"

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		std::uint64_t HashString(std::string_view s)
		{
			// FNV-1a 64-bit, same constants as MaterialRegistry.cpp:13-21.
			// Collisions resolved by string-compare on hit.
			std::uint64_t h = 1469598103934665603ull;
			for (const char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
			return h;
		}
	} // namespace

	TextureRegistry::TextureRegistry(ITextureSlotSink& sink) : m_sink(sink)
	{
		m_entries.reserve(sink.Capacity());
	}

	void TextureRegistry::InitializeDefault(std::string_view magentaPath)
	{
		const TextureHandle h = Acquire(magentaPath);
		std::scoped_lock lock(m_mutex);
		m_defaultHandle = h;
		m_defaultSlot = h.IsValid() ? m_entries[h.index].texture.GetBindlessSlot() : 0xFFFFFFFFu;
	}

	TextureHandle TextureRegistry::Acquire(std::string_view path)
	{
		// CRITICAL (spec §4.C): resolve BEFORE hashing so two spellings that
		// resolve to the same file share one entry.
		const std::string resolved = m_sink.ResolvePath(path);
		const std::uint64_t hash = HashString(resolved);

		std::scoped_lock lock(m_mutex);

		auto range = m_hashToEntry.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			Entry& e = m_entries[it->second];
			if (e.alive && e.resolvedPath == resolved) // string-compare on hit
			{
				++e.refcount;
				return TextureHandle{it->second, e.generation};
			}
		}

		// Miss: blocking load. On failure OR at capacity, return an INVALID handle
		// (ResolveSlot falls back to default, Release is a no-op) - never the
		// default handle (that would let a later Release free the default).
		auto loaded = m_sink.Load(resolved);
		if (!loaded)
		{
			AE_WARN(LogCategory::Asset, "TextureRegistry: load failed '{}': {}", resolved, loaded.error().what());
			return TextureHandle{};
		}

		// Reuse a dead slot if one exists (keeps m_entries compact); else append.
		std::uint32_t index = 0xFFFFFFFFu;
		for (std::uint32_t i = 0; i < m_entries.size(); ++i)
		{
			if (!m_entries[i].alive && m_entries[i].refcount == 0) { index = i; break; }
		}
		if (index == 0xFFFFFFFFu)
		{
			index = static_cast<std::uint32_t>(m_entries.size());
			m_entries.emplace_back();
		}

		Entry& e = m_entries[index];
		e.resolvedPath = resolved;
		e.hash = hash;
		e.texture = std::move(*loaded);
		e.refcount = 1;
		e.alive = true;
		m_hashToEntry.emplace(hash, index);
		return TextureHandle{index, e.generation};
	}

	void TextureRegistry::Release(TextureHandle handle)
	{
		if (!handle.IsValid() || handle.index >= m_entries.size()) return;

		std::scoped_lock lock(m_mutex);
		Entry& e = m_entries[handle.index];
		if (!e.alive || e.generation != handle.generation) return; // stale/double-free
		if (--e.refcount > 0) return;

		auto range = m_hashToEntry.equal_range(e.hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second == handle.index) { m_hashToEntry.erase(it); break; }
		}
		e.alive = false;
		++e.generation;                 // invalidate outstanding handles
		e.resolvedPath.clear();
		e.texture = TextureResource{};  // destroys the Texture -> deferred slot free (§4.D)
	}

	std::uint32_t TextureRegistry::ResolveSlot(TextureHandle handle) const
	{
		std::scoped_lock lock(m_mutex);
		if (handle.IsValid() && handle.index < m_entries.size())
		{
			const Entry& e = m_entries[handle.index];
			if (e.alive && e.generation == handle.generation) return e.texture.GetBindlessSlot();
		}
		return m_defaultSlot; // stale/invalid -> magenta (visible error)
	}
} // namespace aether
```

> `Release` destroying `e.texture` (assigning a fresh empty `TextureResource`) runs `Texture::Destroy` → `ResourceRegistry::Destroy(handle)` → `FreeSampledImageSlotDeferred` (`ResourceRegistry.cpp:1006-1008`, `BindlessManager.hpp:88`), released `deferredFreeFrames` later on the existing `AdvanceFrame` clock. No new deferral machinery (spec §4.D). In the fake sink the empty resource is a plain no-op.

- [ ] **Step 7: Run to verify it passes.**

Run: `cmake --build --preset vs2022-msvc --target EngineTests && ctest --test-dir out/build/vs2022-msvc -C RelWithDebInfo --output-on-failure`
Expected: PASS (3 texture cases + all existing material cases).

> If the `ctest --test-dir` path differs on your machine, use the path CMake reported for the `vs2022-msvc` preset binary dir; run `EngineTests.exe` directly as a fallback.

- [ ] **Step 8: Sync LSP + commit.**

Run: `/sync-lsp`

```bash
git add src/engine/material/TextureRegistry.hpp src/engine/material/TextureRegistry.cpp tests/material/FakeTextureSink.hpp tests/material/TextureRegistryTests.cpp tests/CMakeLists.txt
git commit -m "feat(material): TextureRegistry Acquire + path-addressed dedup (TDD)"
```

---

## Task 3: `TextureRegistry` — Release, generation, default, capacity (TDD)

**Files:**
- Modify: `tests/material/TextureRegistryTests.cpp`

- [ ] **Step 1: Add failing tests** (Release/generation/default/capacity are already implemented in Task 2 — these lock the behavior in, mirroring `MaterialRegistryTests.cpp:37-93`):

```cpp
TEST_CASE("Refcount: shared entry survives one release, frees at zero") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);

    TextureHandle a = reg.Acquire("brick.png");
    TextureHandle b = reg.Acquire("brick.png"); // refcount 2, same entry
    reg.Release(a);
    CHECK(reg.ResolveSlot(b) != TextureResource::kInvalidSlot); // still resident
    reg.Release(b);
    CHECK(reg.ResolveSlot(b) == reg.ResolveSlot(reg.DefaultHandle())); // now stale -> default
}

TEST_CASE("Generation invalidates stale handles after free + reload") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    TextureHandle stale = reg.Acquire("brick.png");
    reg.Release(stale); // entry 0 dead, generation bumped

    TextureHandle fresh = reg.Acquire("moss.png"); // reuses entry 0, new generation
    CHECK(fresh.index == stale.index);
    CHECK(fresh.generation != stale.generation);
    CHECK(reg.ResolveSlot(stale) == reg.ResolveSlot(reg.DefaultHandle())); // stale -> default
    CHECK(reg.ResolveSlot(fresh) != reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Default handle resolves; invalid handle falls back to default") {
    FakeTextureSink sink(8);
    TextureRegistry reg(sink);
    reg.InitializeDefault("magenta.png");

    CHECK(reg.DefaultHandle().IsValid());
    CHECK(reg.ResolveSlot(TextureHandle{}) == reg.ResolveSlot(reg.DefaultHandle()));
}

TEST_CASE("Capacity-exhausted Acquire returns an invalid handle; its Release is a no-op") {
    FakeTextureSink sink(1);
    TextureRegistry reg(sink);
    reg.InitializeDefault("magenta.png"); // consumes the only slot

    TextureHandle h = reg.Acquire("brick.png"); // sink full
    CHECK_FALSE(h.IsValid());
    CHECK(reg.ResolveSlot(h) == reg.ResolveSlot(reg.DefaultHandle()));
    reg.Release(h); // must be a safe no-op
    CHECK(reg.ResolveSlot(reg.DefaultHandle()) != TextureResource::kInvalidSlot); // default intact
}
```

- [ ] **Step 2: Run — verify pass.**

Run: `cmake --build --preset vs2022-msvc --target EngineTests && ctest --test-dir out/build/vs2022-msvc -C RelWithDebInfo --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Commit.**

```bash
git add tests/material/TextureRegistryTests.cpp
git commit -m "test(material): texture registry refcount, generation, default, capacity"
```

---

## Task 4: `MaterialAsset` holds `TextureHandle`s; `PackMaterial` resolves → heap index (ABI frozen)

**Files:**
- Modify: `src/engine/material/MaterialAsset.hpp:29-33`
- Modify: `src/engine/material/MaterialPacking.hpp:7,10`, `src/engine/material/MaterialPacking.cpp:24-28`
- Modify: `tests/material/PackMaterialTests.cpp` (pack now takes a registry)

- [ ] **Step 1: Swap the five raw slots for handles** in `MaterialAsset.hpp` (replace lines 29-33; keep `kNoTexture` as the "optional map not set" sentinel, now expressed by a default-constructed handle):

```cpp
		TextureHandle albedoTex{};
		TextureHandle normalTex{};
		TextureHandle metallicRoughnessTex{};
		TextureHandle occlusionTex{};
		TextureHandle emissiveTex{};
```

Add `#include "material/TextureHandle.hpp"` and keep the existing `kNoTexture` constant (loaders still return it from the "missing texture" branches; it maps to a default-constructed handle — see Step 3 helper). `MaterialAsset` is a value type used as a dedup key by `MaterialRegistry` (it hashes the packed `GpuMaterial`, not the asset), so replacing uints with two-uint handles does not change the material hash path — but see Task 5's note on **what** gets packed.

- [ ] **Step 2: Give `PackMaterial` the registry** — `MaterialPacking.hpp`:

```cpp
	struct MaterialAsset;
	class TextureRegistry;

	// The single source of truth for CPU authoring -> GPU record conversion.
	// Resolves each TextureHandle to its live bindless heap index via the
	// registry; the GPU record still stores a raw uint32 slot (ABI frozen).
	[[nodiscard]] GpuMaterial PackMaterial(const MaterialAsset& asset, const TextureRegistry& textures);
```

- [ ] **Step 3: Resolve handles → heap index** in `MaterialPacking.cpp` (replace the verbatim copies at lines 24-28). A default-constructed handle must map to `GpuMaterial::kNoTexture` so `gltf_mesh.slang:61` skips the sample; a live handle maps to its slot; a stale handle maps to the magenta default (all three handled inside `ResolveSlot` + the default-vs-invalid split below):

```cpp
#include "material/TextureRegistry.hpp"
// ...
	GpuMaterial PackMaterial(const MaterialAsset& a, const TextureRegistry& textures)
	{
		// ... factors/flags unchanged ...

		// Optional map not set (default-constructed handle) -> kNoTexture so the
		// shader skips the sample. Otherwise the registry resolves to the live
		// heap slot, or the magenta default for a stale handle.
		auto slotFor = [&textures](const TextureHandle h) -> std::uint32_t
		{
			return h.IsValid() ? textures.ResolveSlot(h) : GpuMaterial::kNoTexture;
		};

		g.albedoSlot            = slotFor(a.albedoTex);
		g.normalSlot            = slotFor(a.normalTex);
		g.metallicRoughnessSlot = slotFor(a.metallicRoughnessTex);
		g.occlusionSlot         = slotFor(a.occlusionTex);
		g.emissiveSlot          = slotFor(a.emissiveTex);
		return g;
	}
```

> ABI stays frozen: `GpuMaterial` is untouched, `albedoSlot` stays at offset 52, `PackMaterial` stays the single packing path, `gltf_mesh.slang` is not modified. The static_asserts (`GpuMaterial.hpp:48-61`) still hold — verified at Task 9.

- [ ] **Step 4: Update `PackMaterialTests.cpp`** — pack now needs a `TextureRegistry`. Give the pack tests a `FakeTextureSink` + registry, acquire a texture, assign its handle, and assert the packed slot equals the sink's slot:

```cpp
#include "material/TextureRegistry.hpp"
#include "FakeTextureSink.hpp"
// ...
TEST_CASE("PackMaterial resolves handles to heap slots and freezes the 80-byte layout") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    static_assert(sizeof(GpuMaterial) == 80);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png");
    const std::uint32_t expected = tex.ResolveSlot(a.albedoTex);

    GpuMaterial g = PackMaterial(a, tex);
    CHECK(g.albedoSlot == expected);
    CHECK(g.normalSlot == GpuMaterial::kNoTexture); // default-constructed handle -> skip sample
}
```

Update the existing factor/flag pack cases to pass the registry too (`PackMaterial(a, tex)`); they no longer set `.albedoSlot`/`.emissiveSlot` uints (those fields are gone from `MaterialAsset`).

- [ ] **Step 5: Build EngineTests.**

Run: `cmake --build --preset vs2022-msvc --target EngineTests`
Expected: FAIL to LINK yet — `MaterialRegistry::Acquire` still calls `PackMaterial(asset)` with the old arity (fixed in Task 5). Confirm the *compile* errors are only the `PackMaterial` arity mismatch in `MaterialRegistry.cpp`, then proceed.

- [ ] **Step 6: Commit (WIP — engine not yet linkable; committed as a checkpoint).**

```bash
git add src/engine/material/MaterialAsset.hpp src/engine/material/MaterialPacking.hpp src/engine/material/MaterialPacking.cpp tests/material/PackMaterialTests.cpp
git commit -m "feat(material): MaterialAsset holds TextureHandles; PackMaterial resolves to heap slot"
```

---

## Task 5: `MaterialRegistry` forwards the `TextureRegistry` + cascades texture refs

**Files:**
- Modify: `src/engine/material/MaterialRegistry.hpp:21,27-28,33-40`
- Modify: `src/engine/material/MaterialRegistry.cpp:24-27,37-74,76-93`
- Modify: `tests/material/MaterialRegistryTests.cpp`, `tests/material/MaterialSystemTests.cpp`, `tests/material/MaterialInstanceTests.cpp`, `tests/material/MaterialAuthoringTests.cpp` (constructor now takes a `TextureRegistry&`)

- [ ] **Step 1: Add the `TextureRegistry&` to `MaterialRegistry.hpp`** and store the asset's five handles on `SlotEntry` so `Release` drops exactly those (spec §5 bookkeeping note — dedup-hit acquires must still bump texture refs, so the entry must remember them):

```cpp
	class TextureRegistry;
// ...
		explicit MaterialRegistry(IMaterialSlotSink& sink, TextureRegistry& textures);
// ...
		struct SlotEntry
		{
			GpuMaterial packed{};
			std::uint64_t hash = 0;
			std::uint32_t refcount = 0;
			std::uint32_t generation = 0;
			bool alive = false;
			// Texture refs this material holds, so Release drops exactly these
			// (dedup means Acquire of an already-resident material still bumps them).
			TextureHandle textures[5]{};
		};
// ...
		IMaterialSlotSink& m_sink;
		TextureRegistry& m_textures;
```

Add `#include "material/TextureHandle.hpp"` and forward-declare `TextureRegistry`.

- [ ] **Step 2: Rewire `MaterialRegistry.cpp`.** Constructor stores `m_textures`. `Acquire`:
  - Pack via `PackMaterial(asset, m_textures)` (`.cpp:39`).
  - **Bump texture refs for every non-null handle on the asset, on BOTH the dedup-hit and fresh-entry paths** (call `m_textures.Acquire(resolvedPathOfHandle)`? — NO: the asset already holds resident handles; re-`Acquire`-ing by path would re-resolve. Instead add `TextureRegistry::AddRef(TextureHandle)` that bumps an existing entry's refcount by generation-guarded index — a 4-line method mirroring the dedup-hit branch). Store the five handles on the new `SlotEntry`.
  - `Release`: for each stored non-null handle call `m_textures.Release(handle)`, then free the material slot as today.

Add to `TextureRegistry` a public `void AddRef(TextureHandle handle);` (generation-guarded `++m_entries[index].refcount`) — this is the "bump an already-resident texture" primitive the material cascade needs without a path round-trip. Add a matching test in `TextureRegistryTests.cpp`.

> Why `AddRef` and not `Acquire(path)`: the material already owns resolved handles; the texture entry is alive. `AddRef` bumps the refcount directly (O(1), generation-checked) — the exact mirror of `MaterialRegistry`'s dedup-hit `++e.refcount`. `Acquire` is only for turning a *path* into a handle (the loader's job).

- [ ] **Step 3: Update all `MaterialRegistry(sink)` construction sites** to `MaterialRegistry(sink, textures)`:
  - `AssetSubsystem.hpp:92` (Task 6 orders the members).
  - Every test that builds a `MaterialRegistry` (`MaterialRegistryTests.cpp`, `MaterialSystemTests.cpp`, `MaterialInstanceTests.cpp`, `MaterialAuthoringTests.cpp`) — each now also builds a `FakeTextureSink` + `TextureRegistry` and passes it. The pure material-dedup assertions are unchanged (materials with default-constructed texture handles pack identical `kNoTexture` slots → same bytes → same dedup behavior).

- [ ] **Step 4: Build EngineTests + run.**

Run: `cmake --build --preset vs2022-msvc --target EngineTests && ctest --test-dir out/build/vs2022-msvc -C RelWithDebInfo --output-on-failure`
Expected: PASS (all material + texture cases, including the new material→texture cascade).

- [ ] **Step 5: Add the cascade test** to `MaterialSystemTests.cpp` (materials ref-count textures; entity destroy cascades):

```cpp
TEST_CASE("Acquiring a material bumps its textures' refcounts; release drops them") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    FakeSlotSink matSink(8);
    MaterialRegistry mat(matSink, tex);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png"); // refcount 1 (loader ref)
    MaterialHandle m = mat.Acquire(a);       // bumps albedoTex -> refcount 2

    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle()));
    mat.Release(m);                          // drops albedoTex -> refcount 1
    CHECK(tex.ResolveSlot(a.albedoTex) != tex.ResolveSlot(tex.DefaultHandle())); // loader ref keeps it
    tex.Release(a.albedoTex);                // -> 0, freed
    CHECK(tex.ResolveSlot(a.albedoTex) == tex.ResolveSlot(tex.DefaultHandle()));
}
```

- [ ] **Step 6: Commit.**

```bash
git add src/engine/material/MaterialRegistry.hpp src/engine/material/MaterialRegistry.cpp src/engine/material/TextureRegistry.hpp src/engine/material/TextureRegistry.cpp tests/material/
git commit -m "feat(material): MaterialRegistry forwards TextureRegistry + cascades texture refs"
```

---

## Task 6: `AssetTextureSink` (production) + `AssetSubsystem` owns it before `MaterialRegistry`

**Files:**
- Create: `src/engine/material/AssetTextureSink.hpp`, `src/engine/material/AssetTextureSink.cpp`
- Modify: `src/engine/assets/AssetSubsystem.hpp:85-96`, `src/engine/assets/AssetSubsystem.cpp:36-42,98-121`

- [ ] **Step 1: Create `AssetTextureSink`** — the production sink. `ResolvePath` reproduces the `.texture`-sibling + mount-normalization logic (`Texture.cpp:265-280`); `Load` forwards to `Texture::LoadFromFile` and wraps the result in a `TextureResource`. It needs the Vulkan context handles (device/queue/pool) the same way `AssetManager::CreateTexture` gets them:

```cpp
// AssetTextureSink.hpp
#pragma once
#include "material/ITextureSlotSink.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class VulkanContext;
	namespace gpu { class UploadContext; }

	// Production ITextureSlotSink: resolves the .texture sibling and forwards to
	// Texture::LoadFromFile on the game thread (owns the Vulkan context).
	class AssetTextureSink final : public ITextureSlotSink
	{
	public:
		void Initialize(VulkanContext& context, gpu::UploadContext& upload, std::uint32_t capacity);

		[[nodiscard]] std::string ResolvePath(std::string_view path) const override;
		[[nodiscard]] Expected<TextureResource> Load(std::string_view resolvedPath) override;
		[[nodiscard]] std::uint32_t Capacity() const override { return m_capacity; }

	private:
		VulkanContext* m_context = nullptr;
		gpu::UploadContext* m_upload = nullptr;
		std::uint32_t m_capacity = 0;
	};
} // namespace aether
```

`ResolvePath` factors out the block at `Texture.cpp:265-280` (derive the `.texture` sibling, preserve the VFS mount). Keep the fallback: if the `.texture` sibling does not exist, return the original path (so `LoadFromFile`'s existing fallback still fires). `Load` calls `Texture::LoadFromFile(resolvedPath, device, queue, pool)` and returns `TextureResource{std::move(*tex)}` (or forwards the error). `Capacity` returns `BindlessManager::GetCapacity()` (spec §4.B); pass it in at `Initialize`.

> Factoring note: to avoid duplicating the sibling logic, consider extracting a `Texture::ResolveTexturePath(std::string_view) -> std::string` static and calling it from both `Texture::LoadFromFile` and `AssetTextureSink::ResolvePath`. This is the cleanest way to guarantee the registry hashes the *same* string `LoadFromFile` loads (spec §6 dedup-key-mismatch risk). If you prefer not to touch `Texture`, copy the block verbatim and add a `// keep in sync with Texture.cpp:265-280` comment.

- [ ] **Step 2: Own the sink + registry in `AssetSubsystem.hpp`**, declared **before** `m_materialRegistry` (its sink reference and `PackMaterial` now depend on the `TextureRegistry`), mirroring the existing `m_materialBuffer`-before-`m_materialRegistry` discipline (`AssetSubsystem.hpp:90-91`):

```cpp
#include "material/AssetTextureSink.hpp"
#include "material/TextureRegistry.hpp"
// ...
		[[nodiscard]] TextureRegistry& GetTextureRegistry() { return m_textureRegistry; }
// ...
		MaterialBuffer m_materialBuffer;
		// Declared before m_materialRegistry: PackMaterial + the material cascade
		// reference the TextureRegistry. Sink declared before the registry it backs.
		AssetTextureSink m_textureSink;
		TextureRegistry m_textureRegistry{m_textureSink};
		// Declared after m_textureRegistry: the material registry forwards it.
		MaterialRegistry m_materialRegistry{m_materialBuffer, m_textureRegistry};
```

- [ ] **Step 3: Initialize in `AssetSubsystem.cpp`.** After `m_materialBuffer.Initialize();` (line 36) and before `m_materialRegistry.InitializeDefault(...)` (line 42):

```cpp
		m_textureSink.Initialize(vk, m_uploadContext, bindless.GetCapacity());
		m_textureRegistry.InitializeDefault("engine://textures/magenta.png"); // 1x1 fallback
```

> Confirm the magenta fallback asset path during the runtime hand-off. If the engine has no magenta asset, add a 1x1 magenta under `resources/` (or synthesize one via a `LoadFromFileData` of an embedded 1x1 RGBA) — the default slot must be a real resident texture. This is the only asset dependency ④ introduces.

- [ ] **Step 4: Teardown in `AssetSubsystem::Shutdown` (`:98-121`).** Textures must free while `BindlessManager` is alive, i.e. **before** `m_materialBuffer.Shutdown()` (`:115`) and before the Vulkan context tears down. Add, after `m_materialAuthoring.ReleaseAll();` (`:110`):

```cpp
		// Release every texture entry, then flush the deferred bindless-slot ring so
		// slots free while the BindlessManager + ResourceRegistry are still live.
		m_textureRegistry = TextureRegistry{m_textureSink}; // destroys all entries -> deferred frees
		// Drain the deferred rings enough frames to retire the frees (kMaxFramesInFlight).
		// The shutdown path already waits for GPU idle before this; pump the clock so
		// FreeSampledImageSlotDeferred entries actually retire.
```

> Reassigning `m_textureRegistry` destroys all `Entry` `TextureResource`s → `Texture::Destroy` → deferred slot frees queued in the current frame's ring. The engine shutdown sequence waits for GPU idle first (AGENTS.md "Shutdown waits for GPU idle"), and `ResourceRegistry`/`BindlessManager` retire pending destructions on `AdvanceFrame`/`DrainAll`. Confirm the shutdown path drains those rings (it does for material slots today via the same mechanism); if a leak is observed at shutdown, add an explicit `bindless.DrainAll()`-equivalent flush here. Do **not** free textures after `m_materialBuffer.Shutdown()` — the bindless manager must still be live.

- [ ] **Step 5: Register the service** (so `AssetManager` and modules can reach it). Wherever `MaterialRegistry` is registered in `AetherCore.cpp`, register the texture registry too:

```cpp
		m_services.Register<TextureRegistry>(assetsSub.GetTextureRegistry());
```

(Grep `Register<MaterialRegistry>` to find the site; add the include for `material/TextureRegistry.hpp`.)

- [ ] **Step 6: Sync LSP + build Engine.**

Run: `/sync-lsp` then `cmake --build --preset vs2022-msvc --target Engine`
Expected: builds (AssetManager still populates the keep-alive vectors — fixed in Task 7; expect only those errors if any, else clean).

- [ ] **Step 7: Commit.**

```bash
git add src/engine/material/AssetTextureSink.hpp src/engine/material/AssetTextureSink.cpp src/engine/assets/AssetSubsystem.hpp src/engine/assets/AssetSubsystem.cpp src/engine/AetherCore.cpp
git commit -m "feat(material): AssetTextureSink + AssetSubsystem owns TextureRegistry (before MaterialRegistry)"
```

---

## Task 7: Migrate `AssetManager` loaders to the registry; delete both keep-alive vectors

**Files:**
- Modify: `src/engine/assets/AssetManager.hpp:59,84` (drop `outTextures`; `AssetManager` reaches the `TextureRegistry`)
- Modify: `src/engine/assets/AssetManager.cpp:314-500` (`LoadMaterialPreset`), `:549-707` (`FinaliseModelLoad`)
- Modify: `src/engine/scene/LoadedModel.hpp:27`

- [ ] **Step 1: Reach the `TextureRegistry` from `AssetManager`.** It already holds `MaterialRegistry* m_materialRegistry` (`AssetManager.hpp:105`); add `TextureRegistry* m_textureRegistry = nullptr;` and set it in `Initialize` (which already takes `MaterialRegistry&` — add a `TextureRegistry&` param, or fetch it via the already-passed `AssetSubsystem`/services). Update the `Initialize` call in `AssetSubsystem.cpp:57` to pass `m_textureRegistry`.

- [ ] **Step 2: `FinaliseModelLoad` — rewire the loader lambda** (`:584-625`). `loadTextureFromPath` currently: resolves the `.texture` sibling, calls `CreateTexture`, reads `GetBindlessSlot()`, pushes into `loaded.textures`, returns the raw slot. Replace it with a lambda returning a **`TextureHandle`** from `m_textureRegistry->Acquire(texturePath)` (the registry's sink now owns the `.texture`-sibling resolution, so pass the raw glTF path):

```cpp
		auto acquireTexture = [this](std::string_view texturePath) -> TextureHandle
		{
			if (texturePath.empty()) { return {}; }               // optional map -> invalid handle
			return m_textureRegistry->Acquire(texturePath);        // dedup + refcount; blocking (v1)
		};
```

Then, in the "new format" branch (`:666-673`), assign handles instead of slots:

```cpp
			mat.albedoTex            = acquireTexture(srcMat.albedoPath);
			mat.normalTex            = acquireTexture(srcMat.normalPath);
			mat.metallicRoughnessTex = acquireTexture(srcMat.metallicRoughnessPath);
			mat.occlusionTex         = acquireTexture(srcMat.occlusionPath);
			mat.emissiveTex          = acquireTexture(srcMat.emissivePath);
```

- [ ] **Step 3: Rewire the `imageSlots` legacy branch — DO NOT DELETE IT** (`:674-695`; spec §5 step 4, "getting this wrong breaks Fox/Human glTF loading"). This branch maps a glTF texture index → image index → a slot the *new path produced*. It currently resolves to a `uint32` slot via `imageSlots[imageIndex]`. Change the `imageSlots` parameter type from `std::vector<std::uint32_t>` to a **`std::vector<TextureHandle>`** (call it `imageHandles`) threaded from the callers (`LoadModel:513`, `LoadModelAsync:543`, and the `FinaliseModelLoad` signature `:549,84`), and have `resolveSlot` return the handle:

```cpp
			auto resolveHandle = [&](const std::int32_t texIdx) -> TextureHandle
			{
				if (texIdx < 0 || static_cast<std::size_t>(texIdx) >= source.textures.size()) { return {}; }
				const assets::GltfTexture& tex = source.textures[static_cast<std::size_t>(texIdx)];
				if (tex.imageIndex < 0 || static_cast<std::size_t>(tex.imageIndex) >= imageHandles.size()) { return {}; }
				return imageHandles[static_cast<std::size_t>(tex.imageIndex)];
			};
			mat.albedoTex            = resolveHandle(srcMat.baseColorTexture);
			mat.normalTex            = resolveHandle(srcMat.normalTexture);
			// ... etc
```

Both callers currently pass an **empty** `imageSlots` (`:513,543`), so the new-format branch is the live one and the legacy branch is dormant — but preserve it as a compiling `imageHandles` branch (the `source.textures[texIdx].imageIndex` indirection over `assets::GltfTexture` source-parse data is unrelated to the deleted `Texture` keep-alive vector). Update the `AE_VERBOSE` slot logs (`:697`) to log `.IsValid()` or `ResolveSlot` values instead of raw `.albedoSlot`.

- [ ] **Step 4: Delete the keep-alive vector.** Remove `loaded.textures.push_back(...)` (`:623`) — the lambda no longer produces a `Texture`. Delete `std::vector<Texture> textures;` from `LoadedModel.hpp:27` and drop the `#include "material/Texture.hpp"` there if now unused. Update the summary log (`:707`) that prints `loaded.textures.size()`.

- [ ] **Step 5: `LoadMaterialPreset` — drop `outTextures`** (`:314,59`). Change the signature to `Expected<MaterialAsset> LoadMaterialPreset(std::string_view path)`. The two loader lambdas:
  - `loadSlot` (binary path, `:376-390`): becomes an `acquire` that returns a `TextureHandle` from `m_textureRegistry->Acquire(texPath)`; assign to `material.albedoTex` etc. in the switch (`:395-409`).
  - `loadTextureSlot` (TOML path, `:424-457`): same — return a `TextureHandle`, assign to `material.albedoTex … emissiveTex` (`:479-488`). The sink owns `.texture`-sibling resolution, so the manual sibling blocks (`:431-445`) can be removed (or left; `Acquire` re-resolves harmlessly). Update the "loaded" info log (`:490-497`) to test `.IsValid()`.

Remove every `outTextures.push_back(...)` and the `outTextures` capture from both lambdas.

- [ ] **Step 6: `CreateTexture` stays** (`:291-293`) for direct one-off loads (e.g. UI/debug). Only material-feeding callers move to `Acquire`. `CreateTextureAsync` (`:296-307`) is untouched (async is ④b).

- [ ] **Step 7: Sync LSP + build Engine.**

Run: `/sync-lsp` then `cmake --build --preset vs2022-msvc --target Engine`
Expected: builds (modules/scripting may still reference dropped `outTextures` — fixed in Task 8).

- [ ] **Step 8: Commit.**

```bash
git add src/engine/assets/AssetManager.hpp src/engine/assets/AssetManager.cpp src/engine/assets/AssetSubsystem.cpp src/engine/scene/LoadedModel.hpp
git commit -m "refactor(material): AssetManager loaders acquire TextureHandles; delete keep-alive vectors"
```

---

## Task 8: Fix remaining callers; full build + tests

**Files:**
- Modify: any caller of `LoadMaterialPreset` / `PackMaterial` / `MaterialRegistry(...)` in `src/app/` and `src/engine/` surfaced by grep.

- [ ] **Step 1: Grep for stragglers.**

Run (Grep tool, not bash): search `src` for `\.albedoSlot|\.normalSlot|\.metallicRoughnessSlot|\.occlusionSlot|\.emissiveSlot` on `MaterialAsset` values, `outTextures`, `LoadedModel::textures`, `loaded.textures`, and `PackMaterial(` with one argument.
Expected findings and fixes:
  - Any `MaterialAsset` construction that set `.albedoSlot = <uint>` → set `.albedoTex = registry.Acquire(path)` or leave default (invalid) for "no texture".
  - `LoadMaterialPreset(path, outVec)` callers → `LoadMaterialPreset(path)`.
  - Direct `PackMaterial(asset)` callers (outside `MaterialRegistry`) → `PackMaterial(asset, textureRegistry)`.

> Scripting modules (`WorldModule.cpp`, `EffectsModule.cpp`) went through `MaterialSystem::AssignMaterial` / `MaterialInstanceComponent` in phase ①; they operate on factor fields, not texture slots, so they should need **no** change beyond a recompile. Confirm via grep — if any module set a raw `*Slot` uint, route it through `Acquire`.

- [ ] **Step 2: Full build (Engine + App + AssetPacker + shaders).**

Run: `cmake --build --preset vs2022-msvc`
Then (shaders must still compile, ABI unchanged): `cmake --build --preset vs2022-msvc --target App_CompileShaders`
Expected: clean. `GpuMaterial` static_asserts and both shader files are untouched.

- [ ] **Step 3: Run unit tests.**

Run: `ctest --test-dir out/build/vs2022-msvc -C RelWithDebInfo --output-on-failure`
Expected: PASS — all material + texture cases.

- [ ] **Step 4: Commit.**

```bash
git add -A
git commit -m "refactor(material): route remaining callers through TextureHandle; full build green"
```

---

## Task 9: ABI-freeze verification + final cleanup

**Files:** none (verification only, plus any straggler fix).

- [ ] **Step 1: Assert the ABI is frozen.** Confirm `git diff` touched **none** of: `src/engine/material/GpuMaterial.hpp`, `shaders/gltf_mesh.slang`, `shaders/include/GpuMaterial.slangh`. `PackMaterial` is still the only function writing `GpuMaterial::*Slot`. The 80-byte `static_assert`s (`GpuMaterial.hpp:48-61`) compiled — that is the ABI proof.

- [ ] **Step 2: Grep for dead references.**

Run (Grep): `LoadedModel::textures`, `outTextures`, single-arg `PackMaterial(` — expect zero.

- [ ] **Step 3: Clean rebuild + tests once more.**

Run: `cmake --build --preset vs2022-msvc && ctest --test-dir out/build/vs2022-msvc -C RelWithDebInfo --output-on-failure`
Expected: clean; tests pass.

- [ ] **Step 4: Commit.**

```bash
git add -A
git commit -m "chore(material): texture-layer ABI-freeze verification + cleanup"
```

---

## Threading & frames-in-flight notes (fold into review, not a task)

- **Game thread owns Vulkan.** All of ④ runs on the game thread: `TextureRegistry::Acquire` (v1) calls `sink.Load` → `Texture::LoadFromFile` (decode + upload) synchronously. No coroutine resumption is introduced by ④; `CreateTextureAsync`/`ReadFileAsync` and the `queued_executor` game-thread-resume path (`Executor.hpp:101-113`) are the ④b concern only. The `TextureRegistry` still locks `m_mutex` so a future async completion (④b) resuming on the game thread and swapping an entry is race-free against `ResolveSlot` at pack time.
- **Frames-in-flight safety for slot frees.** Freeing a texture entry destroys its `Texture` → `ResourceRegistry::Destroy` → `FreeSampledImageSlotDeferred` (`ResourceRegistry.cpp:1006-1008`), retired `deferredFreeFrames` later on the `BindlessManager::AdvanceFrame` clock already pumped at `AetherCore.cpp:570,685`. A material re-assign that drops a texture to refcount 0 therefore does not free a descriptor an in-flight frame is still sampling — the same guarantee material slots already rely on.
- **Teardown order.** Registry destruction must precede `MaterialBuffer::Shutdown()` and the Vulkan context teardown so the bindless manager is alive to retire the deferred frees (Task 6 Step 4). Shutdown waits for GPU idle first (AGENTS.md).

---

## Runtime hand-off checklist (user runs the GPU app — no display in agent context)

- [ ] **GLTF models render textured, identical to today.** Fox and Human load with correct albedo/normal/ORM. The `imageHandles` legacy-branch rewire (Task 7 Step 3) did not regress; the new-format branch is the live path.
- [ ] **Material presets** (TOML + binary `.material`) still resolve their five texture slots (albedo/normal/metallicRoughness/occlusion/emissive) — `LoadMaterialPreset` returns the same visual result, now via handles.
- [ ] **Dedup works.** Loading the same textured model/material on many entities does **not** multiply bindless sampled-image slots — watch the sampled-image slot count (or the bindless capacity gauge) stay flat as identical entities spawn. Two path spellings of one asset (`.png` vs `.texture`) share one slot.
- [ ] **No slot leak over time.** Destroying/reloading textured scenes repeatedly shows no monotonic growth in used sampled-image slots. A `LoadedModel` discarded immediately after `SpawnModel` keeps its textures resident because the entities' `MaterialComponent`s hold the material refs, which hold the texture refs (the cascade); `on_destroy<MaterialComponent>` releases both on entity teardown.
- [ ] **Stale vs optional distinction is visible.** An entity whose texture was force-freed shows **magenta** (stale → default slot), not untextured; a material with a genuinely absent map (default-constructed handle → `kNoTexture`) shows the base-color factor with the sample skipped, exactly as before.
- [ ] **Magenta default asset exists** at the path wired in Task 6 Step 3; if missing, add a 1x1 magenta texture.
- [ ] **No validation errors** on shutdown (all deferred bindless-slot frees retired; no live-slot destruction).

---

## Self-Review

- **Spec coverage:** §4.A (`TextureHandle`) → Task 1. §4.B (`TextureRegistry`) → Tasks 2-3. §4.C (`ITextureSlotSink` + resolve-before-hash) → Tasks 1-2 (+ `FakeTextureSink`, the two-spellings test). §4.D (`PackMaterial` resolves → heap index, ABI frozen, deferred frees) → Tasks 4, 9. §4.E/§4.F (streaming rejected/deferred) → explicitly out of scope; the threading note records why the ownership model shipped is *not* the broken same-slot swap. §5 migration (MaterialAsset, PackMaterial, FinaliseModelLoad incl. the imageSlots-branch rewire, LoadMaterialPreset, AssetSubsystem ordering, cascade lifetime rule) → Tasks 4-7. §6 risks (dedup-key mismatch, refcount leak, teardown order, default-vs-kNoTexture) → covered by tests + Task 6 Step 4 + the `slotFor` split in Task 4. §7 acceptance (tests + hand-off) → Tasks 2-3, 5 + the runtime checklist.
- **Refinements documented (grounded in current code, not the spec's stale line numbers):** (1) sink returns a move-only `TextureResource`, not `Expected<Texture>`, so the fake sink is GPU-free; (2) the cascade seam is `MaterialRegistry::Acquire`/`Release` with a new `TextureRegistry::AddRef` primitive (no path round-trip) + the entt hook as backstop; (3) `LoadedModel` already spawns per-entity registry handles, so only `LoadedModel::textures` remains to delete; (4) `LoadMaterialPreset` already returns `Expected<MaterialAsset>`; (5) `BindlessManager::AdvanceFrame` is already pumped — no new per-frame pump.
- **Type consistency:** `TextureHandle{index,generation}`, `ITextureSlotSink{ResolvePath,Load,Capacity}`, `TextureResource{GetBindlessSlot}`, `TextureRegistry{Acquire,Release,ResolveSlot,InitializeDefault,DefaultHandle,AddRef}`, `MaterialAsset{albedoTex…emissiveTex}`, `PackMaterial(asset, textures)`, `MaterialRegistry(sink, textures)` used consistently across tasks.
- **ABI freeze is a first-class checkpoint (Task 9):** `GpuMaterial.hpp`, `gltf_mesh.slang`, `GpuMaterial.slangh` are asserted untouched; `PackMaterial` stays the single writer of raw heap-index `uint32`s.
- **Open items flagged in-plan (need runtime confirmation):** the magenta default asset path (Task 6 Step 3) and the shutdown deferred-ring flush (Task 6 Step 4) — both are hand-off checklist items because they can only be validated on the GPU app.
