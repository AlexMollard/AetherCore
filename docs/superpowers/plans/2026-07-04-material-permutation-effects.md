# Material Permutation & Effects (③) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land sub-project ③ of the material overhaul on top of the merged ① core: (a) a content-addressed `PipelineCache` that dedups `GraphicsPipeline` objects and collapses the two ad-hoc pipeline factories, (b) a correct **per-entity** effect-parameter path in its own GPU buffer that replaces today's re-`Acquire`-per-slider material churn and the PBR-field smuggling, and (c) real pipeline consumers for the authored `doubleSided`/`alphaBlend` intent. Engine + app side, **build-time `.spv` selection only** — no runtime recompilation, no `#define` permutations.

**Architecture:** Three orthogonal concepts split cleanly (spec §3):
1. `MaterialTemplate` — the small POD that *forces a distinct pipeline* (program + blend + cull + depth-write). Content-addressed and deduped by `PipelineCache` (the render-state analogue of ①'s `MaterialRegistry`).
2. `MaterialFeatureFlags` — runtime per-fragment branches already carried in `GpuMaterial.flags` (`kModulateVertexColor`, `kAlphaMask`). **③ adds nothing here** — the ABI stays frozen.
3. `EffectParams` — per-entity *mutable* animated inputs (tint/speed/scale/intensity) in a separate 32-byte record and its own BDA SSBO, indexed by a per-entity `paramSlot` **independent** of the material's shared `gpuSlot`. Explicitly **not** deduped.

**Tech Stack:** C++23/26, CMake + CPM, doctest (`tests/` already exists from ①), entt ECS, Vulkan 1.4 (VK_EXT_descriptor_heap + BDA), Slang shaders. Tabs; `kPascalCase` constants; first include is the matching header; no `Vk*` outside `gpu/`/`vulkan/`.

**Spec:** `docs/superpowers/specs/2026-07-04-material-permutation-effects-design.md`

**Build/test commands (from AGENTS.md + ①):**
- Engine: `cmake --build build-vs2022-msvc --config Debug --target Engine`
- App: `cmake --build build-vs2022-msvc --config Debug --target App`
- Tests target: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
- Run tests: `./build-vs2022-msvc/tests/Debug/EngineTests.exe` (or `ctest --test-dir build-vs2022-msvc -C Debug --output-on-failure`)
- Shaders: `cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders`
- After adding/removing/renaming `.cpp`/`.hpp` or editing any `CMakeLists.txt`, run `/sync-lsp`.

---

## Reconciliation vs. spec (verified against current code, 2026-07-04)

The spec cites some line numbers from an earlier tree state. The plan is grounded in the **current** code; the material substance of every spec claim still holds. Deltas resolved:

- **`GpuMaterial` ABI is already frozen and correct.** `kModulateVertexColor = 1u << 3` already exists (`GpuMaterial.hpp:31`), the struct is 80 bytes, and all **13 offset asserts + 1 size assert = 14** are present and untouched (`GpuMaterial.hpp:48-61`). ③ does **not** touch this file. Slang side `GpuMaterial.slangh` mirrors it (its own `_pad0`/`_pad1` at 72/76 are unrelated to `InstanceData._pad0`).
- **`FrameConstants._padEnd` is at offset 760** exactly as the spec says (`FrameConstants.hpp:84`, asserted `:135`; size 768 asserted `:110,136`). Slang mirror at `FrameConstants.slangh:34`.
- **`InstanceData._pad0` is at offset 76** (`GpuContracts.hpp:36`, asserted `:46`); size 104, 8 asserts (`:41-48`). Slang mirror `DrawInstanceData._pad0` at `[[vk::offset(76)]]` (`RenderContracts.slangh:20`).
- **Effects already route through `MaterialSystem` + `MaterialInstanceComponent`.** The spec's "re-`Acquire` per slider" is real but now lives in `MaterialSystem::SetEmissive/SetMetallic/SetRoughness/SetOcclusion` (`MaterialSystem.cpp:72-125`), invoked from `EffectsModule.cpp:47-68`. `set_entity_effect` (`EffectsModule.cpp:19-39`) sets `PipelineComponent{&effect->pipeline}`, re-seeds `MaterialInstanceComponent`, and calls `AssignMaterial`. This is the churny path ③ replaces.
- **`EffectManager` is in `src/app/effects/`** (not engine), already stores `MaterialAsset` (not the deleted `Material`), and its `EffectData{GraphicsPipeline pipeline; MaterialAsset material;}` owns the pipeline. ③ shrinks it to an `EffectDef` registry over `PipelineCache`.
- **`AssignMaterial` current signature** is `AssignMaterial(World&, Entity, MaterialRegistry&, const MaterialAsset&)` and already does acquire-before-release (`MaterialSystem.cpp:31-49`). ③ extends it to also resolve a pipeline.
- **Lifecycle disconnect exists**: `MaterialSystem::DisconnectLifecycle` (`MaterialSystem.cpp:25-29`), called from `AssetSubsystem::Shutdown` (`AssetSubsystem.cpp:105`). The effect lifecycle mirrors this.
- **Per-frame ticks** live in `AssetSubsystem::AdvanceFrame` (`AssetSubsystem.cpp:68-71`), called twice from `AetherCore.cpp:571,686`. `EffectParamBuffer::AdvanceFrame` hangs here.
- **Pipeline-teardown drain (spec §5 open item) confirmed:** `GraphicsPipeline::Destroy()` only *schedules* deferred destruction (`GraphicsPipeline.hpp:56-63`); shutdown safety comes from the engine's GPU-idle-first teardown (AGENTS.md "Engine lifecycle": *"Shutdown waits for GPU idle first"*) and the hot-reload path's `RunExclusive(QuiesceMode::Discard)` + GPU `WaitIdle` (`ScriptedSceneLayer.cpp:105-120`). The `PipelineCache` is owned by `AssetSubsystem` and shut down inside that same GPU-idle window (see Task 8). Today `EffectManager::DestroyAll` runs the identical `Destroy()` path from `ScriptedSceneLayer::OnDetach` (`ScriptedSceneLayer.cpp:226`, `EffectManager.cpp:46-54`) without a leak, which is the real precedent this plan cites rather than assumes.

---

## File Structure

**New files (engine — auto-globbed under `src/engine/`, no CMake edit):**
- `src/engine/material/MaterialTemplate.hpp` — pipeline key POD + hash/equality.
- `src/engine/material/EffectParams.hpp` — 32-byte per-entity effect record (+ asserts).
- `src/engine/material/EffectParamBuffer.hpp` / `.cpp` — BDA SSBO, second instance of the `MaterialBuffer` pattern.
- `src/engine/material/PipelineCache.hpp` / `.cpp` — content-addressed `GraphicsPipeline` dedup.
- `src/engine/material/EffectSystem.hpp` / `.cpp` — `on_destroy<EffectParamsComponent>` lifecycle + param helpers.

**New files (tests):**
- `tests/material/FakePipelineFactory.hpp` — in-memory pipeline factory for `PipelineCache` tests.
- `tests/material/PipelineCacheTests.cpp`, `tests/material/EffectParamBufferTests.cpp`.

**Modified (engine):**
- `src/engine/rendering/GpuContracts.hpp` — rename `InstanceData._pad0` → `effectParamIndex` (offset 76, size unchanged).
- `src/engine/rendering/FrameConstants.hpp` — rename `_padEnd` → `effectParamBufferAddr` (offset 760, size unchanged).
- `src/engine/rendering/RenderQueue.hpp` / `.cpp` — `DrawCommand.effectParamIndex`; copy into `InstanceData`.
- `src/engine/rendering/WorldRenderer.cpp` — read `EffectParamsComponent`, set `DrawCommand.effectParamIndex`.
- `src/engine/rendering/RenderFramePacket.hpp`, `src/engine/gpu/GpuDevice.cpp`, `src/engine/AetherCore.cpp` — plumb `effectParamBufferAddr` into `FrameConstants`.
- `src/engine/scene/Components.hpp` — add `EffectParamsComponent`.
- `src/engine/material/MaterialAsset.hpp` — add `MaterialTemplate templateDesc`.
- `src/engine/material/MaterialSystem.hpp` / `.cpp` — `AssignMaterial` resolves a pipeline through `PipelineCache` + effect-override rule.
- `src/engine/assets/AssetSubsystem.hpp` / `.cpp` — own `PipelineCache` + `EffectParamBuffer`; init `Context`; tick `AdvanceFrame`; connect/disconnect effect lifecycle.
- `src/engine/assets/AssetManager.hpp` / `.cpp` — expose the single pipeline factory to `PipelineCache` (or hand the cache the factory functor).

**Modified (app):**
- `src/app/effects/EffectManager.hpp` / `.cpp` — collapse to an `EffectDef{MaterialTemplate; EffectParams;}` registry over `PipelineCache`; drop `GraphicsPipeline` ownership + `CreateAndRegister`/`DestroyAll`.
- `src/app/layers/ScriptedSceneLayer.cpp` — delete `BuildDefaultPipeline`/`m_defaultPipeline`; register the plasma `EffectDef`; feed `PipelineCache::Context`.
- `src/app/layers/ScriptedSceneLayer.hpp` — drop `m_defaultPipeline`.
- `src/app/scripting/SceneContext.hpp` — `defaultPipeline`/`CachedMesh.pipeline` no longer carry a raw pipeline (resolved via cache).
- `src/app/scripting/modules/WorldModule.cpp` — `add_mesh` always resolves a pipeline via `AssignMaterial`; `create_mesh` stops caching a raw pipeline pointer.
- `src/app/scripting/modules/EffectsModule.cpp` — `set_entity_effect` allocates a `paramSlot`; `set_effect_*` become one `EffectParamBuffer::Write`.

**Shaders:**
- `shaders/include/RenderContracts.slangh` — `_pad0` → `effectParamIndex` (offset 76).
- `shaders/include/MeshVertex.slangh` — add `nointerpolation uint effectParamIndex` varying to `VSOutput`.
- `shaders/include/DefaultVertex.slangh` — forward `instance.effectParamIndex` into `VSOutput`.
- `shaders/include/FrameConstants.slangh` — `_padEnd` → `effectParamBufferAddr` (offset 760).
- `shaders/include/EffectParams.slangh` (new) — mirror of `EffectParams`.
- `shaders/plasma.slang` — read `EffectParams` by name instead of PBR-field overloads.

**~10-12 files touched across engine, app, and shaders — one coherent change, compile-checked at every task.** The app is GPU-driven and cannot run in-agent; implementation ends at a clean compile + the Task 12 runtime hand-off checklist.

---

## Task 1: ABI carrier renames — `InstanceData.effectParamIndex` + `FrameConstants.effectParamBufferAddr` (size-frozen)

Repurpose the two reserved paddings the spec identified. **No struct changes size**; every existing assert holds (the renamed field simply takes over the old padding's offset).

**Files:**
- Modify: `src/engine/rendering/GpuContracts.hpp:36,46`
- Modify: `src/engine/rendering/FrameConstants.hpp:49,84,135` (comment + field + assert)
- Modify: `shaders/include/RenderContracts.slangh:20`
- Modify: `shaders/include/FrameConstants.slangh:34`

- [ ] **Step 1: Rename the host `InstanceData` padding.** In `GpuContracts.hpp`, change the field at offset 76 (`GpuContracts.hpp:36`):

```cpp
			std::uint32_t effectParamIndex = 0xFFFFFFFFu; // per-entity EffectParams slot; 0xFFFF... = no effect
```

Update the matching assert (`GpuContracts.hpp:46`):

```cpp
			static_assert(offsetof(InstanceData, effectParamIndex) == 76);
```

Leave the `sizeof(InstanceData) == 104` assert and the other 7 offset asserts exactly as-is.

- [ ] **Step 2: Rename the shader `DrawInstanceData` padding.** In `RenderContracts.slangh:20`:

```hlsl
    [[vk::offset(76)]] uint     effectParamIndex;
```

- [ ] **Step 3: Rename the host `FrameConstants` tail padding.** In `FrameConstants.hpp`, change the field at offset 760 (`:84`) from `std::uint64_t _padEnd = 0;` to:

```cpp
		std::uint64_t effectParamBufferAddr = 0; // offset 760 : BDA of EffectParamBuffer (0 = no effects active)
```

Update the comment block line (`:49`) from `_padEnd ... End padding` to `effectParamBufferAddr ... BDA of EffectParamBuffer`. Update the assert (`:135`):

```cpp
	static_assert(offsetof(FrameConstants, effectParamBufferAddr) == 760);
```

Leave both `sizeof(FrameConstants) == 768` asserts (`:110,136`) and `RefreshDerived` untouched — it only touches `invViewProj`/`frustumPlanes`, never the tail (verified `FrameConstants.hpp:86-101`).

- [ ] **Step 4: Rename the shader `FrameConstantsData` padding.** In `FrameConstants.slangh:34`:

```hlsl
    [[vk::offset(760)]] uint64_t effectParamBufferAddr; // BDA -> EffectParams[] (0 = no effects)
```

- [ ] **Step 5: Compile-check the engine + shaders.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Run: `cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders`
Expected: builds; the 80/104/768-byte `static_assert`s all still compile (nothing consumes the renamed fields yet — this is a pure, ABI-identical rename).

- [ ] **Step 6: Commit.**

```bash
git add src/engine/rendering/GpuContracts.hpp src/engine/rendering/FrameConstants.hpp shaders/include/RenderContracts.slangh shaders/include/FrameConstants.slangh
git commit -m "refactor(material): repurpose reserved padding as effectParamIndex/effectParamBufferAddr (ABI-identical)"
```

---

## Task 2: `MaterialTemplate` — the pipeline key POD (+ hash/equality)

**Files:**
- Create: `src/engine/material/MaterialTemplate.hpp`

- [ ] **Step 1: Create `src/engine/material/MaterialTemplate.hpp`.** A subset of `GraphicsPipeline::Desc` restricted to the pipeline-forcing axes (spec §4.A). Uses the real `gpu::CullMode` enum (`GraphicsPipeline.hpp:34`). String views must reference storage that outlives the cache (VFS path literals, as today):

```cpp
#pragma once

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// The axes that force a distinct GraphicsPipeline object (spec §3.1): shader
	// program + raster/blend/depth-write state. A subset of GraphicsPipeline::Desc;
	// the format/heap-mapping axes are frame-graph-constant and live in
	// PipelineCache::Context, not here. Content-addressed and deduped by
	// PipelineCache -- the render-state analogue of the 80-byte GpuMaterial record.
	//
	// shaderVfsPath / fragmentVfsPath must reference storage that outlives the cache
	// (today: static VFS path literals like "shaders://gltf_mesh.spv").
	struct MaterialTemplate
	{
		std::string_view shaderVfsPath;                // e.g. "shaders://gltf_mesh.spv"
		std::string_view fragmentVfsPath;              // usually empty (shared vertex+fragment module)
		gpu::CullMode cullMode = gpu::CullMode::None;  // authored doubleSided maps here
		bool blendEnable = false;                      // authored alphaBlend maps here
		bool depthWriteEnable = true;

		friend bool operator==(const MaterialTemplate& a, const MaterialTemplate& b)
		{
			return a.shaderVfsPath == b.shaderVfsPath
			    && a.fragmentVfsPath == b.fragmentVfsPath
			    && a.cullMode == b.cullMode
			    && a.blendEnable == b.blendEnable
			    && a.depthWriteEnable == b.depthWriteEnable;
		}
	};

	// FNV-1a over the template's semantic fields, mirroring MaterialRegistry's
	// hashing style. Collisions are resolved by operator== on a hit, so hash
	// quality affects only dedup speed, not correctness.
	[[nodiscard]] inline std::uint64_t HashMaterialTemplate(const MaterialTemplate& t)
	{
		auto mix = [](std::uint64_t h, const void* data, std::size_t n)
		{
			const auto* p = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
			return h;
		};
		std::uint64_t h = 1469598103934665603ull;
		h = mix(h, t.shaderVfsPath.data(), t.shaderVfsPath.size());
		h = mix(h, t.fragmentVfsPath.data(), t.fragmentVfsPath.size());
		const std::uint32_t cull = static_cast<std::uint32_t>(t.cullMode);
		h = mix(h, &cull, sizeof(cull));
		h = mix(h, &t.blendEnable, sizeof(t.blendEnable));
		h = mix(h, &t.depthWriteEnable, sizeof(t.depthWriteEnable));
		return h;
	}
} // namespace aether
```

> Note: hashing `string_view` **contents** (not the pointer) means two identical path literals dedup even if they are distinct `string_view` instances. `operator==` on `string_view` also compares contents, so the on-hit guard is correct.

- [ ] **Step 2: Compile-check.** (Header-only; compiled transitively later — a quick smoke is to build the tests target after Task 3. No standalone build here.)

- [ ] **Step 3: Commit.**

```bash
git add src/engine/material/MaterialTemplate.hpp
git commit -m "feat(material): add MaterialTemplate pipeline key POD + FNV-1a hash"
```

---

## Task 3: `PipelineCache` — content-addressed pipeline dedup (TDD)

Node-stable (`std::unordered_map<Key, GraphicsPipeline>` for pointer stability — `GraphicsPipeline` is move-only, `GraphicsPipeline.hpp:45-49`), **not** ref-counted (spec §4.C). To keep it unit-testable without a GPU, the cache builds pipelines through an injected factory functor (production: `AssetManager::CreateGraphicsPipeline`; tests: a fake).

**Files:**
- Create: `src/engine/material/PipelineCache.hpp`, `src/engine/material/PipelineCache.cpp`
- Create: `tests/material/FakePipelineFactory.hpp`, `tests/material/PipelineCacheTests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `src/engine/material/PipelineCache.hpp`.** The factory is `std::function<Expected<GraphicsPipeline>(const GraphicsPipeline::Desc&)>` so tests inject a fake and production wires `AssetManager::CreateGraphicsPipeline`:

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "gpu/GpuTypes.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/MaterialTemplate.hpp"

namespace aether
{
	// Content-addressed dedup of GraphicsPipeline objects keyed by MaterialTemplate
	// (spec §4.C). The render-state analogue of ①'s MaterialRegistry. Node-stable
	// (unordered_map => stable GraphicsPipeline addresses for PipelineComponent's raw
	// pointers) and NOT ref-counted: the distinct-template working set is tiny and
	// app-lifetime, so we trade a few permanently-resident VkShaderEXT sets for zero
	// lifetime bookkeeping and stable pointers.
	//
	// Thread-safety: Acquire is mutex-guarded. Shutdown must run inside the engine's
	// GPU-idle window (see AssetSubsystem::Shutdown) because GraphicsPipeline::Destroy
	// only schedules deferred destruction.
	class PipelineCache
	{
	public:
		// Frame-graph-constant axes NOT part of the hash key: identical for every
		// scene pipeline in a given frame graph. Supplied once at init.
		// descriptorHeapMappings is a raw pointer owned by BindlessManager that MUST
		// outlive the cache -- shut the cache down before the BindlessManager it borrows.
		struct Context
		{
			gpu::Format colorFormat = gpu::Format::Undefined;
			gpu::Format depthFormat = gpu::Format::Undefined;
			const void* descriptorHeapMappings = nullptr;
		};

		using Factory = std::function<Expected<GraphicsPipeline>(const GraphicsPipeline::Desc&)>;

		void Initialize(Context context, Factory factory);

		// Hash the template, look up; on a hash hit compare template fields before
		// treating it as a match; on miss build via the factory combining tmpl with
		// the Context, store, return a stable pointer. Returns nullptr on build failure.
		[[nodiscard]] const GraphicsPipeline* Acquire(const MaterialTemplate& tmpl);

		// Destroy() every cached pipeline (schedules deferred destruction) and clear.
		// Must run inside the GPU-idle shutdown window.
		void Shutdown();

		[[nodiscard]] std::size_t Size() const;

	private:
		struct Entry
		{
			MaterialTemplate tmpl{};
			GraphicsPipeline pipeline;
		};

		mutable std::mutex m_mutex;
		Context m_context{};
		Factory m_factory;
		std::unordered_multimap<std::uint64_t, Entry> m_entries; // node-stable => &pipeline is durable
	};
} // namespace aether
```

- [ ] **Step 2: Create `src/engine/material/PipelineCache.cpp`.**

```cpp
#include "material/PipelineCache.hpp"

#include "utils/Logger.hpp"

namespace aether
{
	void PipelineCache::Initialize(Context context, Factory factory)
	{
		std::scoped_lock lock(m_mutex);
		m_context = context;
		m_factory = std::move(factory);
	}

	const GraphicsPipeline* PipelineCache::Acquire(const MaterialTemplate& tmpl)
	{
		const std::uint64_t hash = HashMaterialTemplate(tmpl);

		std::scoped_lock lock(m_mutex);
		auto range = m_entries.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second.tmpl == tmpl)
			{
				return &it->second.pipeline; // stable node address
			}
		}

		GraphicsPipeline::Desc desc{};
		desc.shaderVfsPath = tmpl.shaderVfsPath;
		desc.fragmentVfsPath = tmpl.fragmentVfsPath;
		desc.colorFormat = m_context.colorFormat;
		desc.depthFormat = m_context.depthFormat;
		desc.depthTestEnable = true;
		desc.depthWriteEnable = tmpl.depthWriteEnable;
		desc.depthCompareOp = gpu::CompareOp::LessOrEqual;
		desc.blendEnable = tmpl.blendEnable;
		desc.cullMode = tmpl.cullMode;
		desc.descriptorHeapMappings = m_context.descriptorHeapMappings;

		Expected<GraphicsPipeline> built = m_factory(desc);
		if (!built)
		{
			AE_ERROR(LogCategory::Render, "PipelineCache: pipeline build failed for '{}'", tmpl.shaderVfsPath);
			return nullptr;
		}

		auto inserted = m_entries.emplace(hash, Entry{tmpl, std::move(built.value())});
		return &inserted->second.pipeline;
	}

	void PipelineCache::Shutdown()
	{
		std::scoped_lock lock(m_mutex);
		for (auto& [hash, entry]: m_entries)
		{
			entry.pipeline.Destroy();
		}
		m_entries.clear();
		m_factory = nullptr;
	}

	std::size_t PipelineCache::Size() const
	{
		std::scoped_lock lock(m_mutex);
		return m_entries.size();
	}
} // namespace aether
```

> Note the depth defaults reproduce today's `BuildDefaultPipeline` (`ScriptedSceneLayer.cpp:52-60`) and `EffectManager::CreateAndRegister` (`EffectManager.cpp:21-29`) settings — `depthTestEnable=true`, `LessOrEqual`. `depthWriteEnable` becomes template-driven (default `true` matches both existing factories).

- [ ] **Step 3: Create the fake factory `tests/material/FakePipelineFactory.hpp`.** `GraphicsPipeline` is move-only and holds an invalid handle by default; a default-constructed one is a stand-in "distinct object". The fake returns a fresh `GraphicsPipeline{}` per build and counts builds so tests assert dedup:

```cpp
#pragma once
#include "rendering/GraphicsPipeline.hpp"

// In-memory pipeline factory for PipelineCache tests. No GPU: returns a fresh
// default-constructed (invalid-handle) GraphicsPipeline per build and counts
// builds. Cache identity is tested via the returned pointer, not GPU validity.
struct FakePipelineFactory
{
	int buildCount = 0;

	aether::Expected<aether::GraphicsPipeline> operator()(const aether::GraphicsPipeline::Desc&)
	{
		++buildCount;
		return aether::GraphicsPipeline{}; // move-only; distinct object per build
	}
};
```

> If `Expected<GraphicsPipeline>` construction from a prvalue is awkward, return via the engine's success helper used elsewhere (check how `GraphicsPipeline::Create` returns success in `GraphicsPipeline.cpp`) and mirror it. `Shutdown()` calling `Destroy()` on an invalid-handle pipeline is a safe no-op (`GraphicsPipeline::Destroy` guards on the handle).

- [ ] **Step 4: Write failing tests `tests/material/PipelineCacheTests.cpp`.**

```cpp
#include <doctest/doctest.h>
#include "material/MaterialTemplate.hpp"
#include "material/PipelineCache.hpp"
#include "FakePipelineFactory.hpp"

using namespace aether;

static MaterialTemplate GltfTemplate()
{
	MaterialTemplate t;
	t.shaderVfsPath = "shaders://gltf_mesh.spv";
	return t;
}

TEST_CASE("PipelineCache dedups identical templates to one pipeline pointer") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	const GraphicsPipeline* a = cache.Acquire(GltfTemplate());
	const GraphicsPipeline* b = cache.Acquire(GltfTemplate());

	CHECK(a != nullptr);
	CHECK(a == b);            // deduped
	CHECK(factory.buildCount == 1);
	CHECK(cache.Size() == 1);
}

TEST_CASE("PipelineCache builds distinct pipelines for differing blend/cull/program") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	MaterialTemplate opaque = GltfTemplate();
	MaterialTemplate blended = GltfTemplate(); blended.blendEnable = true;
	MaterialTemplate culled = GltfTemplate(); culled.cullMode = gpu::CullMode::Back;
	MaterialTemplate plasma; plasma.shaderVfsPath = "shaders://plasma.spv";

	const GraphicsPipeline* p0 = cache.Acquire(opaque);
	const GraphicsPipeline* p1 = cache.Acquire(blended);
	const GraphicsPipeline* p2 = cache.Acquire(culled);
	const GraphicsPipeline* p3 = cache.Acquire(plasma);

	CHECK(p0 != p1);
	CHECK(p0 != p2);
	CHECK(p0 != p3);
	CHECK(factory.buildCount == 4);
	CHECK(cache.Size() == 4);
}

TEST_CASE("PipelineCache pointers stay stable as more templates are added") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	const GraphicsPipeline* first = cache.Acquire(GltfTemplate());
	for (int i = 0; i < 32; ++i)
	{
		MaterialTemplate t; t.shaderVfsPath = "shaders://plasma.spv"; t.depthWriteEnable = (i % 2 == 0);
		// force many distinct keys via fragment path variety
		t.fragmentVfsPath = (i % 3 == 0) ? "a" : (i % 3 == 1) ? "b" : "c";
		cache.Acquire(t);
	}
	// The original node's address must not have moved (node-stable container).
	CHECK(cache.Acquire(GltfTemplate()) == first);
}
```

> The stability test deliberately inserts many entries; because `unordered_map` nodes never move, `first` stays valid. This is the guard against the "reallocating vector dangles every `PipelineComponent`" risk (spec §4.C, R-dangling-pointer).

- [ ] **Step 5: Register the new test files** in `tests/CMakeLists.txt` — add to the `add_executable(EngineTests ...)` list:

```cmake
    material/PipelineCacheTests.cpp
```

(Run `/sync-lsp` after editing `CMakeLists.txt`.)

- [ ] **Step 6: Run — verify pass.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
Run: `./build-vs2022-msvc/tests/Debug/EngineTests.exe`
Expected: PASS (3 new cases + all ① cases).

- [ ] **Step 7: Commit.**

```bash
git add src/engine/material/PipelineCache.hpp src/engine/material/PipelineCache.cpp tests/material/FakePipelineFactory.hpp tests/material/PipelineCacheTests.cpp tests/CMakeLists.txt
git commit -m "feat(material): PipelineCache content-addressed dedup over injected factory (TDD)"
```

---

## Task 4: `EffectParams` record + `shaders/include/EffectParams.slangh`

**Files:**
- Create: `src/engine/material/EffectParams.hpp`
- Create: `shaders/include/EffectParams.slangh`

- [ ] **Step 1: Create `src/engine/material/EffectParams.hpp`** (32 bytes, honest fields; offset asserts mirroring `GpuMaterial.hpp:48-61` style, spec §4.E):

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace aether
{
	// Per-entity animated effect inputs. Mutable per-entity state (NOT content-
	// addressed, unlike GpuMaterial) stored in EffectParamBuffer, one slot per
	// effect entity. Replaces the PBR-field smuggling in the old plasma path
	// (tint<-emissive, speed<-metallic, scale<-roughness, intensity<-occlusion).
	// Must stay binary-compatible with EffectParams in shaders/include/EffectParams.slangh.
	struct EffectParams
	{
		glm::vec4 tint{1.0f};   // offset 0
		float speed = 1.0f;     // offset 16
		float scale = 1.0f;     // offset 20
		float intensity = 1.0f; // offset 24
		std::uint32_t _pad = 0; // offset 28
	};

	static_assert(sizeof(EffectParams) == 32, "EffectParams size changed - update shaders/include/EffectParams.slangh.");
	static_assert(offsetof(EffectParams, tint) == 0);
	static_assert(offsetof(EffectParams, speed) == 16);
	static_assert(offsetof(EffectParams, scale) == 20);
	static_assert(offsetof(EffectParams, intensity) == 24);
	static_assert(offsetof(EffectParams, _pad) == 28);
} // namespace aether
```

- [ ] **Step 2: Create `shaders/include/EffectParams.slangh`.**

```hlsl
#ifndef AETHER_EFFECT_PARAMS_SLANGH
#define AETHER_EFFECT_PARAMS_SLANGH

// Must stay binary-compatible with aether::EffectParams (32 bytes).

struct EffectParams
{
    [[vk::offset(0)]]  float4 tint;
    [[vk::offset(16)]] float  speed;
    [[vk::offset(20)]] float  scale;
    [[vk::offset(24)]] float  intensity;
    [[vk::offset(28)]] uint   _pad;
};

#endif
```

- [ ] **Step 3: Compile-check.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds (asserts compile). The `.slangh` is header-only until plasma includes it (Task 10).

- [ ] **Step 4: Commit.**

```bash
git add src/engine/material/EffectParams.hpp shaders/include/EffectParams.slangh
git commit -m "feat(material): add 32-byte EffectParams record + slang mirror"
```

---

## Task 5: `EffectParamBuffer` — second instance of the MaterialBuffer pattern (TDD)

A persistently-mapped BDA SSBO with its own `AllocateSlot/FreeSlot/Write/AdvanceFrame` over its own `DeferredSlotFreeList` (`kReuseDelayFrames = kMaxFramesInFlight + 1`, `DeferredSlotFreeList.hpp:25`). **No dedup** — a unique slot per effect entity (spec §4.E). Mirrors `MaterialBuffer.{hpp,cpp}` almost verbatim over `EffectParams` instead of `GpuMaterial`.

**Files:**
- Create: `src/engine/material/EffectParamBuffer.hpp`, `src/engine/material/EffectParamBuffer.cpp`
- Create: `tests/material/EffectParamBufferTests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create `src/engine/material/EffectParamBuffer.hpp`** (copy `MaterialBuffer.hpp` shape; drop the `IMaterialSlotSink` base — this buffer is used directly, not through the registry). Note the allocator/free-list are pure CPU, so the alloc/free/advance logic is unit-testable without a GPU; only `Write`/`Initialize` touch Vulkan:

```cpp
#pragma once

#include <cstdint>
#include <mutex>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

#include "material/DeferredSlotFreeList.hpp"
#include "material/EffectParams.hpp"

namespace aether
{
	// Persistently-mapped SSBO of per-entity EffectParams. Second instance of the
	// MaterialBuffer pattern (spec §4.E), but NOT content-addressed: AllocateSlot
	// hands out a unique slot per effect entity so two same-effect entities never
	// share params. FreeSlot defers reuse via DeferredSlotFreeList for GPU-in-flight
	// safety, identical to MaterialBuffer.
	class EffectParamBuffer
	{
	public:
		static constexpr std::uint32_t kMaxEffects = 4096; // reuse MaterialBuffer capacity; effect entities are few
		static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

		EffectParamBuffer() = default;
		~EffectParamBuffer();

		EffectParamBuffer(const EffectParamBuffer&) = AE_DELETE_MSG("use std::move");
		EffectParamBuffer& operator=(const EffectParamBuffer&) = AE_DELETE_MSG("use std::move");

		void Initialize();
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const { return m_handle.IsValid(); }

		[[nodiscard]] std::uint32_t AllocateSlot();
		void FreeSlot(std::uint32_t slot);
		void Write(std::uint32_t slot, const EffectParams& params);
		void AdvanceFrame(std::uint64_t frameIndex);

		[[nodiscard]] std::uint64_t GetDeviceAddressU64() const { return static_cast<std::uint64_t>(m_address); }

	private:
		mutable std::mutex m_mutex;
		gpu::BufferHandle m_handle{};
		EffectParams* m_mapped = nullptr;
		gpu::DeviceAddress m_address = 0;
		DeferredSlotFreeList m_slotAllocator;
	};
} // namespace aether
```

- [ ] **Step 2: Create `src/engine/material/EffectParamBuffer.cpp`** (copy `MaterialBuffer.cpp` body, swap types + debug name):

```cpp
#include "material/EffectParamBuffer.hpp"

#include "utils/Assert.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	EffectParamBuffer::~EffectParamBuffer()
	{
		Shutdown();
	}

	void EffectParamBuffer::Initialize()
	{
		AE_PROFILE_ZONE();
		const gpu::MappedBufferDesc desc{
		        .size = sizeof(EffectParams) * kMaxEffects,
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "EffectParamBuffer",
		};
		m_handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_handle.IsValid())
		{
			Throw(AetherError::Engine("EffectParamBuffer: CreateMappedBuffer failed"));
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_handle);
		m_mapped = static_cast<EffectParams*>(view.mappedPtr);
		m_address = view.deviceAddress;
		m_slotAllocator.Reset(kMaxEffects);
	}

	void EffectParamBuffer::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (!m_handle.IsValid())
		{
			return;
		}
		gpu::ResourceRegistry::Destroy(m_handle);
		m_handle = {};
		m_mapped = nullptr;
		m_address = 0;
		m_slotAllocator.Clear();
	}

	std::uint32_t EffectParamBuffer::AllocateSlot()
	{
		std::scoped_lock lock(m_mutex);
		return m_slotAllocator.Allocate();
	}

	void EffectParamBuffer::FreeSlot(std::uint32_t slot)
	{
		if (slot >= kMaxEffects)
		{
			return;
		}
		std::scoped_lock lock(m_mutex);
		m_slotAllocator.Free(slot);
	}

	void EffectParamBuffer::AdvanceFrame(std::uint64_t frameIndex)
	{
		std::scoped_lock lock(m_mutex);
		m_slotAllocator.AdvanceFrame(frameIndex);
	}

	void EffectParamBuffer::Write(std::uint32_t slot, const EffectParams& params)
	{
		if (slot >= kMaxEffects || m_mapped == nullptr)
		{
			return;
		}
		m_mapped[slot] = params;
		gpu::ResourceRegistry::FlushMappedBuffer(m_handle, static_cast<gpu::DeviceSize>(slot) * sizeof(EffectParams), sizeof(EffectParams));
	}
} // namespace aether
```

- [ ] **Step 3: Write a unit test for the per-entity (no-alias) + deferred-free invariants.** `Initialize`/`Write` need a GPU, so the unit test covers the allocator semantics via `DeferredSlotFreeList` directly (already covered by `tests/material/DeferredSlotFreeListTests.cpp` from ①) **plus** a slot-uniqueness test that does not require a live buffer. Create `tests/material/EffectParamBufferTests.cpp` exercising the allocator contract that guarantees the spec's "two entities, same effect → two distinct slots":

```cpp
#include <doctest/doctest.h>
#include "material/DeferredSlotFreeList.hpp"

using namespace aether;

// EffectParamBuffer's slot uniqueness + deferred-reuse come entirely from its
// DeferredSlotFreeList (no dedup layer on top, spec §4.E). These tests lock in
// the invariants the shared-slot design would violate, without needing a GPU.

TEST_CASE("Effect slots: two allocations are always distinct (no aliasing)") {
	DeferredSlotFreeList alloc;
	alloc.Reset(8);
	const std::uint32_t a = alloc.Allocate();
	const std::uint32_t b = alloc.Allocate();
	CHECK(a != DeferredSlotFreeList::kInvalidSlot);
	CHECK(b != DeferredSlotFreeList::kInvalidSlot);
	CHECK(a != b); // same-effect entities never collide
}

TEST_CASE("Effect slots: freed slot not reused until kReuseDelayFrames elapse") {
	DeferredSlotFreeList alloc;
	alloc.Reset(1); // single slot forces the deferral to matter
	const std::uint32_t s = alloc.Allocate();
	CHECK(s == 0u);
	alloc.Free(s); // retired at frame 0 + kReuseDelayFrames
	CHECK(alloc.Allocate() == DeferredSlotFreeList::kInvalidSlot); // not yet reusable

	for (std::uint64_t f = 1; f < DeferredSlotFreeList::kReuseDelayFrames; ++f)
	{
		alloc.AdvanceFrame(f);
		CHECK(alloc.Allocate() == DeferredSlotFreeList::kInvalidSlot);
	}
	alloc.AdvanceFrame(DeferredSlotFreeList::kReuseDelayFrames);
	CHECK(alloc.Allocate() == 0u); // now reusable
}
```

> This keeps the buffer's GPU-dependent `Write`/`Initialize` out of the unit test while still proving the load-bearing invariants (spec §7: "the exact bug the draft's shared-slot design would fail"). The end-to-end "two orbs tinted independently" check is in the Task 12 runtime hand-off.

- [ ] **Step 4: Register the test file** in `tests/CMakeLists.txt`:

```cmake
    material/EffectParamBufferTests.cpp
```

(Run `/sync-lsp`.)

- [ ] **Step 5: Build + run tests.**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests && ./build-vs2022-msvc/tests/Debug/EngineTests.exe`
Expected: PASS.

- [ ] **Step 6: Commit.**

```bash
git add src/engine/material/EffectParamBuffer.hpp src/engine/material/EffectParamBuffer.cpp tests/material/EffectParamBufferTests.cpp tests/CMakeLists.txt
git commit -m "feat(material): EffectParamBuffer (per-entity, no-dedup) + allocator invariants (TDD)"
```

---

## Task 6: `EffectParamsComponent` + `MaterialAsset.templateDesc`

**Files:**
- Modify: `src/engine/scene/Components.hpp:35-47`
- Modify: `src/engine/material/MaterialAsset.hpp`

- [ ] **Step 1: Add `EffectParamsComponent`** to `Components.hpp` after `MaterialInstanceComponent` (`:44-47`). It carries the entity's **independent** param-buffer slot (distinct from `MaterialComponent.gpuSlot`):

```cpp
	// Per-entity effect-parameter slot, independent of MaterialComponent.gpuSlot.
	// Its presence also marks the entity as effect-driven for the effect-override
	// rule in MaterialSystem::AssignMaterial. Freed via EffectSystem's on_destroy hook.
	struct EffectParamsComponent
	{
		std::uint32_t paramSlot = 0xFFFFFFFFu;
	};
```

- [ ] **Step 2: Add `templateDesc` to `MaterialAsset`.** In `MaterialAsset.hpp`, add the include and field. Default = today's standard gltf/opaque template (matches `BuildDefaultPipeline`):

```cpp
#include "material/MaterialTemplate.hpp"
// ... inside struct MaterialAsset, after the bool flags ...
		// The pipeline this surface draws with. doubleSided/alphaBlend below map
		// into templateDesc.cullMode/blendEnable at AssignMaterial time. Default is
		// the standard opaque gltf pipeline (today's BuildDefaultPipeline).
		MaterialTemplate templateDesc{.shaderVfsPath = "shaders://gltf_mesh.spv"};
```

> Adding a field to `MaterialAsset` changes the packed-hash input only if `PackMaterial` reads it — it does **not** (templates are a pipeline axis, not a `GpuMaterial` field), so material dedup is unaffected. `MaterialAsset` is authoring-only and never a GPU ABI struct, so no `static_assert` fires. Verify `MaterialInstanceComponent{asset}` and the `MaterialSystem::Set*` copy-on-write path still compile (they copy the whole asset, `MaterialSystem.cpp:75-124`).

- [ ] **Step 3: Compile-check.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: builds. (`/sync-lsp` not needed — no new files, no CMake change.)

- [ ] **Step 4: Commit.**

```bash
git add src/engine/scene/Components.hpp src/engine/material/MaterialAsset.hpp
git commit -m "feat(material): add EffectParamsComponent + MaterialAsset.templateDesc"
```

---

## Task 7: `AssignMaterial` resolves a pipeline via `PipelineCache` + effect-override rule

Extend the single ①-established seam so it also resolves the `PipelineComponent`, mapping `doubleSided`/`alphaBlend` into the template (their first real consumer, spec §1/§4.D), and honoring the **effect-override ordering contract**: if the entity has an `EffectParamsComponent`, the effect's template wins so a stray `set_material` repaints surface color without dropping the effect pipeline.

**Files:**
- Modify: `src/engine/material/MaterialSystem.hpp`, `src/engine/material/MaterialSystem.cpp`

- [ ] **Step 1: Extend the `AssignMaterial` signature** in `MaterialSystem.hpp` to take the `PipelineCache` and an optional effect-template override. Keep the ① signature working by adding the cache param (all callers updated in Tasks 9-10). Add includes for `PipelineCache` fwd-decl:

```cpp
	class PipelineCache;
	class GraphicsPipeline;
	// ...
		// Assign a material to an entity: acquire the material handle, resolve its
		// pipeline through the cache (mapping authored doubleSided/alphaBlend into
		// the template), store MaterialComponent{handle,slot} + PipelineComponent.
		// Effect-override rule: if the entity has an EffectParamsComponent, the
		// effect template supplied via effectTemplate (non-null) selects the pipeline
		// instead of asset.templateDesc, so set_material on an effect entity repaints
		// color without dropping the effect program (spec §4.D).
		void AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const MaterialAsset& asset);
```

- [ ] **Step 2: Implement pipeline resolution** in `MaterialSystem.cpp`. Update the existing `AssignMaterial` (`:31-49`) to also emplace `PipelineComponent`. Map the authored flags into a working copy of the template, then check for an existing `EffectParamsComponent` to apply the override:

```cpp
	void MaterialSystem::AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const MaterialAsset& asset)
	{
		auto& r = world.GetRegistry();
		const entt::entity e = World::ToEntt(entity);

		// -- material handle (unchanged ① acquire-before-release) --
		MaterialHandle old{};
		if (const auto* existing = r.try_get<MaterialComponent>(e))
		{
			old = existing->handle;
		}
		const MaterialHandle h = registry.Acquire(asset);
		registry.Release(old);
		r.emplace_or_replace<MaterialComponent>(e, MaterialComponent{h, registry.ResolveSlot(h)});

		// -- pipeline resolution (③) --
		// Effect-override: an effect entity keeps its effect pipeline even when a
		// stray set_material repaints its surface. The effect template is carried on
		// the entity's current PipelineComponent-driving asset only when it has an
		// EffectParamsComponent; otherwise the authored surface template wins.
		MaterialTemplate tmpl = asset.templateDesc;
		tmpl.cullMode = asset.doubleSided ? gpu::CullMode::None : gpu::CullMode::Back;
		tmpl.blendEnable = asset.alphaBlend;

		if (r.all_of<EffectParamsComponent>(e))
		{
			// Effect entity: do NOT overwrite the pipeline the effect set. Leave the
			// existing PipelineComponent intact (set_entity_effect owns it, §4.D).
			return;
		}

		const GraphicsPipeline* pipeline = pipelineCache.Acquire(tmpl);
		r.emplace_or_replace<PipelineComponent>(e, PipelineComponent{pipeline});
	}
