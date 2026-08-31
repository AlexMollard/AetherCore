#include <doctest/doctest.h>
#include "material/MaterialTemplate.hpp"
#include "material/PipelineCache.hpp"
#include "FakePipelineFactory.hpp"

#include <cstdint>
#include <utility>

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
	CHECK(a == b);
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
		t.fragmentVfsPath = (i % 3 == 0) ? "a" : (i % 3 == 1) ? "b" : "c";
		CHECK(cache.Acquire(t) != nullptr);
	}
	// The original node's address must not have moved (node-stable container).
	CHECK(cache.Acquire(GltfTemplate()) == first);
}

// A shader recompiled while the editor is running has to REPLACE the cached pipeline.
// Without this the cache handed out the pipeline built from the previous bytes, so a
// material graph compiled correctly and nothing on screen changed.
TEST_CASE("PipelineCache::Reload rebuilds the pipelines built from one shader") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	MaterialTemplate opaque = GltfTemplate();
	MaterialTemplate blended = GltfTemplate(); blended.blendEnable = true;
	MaterialTemplate other; other.shaderVfsPath = "shaders://plasma.spv";
	cache.Acquire(opaque);
	cache.Acquire(blended);
	cache.Acquire(other);
	CHECK(factory.buildCount == 3);

	CHECK(cache.Reload("shaders://gltf_mesh.spv") == 2);
	// Both variants of that shader rebuilt; the unrelated one was left alone.
	CHECK(factory.buildCount == 5);
	CHECK(cache.Size() == 3);
}

// Materials hold the pointer Acquire returned, so a reload that moved the entry would leave
// every one of them pointing at freed memory.
TEST_CASE("PipelineCache::Reload keeps the pointer materials already hold") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	const GraphicsPipeline* before = cache.Acquire(GltfTemplate());
	cache.Reload("shaders://gltf_mesh.spv");
	CHECK(cache.Acquire(GltfTemplate()) == before);
}

TEST_CASE("PipelineCache::Reload ignores a shader nothing was built from") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	cache.Acquire(GltfTemplate());
	CHECK(cache.Reload("shaders://never_used.spv") == 0);
	CHECK(factory.buildCount == 1);
}

// The replaced pipeline may still be read by a frame already submitted, so it is destroyed
// only once those frames have retired - and it must not survive a shutdown in between.
TEST_CASE("PipelineCache retires a replaced pipeline instead of destroying it immediately") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	cache.Acquire(GltfTemplate());
	cache.Reload("shaders://gltf_mesh.spv");
	// Advancing far enough past the frame it was retired on drains the pending list; the
	// live entry is untouched either way.
	cache.AdvanceFrame(1);
	CHECK(cache.Size() == 1);
	cache.AdvanceFrame(64);
	CHECK(cache.Size() == 1);
	CHECK(cache.Acquire(GltfTemplate()) != nullptr);
	CHECK(factory.buildCount == 2);
}

namespace
{
	// Stands in for the driver half of a reload. The "prepared pipeline" is an opaque void* to
	// everything above the Vulkan layer, so a counter cast to a pointer is a faithful fake.
	struct FakePrepare
	{
		int prepared = 0;
		int committed = 0;
		int discarded = 0;

		[[nodiscard]] PipelineCache::PrepareFn Prepare()
		{
			return [this](const GraphicsPipeline::Desc&)
			{
				++prepared;
				return gpu::ResourceRegistry::PreparedPipeline{reinterpret_cast<void*>(static_cast<std::uintptr_t>(prepared))};
			};
		}

		[[nodiscard]] PipelineCache::CommitFn Commit()
		{
			return [this](gpu::ResourceRegistry::PreparedPipeline) -> Expected<GraphicsPipeline>
			{
				++committed;
				return GraphicsPipeline{};
			};
		}

		[[nodiscard]] PipelineCache::DiscardFn Discard()
		{
			return [this](gpu::ResourceRegistry::PreparedPipeline) { ++discarded; };
		}
	};
} // namespace

// The split exists so the driver's SPIR-V compile - tens of milliseconds - happens on a
// worker while only the swap runs on the thread that owns the registry.
TEST_CASE("PrepareReload builds one pipeline per affected template without touching the cache") {
	PipelineCache cache;
	FakePipelineFactory factory;
	FakePrepare fake;
	cache.Initialize({}, std::ref(factory), fake.Prepare(), fake.Commit(), fake.Discard());

	MaterialTemplate opaque = GltfTemplate();
	MaterialTemplate blended = GltfTemplate(); blended.blendEnable = true;
	MaterialTemplate other; other.shaderVfsPath = "shaders://plasma.spv";
	const GraphicsPipeline* before = cache.Acquire(opaque);
	cache.Acquire(blended);
	cache.Acquire(other);

	auto prepared = cache.PrepareReload("shaders://gltf_mesh.spv");
	CHECK(prepared.size() == 2);
	CHECK(fake.prepared == 2);
	// Nothing has been swapped yet: the old pipelines must keep rendering until commit.
	CHECK(fake.committed == 0);
	CHECK(cache.Acquire(opaque) == before);

	CHECK(cache.CommitReload(std::move(prepared)) == 2);
	CHECK(fake.committed == 2);
	// Same entry, rebuilt in place, so every material still holding this pointer is fine.
	CHECK(cache.Acquire(opaque) == before);
	CHECK(cache.Size() == 3);
}

// Abandoned prepared pipelines own real driver objects that nothing else will ever free.
TEST_CASE("DiscardPrepared hands back pipelines that are never committed") {
	PipelineCache cache;
	FakePipelineFactory factory;
	FakePrepare fake;
	cache.Initialize({}, std::ref(factory), fake.Prepare(), fake.Commit(), fake.Discard());

	cache.Acquire(GltfTemplate());
	auto prepared = cache.PrepareReload("shaders://gltf_mesh.spv");
	REQUIRE(prepared.size() == 1);

	cache.DiscardPrepared(std::move(prepared));
	CHECK(fake.discarded == 1);
	CHECK(fake.committed == 0);
}

TEST_CASE("PrepareReload is empty when no pipeline uses that shader") {
	PipelineCache cache;
	FakePipelineFactory factory;
	FakePrepare fake;
	cache.Initialize({}, std::ref(factory), fake.Prepare(), fake.Commit(), fake.Discard());

	cache.Acquire(GltfTemplate());
	CHECK(cache.PrepareReload("shaders://never_used.spv").empty());
	CHECK(fake.prepared == 0);
}

// A cache initialised the old way must still work: the synchronous Reload is what everything
// other than the material editor uses.
TEST_CASE("PrepareReload is inert when no async factory was supplied") {
	PipelineCache cache;
	FakePipelineFactory factory;
	cache.Initialize({}, std::ref(factory));

	cache.Acquire(GltfTemplate());
	CHECK(cache.PrepareReload("shaders://gltf_mesh.spv").empty());
	CHECK(cache.Reload("shaders://gltf_mesh.spv") == 1);
}
