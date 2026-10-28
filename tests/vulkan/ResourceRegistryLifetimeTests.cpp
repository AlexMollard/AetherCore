#include <vector>
#include <doctest/doctest.h>

#include "vulkan/ResourceRegistry.hpp"

TEST_CASE("Resolved pipeline state survives registry growth until its handle is destroyed")
{
	aether::ResourceRegistry registry;
	std::vector<aether::gpu::PipelineHandle> handles;
	handles.reserve(513);
	aether::ResourceRegistry::PipelineEntry original;
	original.isGraphics = true;
	original.depthTestEnable = VK_TRUE;
	handles.push_back(registry.RegisterPipeline(original, "Retained draw state"));
	const auto* retained = registry.Resolve(handles.front());
	REQUIRE(retained != nullptr);
	for (unsigned i = 0; i < 512; ++i)
	{
		handles.push_back(registry.RegisterPipeline({}, "Additional shader state"));
	}
	REQUIRE(registry.Resolve(handles.front()) == retained);
	CHECK(retained->isGraphics);
	CHECK(retained->depthTestEnable == VK_TRUE);
	for (const auto handle: handles)
	{
		registry.Destroy(handle);
	}
	CHECK(registry.Resolve(handles.front()) == nullptr);
	registry.DrainAll();
}