```

Add includes at the top of `MaterialSystem.cpp`: `#include "material/PipelineCache.hpp"`, `#include "rendering/GraphicsPipeline.hpp"`, `#include "gpu/GpuTypes.hpp"`.

> **Design decision (documented):** rather than threading an `effectTemplate` argument through, the override is expressed as *"if the entity is effect-driven, `AssignMaterial` leaves the pipeline alone"*. `set_entity_effect` (Task 10) is the sole writer of an effect entity's `PipelineComponent`, so a later `set_material` updates `MaterialComponent` (color) but the early-return preserves the effect pipeline. This is simpler than passing a template and exactly matches the spec's "effect assignment is the last word" (§4.D). The `cullMode`/`blendEnable` mapping still runs for non-effect entities.

- [ ] **Step 3: Update the internal `Set*` copy-on-write callers.** The per-field setters (`SetBaseColor`/`SetMetallic`/... `MaterialSystem.cpp:72-125`) call `AssignMaterial(world, entity, registry, ref.inst.asset)`. They now need the cache. Add a `PipelineCache&` parameter to each `Set*` in the header and thread it through (callers in `EffectsModule.cpp` are reworked in Task 10; `MaterialAuthoring`/`WorldModule` callers updated in Task 9). This is a mechanical signature widening — do it now so the engine compiles, then fix call sites in Tasks 9-10.

