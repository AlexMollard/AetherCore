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