- [ ] **Step 4: Compile-check the engine.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: **Engine builds**, but app/tests callers of the old `AssignMaterial`/`Set*` signatures now fail — that is expected and fixed in Tasks 9-10. If the Engine library itself has internal callers (e.g. `MaterialAuthoring`), update those here so `Engine` alone links.

> Check `MaterialAuthoring.cpp` for `AssignMaterial`/`Set*` calls (`grep -rn "AssignMaterial\|MaterialSystem::Set" src/engine`). Thread the cache through `MaterialAuthoring` (it already holds a `MaterialRegistry&`; give it a `PipelineCache&` too, wired in Task 8). Keep this task's Engine build green.

- [ ] **Step 5: Commit.**

```bash
git add src/engine/material/MaterialSystem.hpp src/engine/material/MaterialSystem.cpp src/engine/material/MaterialAuthoring.hpp src/engine/material/MaterialAuthoring.cpp
git commit -m "feat(material): AssignMaterial resolves pipeline via PipelineCache + effect-override rule"
```

---

## Task 8: `AssetSubsystem` owns `PipelineCache` + `EffectParamBuffer`; init, tick, teardown; effect lifecycle

**Files:**
- Modify: `src/engine/assets/AssetSubsystem.hpp:53-96`
- Modify: `src/engine/assets/AssetSubsystem.cpp:22-121`
- Create: `src/engine/material/EffectSystem.hpp`, `src/engine/material/EffectSystem.cpp`
- Modify: `src/app/layers/ScriptedSceneLayer.cpp` (feed `Context`), `src/engine/AetherCore.cpp` (packet wiring — see Task 11)

- [ ] **Step 1: Add members + getters to `AssetSubsystem.hpp`.** Declare `PipelineCache` and `EffectParamBuffer` after `m_materialBuffer`/`m_materialRegistry` (order matters only for the registry's sink reference; the cache/effect buffer have no such dependency). Add getters mirroring `GetMaterialBuffer`/`GetMaterialRegistry`:

```cpp
#include "material/PipelineCache.hpp"
#include "material/EffectParamBuffer.hpp"
// ...
		[[nodiscard]] PipelineCache& GetPipelineCache() { return m_pipelineCache; }
		[[nodiscard]] EffectParamBuffer& GetEffectParamBuffer() { return m_effectParamBuffer; }
		// Initialize the pipeline cache's frame-graph-constant Context + factory.
		// Called by the app layer once formats/heap-mappings are known.
		void InitializePipelineCache(PipelineCache::Context context);
// ...
	private:
		// ... existing ...
		PipelineCache m_pipelineCache;
		EffectParamBuffer m_effectParamBuffer;
```

- [ ] **Step 2: Wire init + tick + teardown in `AssetSubsystem.cpp`.**
  - In `Init` (`:36-42`), after `m_materialBuffer.Initialize();` add `m_effectParamBuffer.Initialize();`.
  - Add `InitializePipelineCache` that sets the cache's factory to `AssetManager::CreateGraphicsPipeline`:

```cpp
	void AssetSubsystem::InitializePipelineCache(PipelineCache::Context context)
	{
		m_pipelineCache.Initialize(context, [this](const GraphicsPipeline::Desc& desc)
		{
			return m_assetManager.CreateGraphicsPipeline(desc);
		});
	}
```

  - In `AdvanceFrame` (`:68-71`), add `m_effectParamBuffer.AdvanceFrame(frameIndex);` next to the material buffer tick.
  - In `Shutdown` (`:98-121`): the world-outlives guard already runs `MaterialSystem::DisconnectLifecycle`. Add `EffectSystem::DisconnectLifecycle(*m_world)` there too (before nulling `m_world`). After `m_materialBuffer.Shutdown();` add `m_effectParamBuffer.Shutdown();` and `m_pipelineCache.Shutdown();`.

> **Teardown ordering (spec §5 R-teardown):** `AssetSubsystem::Shutdown` runs inside the engine's GPU-idle-first shutdown (AGENTS.md). `PipelineCache::Shutdown` schedules deferred pipeline destruction; because the device is already idle / draining, the `VkShaderEXT` sets are reclaimed without a validation-layer leak — the same guarantee `EffectManager::DestroyAll` relied on. `m_pipelineCache.Shutdown()` must run **before** the `BindlessManager` it borrowed `descriptorHeapMappings` from is destroyed; `AssetSubsystem` shuts down before `GpuDevice`, so this holds. Add a `// cache borrows BindlessManager heap mappings; must shut down before BindlessManager` comment.

- [ ] **Step 3: Create `EffectSystem` for the param-slot lifecycle** (`src/engine/material/EffectSystem.{hpp,cpp}`), mirroring `MaterialSystem::ConnectLifecycle`/`DisconnectLifecycle` (`MaterialSystem.cpp:20-29`) but on `EffectParamsComponent` and freeing the slot through `EffectParamBuffer::FreeSlot` (deferred, spec §4.H):

```cpp
// EffectSystem.hpp
#pragma once
namespace aether
{
	class World;
	class EffectParamBuffer;
	namespace EffectSystem
	{
		// on_destroy<EffectParamsComponent> -> EffectParamBuffer::FreeSlot (deferred).
		void ConnectLifecycle(World& world, EffectParamBuffer& buffer);
		void DisconnectLifecycle(World& world);
	}
}
```

```cpp
// EffectSystem.cpp
#include "material/EffectSystem.hpp"
#include <entt/entt.hpp>
#include "material/EffectParamBuffer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		void OnEffectParamsDestroyed(EffectParamBuffer& buffer, entt::registry& r, entt::entity e)
		{
			buffer.FreeSlot(r.get<EffectParamsComponent>(e).paramSlot);
		}
	}
	void EffectSystem::ConnectLifecycle(World& world, EffectParamBuffer& buffer)
	{
		world.GetRegistry().on_destroy<EffectParamsComponent>().connect<&OnEffectParamsDestroyed>(buffer);
	}
	void EffectSystem::DisconnectLifecycle(World& world)
	{
		world.GetRegistry().on_destroy<EffectParamsComponent>().disconnect();
	}
}
```

Connect it in `AssetSubsystem::Init` after the material lifecycle is connected (find where `MaterialSystem::ConnectLifecycle` is called — it is invoked from `AssetManager::Initialize`; connect `EffectSystem::ConnectLifecycle(world, m_effectParamBuffer)` in `AssetSubsystem::Init` right after `m_assetManager.Initialize(...)` at `:57`, since `Init` has `world` in scope).

- [ ] **Step 4: Feed the cache Context from the app layer.** In `ScriptedSceneLayer::OnAttach` (`ScriptedSceneLayer.cpp:161`, replacing `BuildDefaultPipeline`), call:

```cpp
			auto& assetsSub = context.Get<aether::AssetSubsystem>();
			assetsSub.InitializePipelineCache({
			        .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
			        .depthFormat = context.Get<Swapchain>().GetDepthFormat(),
			        .descriptorHeapMappings = context.Get<BindlessManager>().GetDescriptorHeapMappings(),
			});
```

(Leave `BuildDefaultPipeline` deletion to Task 9 so the default-pipeline fallback is guaranteed first.)

- [ ] **Step 5: Compile-check the engine.**

Run: `/sync-lsp` (new `EffectSystem.*` files), then `cmake --build build-vs2022-msvc --config Debug --target Engine`
Expected: Engine builds. App still references removed/changed signatures — fixed in Tasks 9-10.

- [ ] **Step 6: Commit.**

```bash
git add src/engine/assets/AssetSubsystem.hpp src/engine/assets/AssetSubsystem.cpp src/engine/material/EffectSystem.hpp src/engine/material/EffectSystem.cpp
git commit -m "feat(material): AssetSubsystem owns PipelineCache+EffectParamBuffer; effect on_destroy lifecycle"
```

---

## Task 9: Route `add_mesh` / `set_material` through the cache; delete `BuildDefaultPipeline`

Guarantee **every rendered entity gets a `PipelineComponent`** before deleting the app-side default pipeline (spec §4.I step 3, R-material-less-mesh): `WorldRenderer::Flush` requires `PipelineComponent` (`WorldRenderer.cpp:34`).

**Files:**
- Modify: `src/app/scripting/modules/WorldModule.cpp:404-476` (create_mesh/add_mesh/set_material)
- Modify: `src/app/scripting/SceneContext.hpp:41,57-62` (drop raw pipeline)
- Modify: `src/app/layers/ScriptedSceneLayer.cpp` (delete `BuildDefaultPipeline`, `m_defaultPipeline`, `m_sceneCtx.defaultPipeline`)
- Modify: `src/app/layers/ScriptedSceneLayer.hpp` (drop `m_defaultPipeline`)
- Modify: `src/engine/assets/AssetManager.*` if `SpawnModel` takes a pipeline (see step 4)

- [ ] **Step 1: `add_mesh` always resolves a pipeline via `AssignMaterial`.** In `WorldModule.cpp` `das_add_mesh` (`:428-449`), the current code emplaces `PipelineComponent{entry.pipeline}` then conditionally assigns a material. Change so the pipeline comes from the cache through `AssignMaterial` (which now emplaces `PipelineComponent`), and drop the raw `entry.pipeline`:

```cpp
		const aether::Entity e{entityId};
		w->EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = entry.mesh});
		// AssignMaterial resolves the pipeline through PipelineCache (default template
		// for material-less meshes), so every add_mesh entity gets a PipelineComponent.
		aether::MaterialSystem::AssignMaterial(*w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), entry.materialAsset);
```

`entry.materialAsset` already defaults sensibly (`:420`, the primitive default with `modulateVertexColor=true`), and its `templateDesc` defaults to the gltf/opaque template — so the cache resolves the exact pipeline `BuildDefaultPipeline` produced. Remove the `if (ctx.assets)` guard's role in gating the pipeline (keep a null-check on `ctx.assets` but make it required for the mesh to render).

> `ctx.assets->GetPipelineCache()` requires `AssetManager` to expose the cache, or route through `AssetSubsystem`. Simplest: add `PipelineCache& AssetManager::GetPipelineCache()` returning the subsystem's cache (AssetManager already holds a `MaterialRegistry*`; give it a `PipelineCache*` set in `AssetManager::Initialize`, wired from `AssetSubsystem::Init` at `:57`). Update the `Initialize` signature to also take `PipelineCache&`.

- [ ] **Step 2: Drop the raw pipeline from `SceneContext`.** Remove `CachedMesh.pipeline` (`SceneContext.hpp:60`) and `SceneContext.defaultPipeline` (`:41`). Update `create_mesh` (`WorldModule.cpp:417-421`) to stop setting `entry.pipeline`, and remove the `!ctx.defaultPipeline` early-outs at `WorldModule.cpp:203,372` (SpawnModel path — see step 4). Update `das_create_mesh` guard `!ctx.primitives || !ctx.defaultPipeline` (`:372`) to just `!ctx.primitives`.

- [ ] **Step 3: `set_material` picks up the cache.** `das_set_material` (`:457-469`) already builds a bare `MaterialAsset` and calls `AssignMaterial`; add the cache argument. Because the asset has the default `templateDesc`, painting a non-effect entity resolves the gltf pipeline (unchanged behavior); painting an **effect** entity hits the override early-return (Task 7) and keeps the effect pipeline. Same for `das_make_material`/`bind_material`/`entity_material_set_*` — thread `ctx.assets->GetPipelineCache()` through every `AssignMaterial`/`MaterialSystem::Set*` call in `WorldModule.cpp` (grep within the file).

- [ ] **Step 4: SpawnModel default pipeline.** `ctx.assets->SpawnModel(*modelPtr, *ctx.defaultPipeline, id)` (`WorldModule.cpp:251`) passes the raw default pipeline. Change `AssetManager::SpawnModel` to resolve per-entity pipelines through `AssignMaterial`/the cache internally (it already assigns materials per primitive in ①), dropping the pipeline parameter. If `SpawnModel` currently emplaces `PipelineComponent{&defaultPipeline}` for each spawned mesh entity, replace that with the cache-resolved pipeline from each primitive's `MaterialAsset.templateDesc`. Update the `SpawnModel` signature and its header decl.

> Grep `grep -rn "SpawnModel" src` to find the declaration + all callers. This is the one non-mechanical change in Task 9 — the gltf material→pipeline resolution must go through `AssignMaterial` so gltf entities get cache-deduped pipelines instead of the shared raw default.

- [ ] **Step 5: Delete `BuildDefaultPipeline` + `m_defaultPipeline`.** Remove `ScriptedSceneLayer::BuildDefaultPipeline` (`.cpp:47-68`), its call (`:161`, now replaced by `InitializePipelineCache` from Task 8), `m_sceneCtx.defaultPipeline = &m_defaultPipeline;` (`:192`), and the `m_defaultPipeline` member in `ScriptedSceneLayer.hpp`. Keep the plasma effect registration (reworked in Task 10).

- [ ] **Step 6: Full build (engine + app).**

Run: `/sync-lsp`, then `cmake --build build-vs2022-msvc --config Debug`
Expected: Engine + App build. (EffectsModule may still reference old effect API — that is Task 10; if it blocks the App build, do Tasks 9 and 10 as one build unit and commit after Task 10's build passes. Prefer to land Task 9's engine+non-effects app changes, then Task 10.)

- [ ] **Step 7: Commit.**

```bash
git add src/app/scripting/modules/WorldModule.cpp src/app/scripting/SceneContext.hpp src/app/layers/ScriptedSceneLayer.cpp src/app/layers/ScriptedSceneLayer.hpp src/engine/assets/AssetManager.hpp src/engine/assets/AssetManager.cpp
git commit -m "refactor(material): route add_mesh/set_material/SpawnModel pipelines through PipelineCache; delete BuildDefaultPipeline"
```

---

## Task 10: Rework effects — `EffectDef` registry, per-entity `paramSlot`, single-`Write` setters

Collapse `EffectManager` to an `EffectDef{MaterialTemplate templateDesc; EffectParams defaultParams;}` registry (spec §4.I step 4). `set_entity_effect` sets the effect pipeline (via cache) + allocates a `paramSlot` + writes default params; `set_effect_*` become one `EffectParamBuffer::Write`. Parity defaults copied verbatim so the existing orb is byte-identical.

**Files:**
- Modify: `src/app/effects/EffectManager.hpp`, `src/app/effects/EffectManager.cpp`
- Modify: `src/app/layers/ScriptedSceneLayer.cpp:163-176,226` (register plasma `EffectDef`; drop `DestroyAll`)
- Modify: `src/app/scripting/modules/EffectsModule.cpp`

- [ ] **Step 1: Shrink `EffectManager` to a def registry.** In `EffectManager.hpp`, replace `EffectData{GraphicsPipeline pipeline; MaterialAsset material;}` with:

```cpp
	struct EffectDef
	{
		aether::MaterialTemplate templateDesc{}; // program + blend/cull/depth for the effect pipeline
		aether::EffectParams defaultParams{};    // seeded into the param buffer on set_entity_effect
	};
```

Replace `Register`/`CreateAndRegister`/`DestroyAll` with a simple `Register(const char* name, const EffectDef&)` + `Find`. Delete the `GraphicsPipeline`/`AssetManager` includes and the pipeline-owning code (`EffectManager.cpp:8-38,46-54`); the manager no longer owns GPU pipelines — `PipelineCache` does. Add includes for `MaterialTemplate.hpp`, `EffectParams.hpp`.

- [ ] **Step 2: Register plasma as an `EffectDef` in `ScriptedSceneLayer::OnAttach`** (replacing `:163-176`). Copy the parity defaults verbatim from the old `plasmaMat` (`ScriptedSceneLayer.cpp:168-173`): tint `(1, 0.3, 0.8)`, speed `0.5`, scale `2.0`, intensity `0.8` (spec §4.I step 5):

```cpp
		{
			aether::app::effects::EffectDef plasma;
			plasma.templateDesc.shaderVfsPath = "shaders://plasma.spv";
			plasma.templateDesc.depthWriteEnable = true; // matches old CreateAndRegister
			plasma.defaultParams.tint = glm::vec4(1.0f, 0.3f, 0.8f, 1.0f);
			plasma.defaultParams.speed = 0.5f;
			plasma.defaultParams.scale = 2.0f;
			plasma.defaultParams.intensity = 0.8f;
			m_effectManager.Register("plasma", plasma);
		}
```

Delete `m_effectManager.DestroyAll();` from `OnDetach` (`:226`) — there are no owned pipelines to destroy; the cache owns them and is torn down in `AssetSubsystem::Shutdown`.

- [ ] **Step 3: Rework `EffectsModule.cpp`.** `set_entity_effect` (`:19-39`) now:
  1. `Find` the `EffectDef`.
  2. Resolve the effect pipeline via `ctx.assets->GetPipelineCache().Acquire(def->templateDesc)` and emplace `PipelineComponent{pipeline}`.
  3. Reuse an existing `EffectParamsComponent.paramSlot` if present (no leak/double-alloc, spec §4.H); else `AllocateSlot()` on the effect buffer and emplace the component.
  4. `Write(paramSlot, def->defaultParams)`.

```cpp
	void das_set_entity_effect(aether::World* w, uint32_t id, const char* name)
	{
		auto& ctx = ActiveContext();
		if (!ctx.effects || !ctx.assets) { AE_WARN(aether::LogCategory::App, "set_entity_effect: no EffectManager/assets"); return; }
		const auto* def = ctx.effects->Find(name);
		if (!def) { AE_WARN(aether::LogCategory::App, "set_entity_effect: unknown effect '{}'", name); return; }

		const aether::Entity e{id};
		auto& buffer = ctx.assets->GetEffectParamBuffer();

		const aether::GraphicsPipeline* pipeline = ctx.assets->GetPipelineCache().Acquire(def->templateDesc);
		w->EmplaceOrReplace<aether::PipelineComponent>(e, aether::PipelineComponent{.pipeline = pipeline});

		std::uint32_t slot = aether::EffectParamBuffer::kInvalidSlot;
		if (const auto* existing = w->TryGet<aether::EffectParamsComponent>(e)) { slot = existing->paramSlot; }
		if (slot == aether::EffectParamBuffer::kInvalidSlot) { slot = buffer.AllocateSlot(); }
		w->EmplaceOrReplace<aether::EffectParamsComponent>(e, aether::EffectParamsComponent{slot});
		buffer.Write(slot, def->defaultParams);
	}
```

The four setters become one `Write` after mutating an in-CPU copy. Store the CPU-side `EffectParams` so a single-field edit does not clobber the others — easiest is to keep the live params in a component. **Add an `EffectParams params;` field to `EffectParamsComponent`** (it stays CPU-only, does not affect the GPU slot) so each setter reads-modifies-writes:

```cpp
	void das_set_effect_color(aether::World* w, uint32_t id, float r, float g, float b)
	{
		const aether::Entity e{id};
		auto* comp = w->TryGet<aether::EffectParamsComponent>(e);
		if (!comp) { return; }
		comp->params.tint = glm::vec4(r, g, b, comp->params.tint.w);
		ActiveContext().assets->GetEffectParamBuffer().Write(comp->paramSlot, comp->params);
	}
	// speed -> params.speed, scale -> params.scale, intensity -> params.intensity, same shape.
```

> **Amend Task 6 Step 1**: add `EffectParams params{};` to `EffectParamsComponent` (CPU-side authoritative copy). Update the `set_entity_effect` above to also store `def->defaultParams` into `comp.params`. This keeps the "one Write per slider, no material churn" property (spec §4.F) — no `AssignMaterial`, no re-`Acquire`. If you prefer to avoid revisiting Task 6, add the field there originally; it is called out here so the setters have a coherent CPU source.

- [ ] **Step 4: Update `SceneContext`/includes.** `SceneContext.effects` type is unchanged (still `EffectManager*`). Ensure `EffectsModule.cpp` includes `material/EffectParamBuffer.hpp`, `rendering/GraphicsPipeline.hpp`, `material/PipelineCache.hpp`, `scene/Components.hpp`.

- [ ] **Step 5: Full build.**

Run: `/sync-lsp`, then `cmake --build build-vs2022-msvc --config Debug`
Expected: Engine + App build clean.

- [ ] **Step 6: Run unit tests.**

Run: `./build-vs2022-msvc/tests/Debug/EngineTests.exe`
Expected: PASS (all ① + ③ cases).

- [ ] **Step 7: Commit.**

```bash
git add src/app/effects/EffectManager.hpp src/app/effects/EffectManager.cpp src/app/layers/ScriptedSceneLayer.cpp src/app/scripting/modules/EffectsModule.cpp src/engine/scene/Components.hpp
git commit -m "refactor(effects): EffectDef registry + per-entity EffectParamBuffer; single-Write setters (no material churn)"
```

---

## Task 11: Plumb `effectParamIndex` draw-side and `effectParamBufferAddr` frame-side; shader read

Carry the per-entity `paramSlot` to the fragment shader as a `nointerpolation` varying, and give the plasma shader the effect buffer BDA so it reads `EffectParams` by name (spec §4.F/§4.G).

**Files:**
- Modify: `src/engine/rendering/RenderQueue.hpp:41-56` (DrawCommand), `RenderQueue.cpp:497-504` (InstanceData write)
- Modify: `src/engine/rendering/WorldRenderer.cpp:46-50,78-91`
- Modify: `src/engine/rendering/RenderFramePacket.hpp`, `src/engine/gpu/GpuDevice.cpp:229`, `src/engine/AetherCore.cpp:485`
- Modify: `shaders/include/MeshVertex.slangh:19-30`, `shaders/include/DefaultVertex.slangh:76-86`
- Modify: `shaders/plasma.slang`

- [ ] **Step 1: `DrawCommand` gains the field.** In `RenderQueue.hpp` `DrawCommand` (`:41-56`) add next to `materialIndex`:

```cpp
		std::uint32_t effectParamIndex = 0xFFFFFFFFu; // per-entity EffectParams slot; 0xFFFF... = no effect
```

- [ ] **Step 2: Copy it into `InstanceData`.** In `RenderQueue.cpp` where the `InstanceData` is written (`:497-504`), add `.effectParamIndex = dc.effectParamIndex,` to the aggregate init (the field now exists at offset 76 from Task 1).

- [ ] **Step 3: `WorldRenderer::Flush` reads `EffectParamsComponent`.** In `WorldRenderer.cpp`, alongside the `materialIndex` read (`:46-50`), add:

```cpp
			std::uint32_t effectParamIndex = 0xFFFFFFFFu;
			if (const auto fx = world.GetRegistry().try_get<EffectParamsComponent>(enttEntity))
			{
				effectParamIndex = fx->paramSlot;
			}
```

and pass `.effectParamIndex = effectParamIndex,` in the `queue.Submit({...})` aggregate (`:78-91`).

- [ ] **Step 4: Forward the varying in the vertex stage.** In `MeshVertex.slangh` `VSOutput` (`:29`), add after `materialIndex`:

```hlsl
    [[vk::location(8)]] nointerpolation uint effectParamIndex : TEXCOORD8;
```

In `DefaultVertex.slangh` (`:85`), add `o.effectParamIndex = instance.effectParamIndex;` next to `o.materialIndex = instance.materialIndex;`.

> `gltf_mesh.slang` shares `DefaultVertex.slangh`, so its `VSOutput` gains the field too. That is fine — its fragment stage simply ignores `effectParamIndex`. Confirm the added varying does not exceed interpolator limits (8 `TEXCOORD` locations + `SV_Position` is comfortably within Vulkan's 32-location minimum).

- [ ] **Step 5: Frame-side BDA plumbing.** Add `std::uint64_t effectParamBufferAddr = 0;` to `RenderFramePacket` (near `materialBufferAddr`, `RenderFramePacket.hpp:48`). In `AetherCore.cpp` (`:485`), set `packet.effectParamBufferAddr = assetsSub.GetEffectParamBuffer().GetDeviceAddressU64();`. In `GpuDevice.cpp` (`:229`, where `fc.materialBufferAddr = packet.materialBufferAddr;`), add `fc.effectParamBufferAddr = packet.effectParamBufferAddr;`.

- [ ] **Step 6: Safety assert.** Where `FrameConstants` is finalized for submit (near `GpuDevice.cpp:229`), add a debug assert that the effect BDA is non-zero once effects are active. Since the buffer is always initialized (Task 8), a simpler unconditional guard is:

```cpp
		AE_ASSERT(fc.effectParamBufferAddr != 0, "EffectParamBuffer BDA not wired");
```

(The buffer is created at startup, so the address is always valid; this catches a mis-wire, spec §4.G.)

- [ ] **Step 7: Rewrite the plasma fragment to read `EffectParams` by name.** In `plasma.slang`, `#include "include/EffectParams.slangh"` (after the other includes, `:16`), then replace the material-field overloading (`:40,56-58,75-78`) with:

```hlsl
    FrameConstantsData* fc = pc.frame.Ptr();
    float elapsed = fc->elapsedTime;

    EffectParams fx;
    fx.tint = float4(1.0, 1.0, 1.0, 1.0);
    fx.speed = 1.0;
    fx.scale = 1.0;
    fx.intensity = 1.0;
    if (input.effectParamIndex != 0xFFFFFFFFu)
    {
        EffectParams* params = (EffectParams*)(fc->effectParamBufferAddr);
        fx = params[input.effectParamIndex];
    }

    float scale = max(fx.scale, 0.01);
    float speed = max(fx.speed, 0.0);
    float intensity = max(fx.intensity, 0.0);
    float3 tint = fx.tint.rgb;
```

Delete the now-unused `GpuMaterial* materials = ...` block and the `mat` scaffolding (`:40-59`) plus the base-color/vertex-color reads that fed off the material (the plasma output is fully procedural; keep `mat.baseColorFactor.a` usage by returning alpha `1.0`, or read tint alpha `fx.tint.a`). Keep `GpuMaterial.slangh` included only if still referenced; otherwise drop the include. Preserve the rest of the procedural body verbatim so the visual is byte-identical.

> **Parity check:** old defaults were tint `(1,0.3,0.8)`, speed `0.5`, scale `2.0`, intensity `0.8` (from `plasmaMat`), then the das script overrode color `(0.9,0.4,1.0)`, speed `1.0`, intensity `1.8` (`sandbox.das:473-475`). The new `EffectDef.defaultParams` (Task 10) reproduces the base, and the das setters reproduce the overrides through `EffectParamBuffer::Write`. The orb should look identical.

- [ ] **Step 8: Build engine + app + shaders.**

Run: `cmake --build build-vs2022-msvc --config Debug`
Run: `cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders`
Expected: all build; the 80/104/768 asserts still compile; `plasma.spv` + `gltf_mesh.spv` recompile without error.

- [ ] **Step 9: Run unit tests.**

Run: `./build-vs2022-msvc/tests/Debug/EngineTests.exe`
Expected: PASS.

- [ ] **Step 10: Commit.**

```bash
git add src/engine/rendering/RenderQueue.hpp src/engine/rendering/RenderQueue.cpp src/engine/rendering/WorldRenderer.cpp src/engine/rendering/RenderFramePacket.hpp src/engine/gpu/GpuDevice.cpp src/engine/AetherCore.cpp shaders/include/MeshVertex.slangh shaders/include/DefaultVertex.slangh shaders/plasma.slang
git commit -m "feat(material): plumb effectParamIndex draw-side + effectParamBufferAddr frame-side; plasma reads EffectParams by name"
```

---

## Task 12: Final build, cleanup, hand-off checklist

- [ ] **Step 1: Grep for stragglers.**

Run: `grep -rn "_padEnd\|InstanceData._pad0\|CreateAndRegister\|BuildDefaultPipeline\|defaultPipeline\|EffectData" src`
Expected: no live references to the removed symbols (comments/history aside). The renamed ABI fields (`effectParamIndex`, `effectParamBufferAddr`) should appear only in their intended sites. Fix any found.

- [ ] **Step 2: Confirm the frozen-ABI tripwires still compile.**

Run: `cmake --build build-vs2022-msvc --config Debug --target Engine`
Verify these `static_assert`s are unchanged and pass:
- `GpuMaterial` 80 bytes + 14 asserts (`GpuMaterial.hpp:48-61`) — **untouched by ③**.
- `FrameConstants` 768 bytes (`FrameConstants.hpp:110,136`) — offset-760 assert now names `effectParamBufferAddr`.
- `InstanceData` 104 bytes (`GpuContracts.hpp:41-48`) — offset-76 assert now names `effectParamIndex`.
- `EffectParams` 32 bytes (`EffectParams.hpp`).

- [ ] **Step 3: Full clean build + shaders + tests.**

Run: `cmake --build build-vs2022-msvc --config Debug`
Run: `cmake --build build-vs2022-msvc --config Debug --target App_CompileShaders`
Run: `./build-vs2022-msvc/tests/Debug/EngineTests.exe`
Expected: Engine + App build clean; shaders compile; all EngineTests pass.

- [ ] **Step 4: Commit any cleanup.**

```bash
git add -A
git commit -m "chore(material): remove ③ stragglers; verify frozen ABI asserts"
```

- [ ] **Step 5: Runtime hand-off checklist (user runs the GPU app — cannot run in-agent).**
  - **Existing scenes render unchanged:** gltf models (Fox/Human), rainbow cubes (vertex-color flag intact), painted toys — the default template resolves the same pipeline `BuildDefaultPipeline` built.
  - **The plasma orb (`sandbox.das:472-475`) looks identical** to before; `set_effect_color/speed/scale/intensity` update it live with **no per-frame material churn** (watch: no "MaterialBuffer is full" warnings while dragging).
  - **Spawn two plasma orbs and tint them differently** — they must show **independent** colors (the multi-entity case the single-orb sandbox lacks; this is the load-bearing bug ③ fixes, spec §7). Add a temporary second `set_entity_effect` in `sandbox.das` to verify, then revert.
  - **`set_material` on the plasma orb changes its surface color without dropping the plasma pipeline** (effect-override contract, §4.D).
  - **A material-less `add_mesh` still renders** (default-pipeline fallback via the cache).
  - **Reloading/destroying scenes leaks neither material nor param slots** over time (watch for buffer-full warnings across several F5 reloads), and **app exit tears down cached pipelines without a validation-layer leak** (run a Debug build with Vulkan validation enabled and confirm no `VkShaderEXT`/pipeline leak reports at exit).

---

## Self-Review

- **Spec coverage:** §4.A `MaterialTemplate` → Task 2; §4.B feature flags (no-op, ABI frozen) → verified in Reconciliation + Task 12 Step 2; §4.C `PipelineCache` → Task 3 (+ Context/teardown in Task 8); §4.D `AssignMaterial` pipeline resolution + effect-override → Task 7; §4.E `EffectParams`/`EffectParamBuffer` (no dedup) → Tasks 4-5; §4.F component + draw-side index + shader varying → Tasks 6, 11; §4.G `FrameConstants.effectParamBufferAddr` (ABI frozen) → Tasks 1, 11; §4.H effect lifecycle + deferred free → Task 8; §4.I migration steps 1-5 → Tasks 1-11 in order. All covered.
- **ABI honesty (spec §5 R-ABI):** ③ touches `GpuMaterial` **not at all** (its 14 asserts are the tripwire and cannot trip). `FrameConstants` (768) and `InstanceData` (104) change only via **field renames over existing reserved padding** at the same offsets — size-frozen, every assert holds, verified at Task 1 and re-verified at Task 12. `EffectParams` adds a *new* 32-byte ABI struct with its own asserts, in its own buffer; it never widens the two frozen contracts.
- **Hot-render-path honesty:** `DrawCommand`, `InstanceData`, and `VSOutput` each gain **one field; none changes size** (spec §4.F). `WorldRenderer::Flush` and `RenderQueue`'s per-draw write are on the hot path — the added reads are one `try_get` and one aggregate field, mirroring the existing `materialIndex` path exactly.
- **Effect-override design decision (deviation from a literal reading of §4.D):** the plan expresses "effect assignment is the last word" as *AssignMaterial early-returns before touching PipelineComponent when the entity has an EffectParamsComponent*, rather than threading an `effectTemplate` argument. Same observable behavior (set_material repaints color, keeps effect program), less plumbing. Documented inline in Task 7 Step 2.
- **CPU-side EffectParams copy:** the four setters need a coherent read-modify-write source; the plan stores the authoritative `EffectParams` on `EffectParamsComponent.params` (CPU-only, no GPU-slot impact). This is called out as an amendment to Task 6 Step 1 and used in Task 10 Step 3 — flagged so implementers add the field once, early.
- **Test strategy:** `PipelineCache` dedup/distinctness/pointer-stability over a fake factory (Task 3); `EffectParamBuffer`'s no-alias + deferred-reuse invariants via its `DeferredSlotFreeList` without a GPU (Task 5) — the two-entities-distinct-slots property the shared-slot design would fail is a unit test, per spec §7. GPU-dependent behavior (live `Write`, visual parity, two-orb independence) is deferred to the Task 12 runtime hand-off, honestly, because the app cannot run in-agent.
- **Incremental compile-checks:** every task ends at a green build of at least `Engine`; Tasks 3/5 also run tests. Tasks 9-10 are the one place where the App may not build until both land (effect API + call sites move together) — called out explicitly with a fallback of treating them as one build unit.
- **Scene preservation:** the plasma orb parity is pinned by copying the exact `plasmaMat` defaults into `EffectDef.defaultParams` (Task 10 Step 2) and re-checking the das overrides (Task 11 Step 7); `sandbox.das` is not modified except a temporary two-orb probe in the hand-off, reverted after.
