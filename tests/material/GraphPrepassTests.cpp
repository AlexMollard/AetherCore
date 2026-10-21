#include <doctest/doctest.h>
#include <cstring>
#include <vector>
#include "gpu/SpirvEntryPoints.hpp"
#include "material/PipelineCache.hpp"

using namespace aether;

namespace
{
	std::vector<std::byte> GraphModule(std::uint32_t stage = 4)
	{
		std::vector<std::uint32_t> words{0x07230203, 0x00010600, 0, 16, 0};
		constexpr std::string_view name = "graphPrepassMain";
		const auto nameWords = (name.size() + 1 + 3) / 4;
		words.push_back(static_cast<std::uint32_t>(3 + nameWords) << 16 | 15);
		words.push_back(stage);
		words.push_back(1);
		const auto at = words.size();
		words.resize(at + nameWords);
		std::memcpy(words.data() + at, name.data(), name.size());
		std::vector<std::byte> bytes(words.size() * 4);
		std::memcpy(bytes.data(), words.data(), bytes.size());
		return bytes;
	}

	PipelineCache::ShaderReader GraphReader()
	{
		return [](std::string_view) -> Expected<std::vector<std::byte>>
		{
			return GraphModule();
		};
	}

	MaterialTemplate GraphTemplate()
	{
		return {.shaderVfsPath = "shaders://published-graph.spv"};
	}
} // namespace

TEST_CASE("SPIR-V graph capability requires an exact fragment entry and bounded instructions")
{
	CHECK(gpu::HasSpirvFragmentEntry(GraphModule(), "graphPrepassMain").value());
	CHECK_FALSE(gpu::HasSpirvFragmentEntry(GraphModule(0), "graphPrepassMain").value());
	CHECK_FALSE(gpu::HasSpirvFragmentEntry(GraphModule(), "graphPrepass").value());
	auto bad = GraphModule();
	bad.resize(bad.size() - 1);
	CHECK_FALSE(gpu::HasSpirvFragmentEntry(bad, "graphPrepassMain"));
	bad = GraphModule();
	bad.insert(bad.end(), 4, std::byte{0});
	CHECK_FALSE(gpu::HasSpirvFragmentEntry(bad, "graphPrepassMain"));
	bad = GraphModule();
	for (std::size_t i = 32; i < bad.size(); ++i)
	{
		bad[i] = std::byte{0xff};
	}
	CHECK_FALSE(gpu::HasSpirvFragmentEntry(bad, "graphPrepassMain"));
}

TEST_CASE("Graph prepass acquisition builds an opaque two-target companion or fails together")
{
	PipelineCache cache;
	bool reject = true;
	int builds = 0;
	cache.Initialize(
	        {},
	        [&](const GraphicsPipeline::Desc& desc) -> Expected<GraphicsPipeline>
	        {
		        ++builds;
		        if (desc.fragmentEntry == "graphPrepassMain")
		        {
			        CHECK(desc.colorAttachmentCount == 2);
			        CHECK(desc.depthWriteEnable);
			        CHECK_FALSE(desc.blendEnable);
			        if (reject)
			        {
				        return std::unexpected(AetherError::Engine("prepass failure"));
			        }
		        }
		        return GraphicsPipeline{};
	        },
	        {},
	        {},
	        {},
	        GraphReader());
	CHECK(cache.Acquire(GraphTemplate()) == nullptr);
	CHECK(cache.Size() == 0);
	reject = false;
	const auto* graph = cache.Acquire(GraphTemplate());
	REQUIRE(graph);
	REQUIRE(graph->GetScenePrepass());
	GraphicsPipeline fallback;
	CHECK(SelectScenePrepassPipeline(graph, &fallback, false) == graph->GetScenePrepass());
	CHECK(SelectScenePrepassPipeline(&fallback, &fallback, false) == &fallback);
	CHECK(SelectScenePrepassPipeline(graph, &fallback, true) == nullptr);
	CHECK(builds == 4);
}

TEST_CASE("Graph synchronous reload preserves the old pair when companion rebuilding fails")
{
	PipelineCache cache;
	bool reject = false;
	cache.Initialize(
	        {},
	        [&](const GraphicsPipeline::Desc& desc) -> Expected<GraphicsPipeline>
	        {
		        if (reject && desc.fragmentEntry == "graphPrepassMain")
		        {
			        return std::unexpected(AetherError::Engine("prepass failure"));
		        }
		        return GraphicsPipeline{};
	        },
	        {},
	        {},
	        {},
	        GraphReader());
	const auto* graph = cache.Acquire(GraphTemplate());
	REQUIRE(graph);
	const auto* oldPrepass = graph->GetScenePrepass();
	reject = true;
	CHECK(cache.Reload("shaders://published-graph.spv") == 0);
	CHECK(graph->GetScenePrepass() == oldPrepass);
	reject = false;
	CHECK(cache.Reload("shaders://published-graph.spv") == 1);
	CHECK(cache.Acquire(GraphTemplate()) == graph);
	CHECK(graph->GetScenePrepass() != oldPrepass);
	cache.AdvanceFrame(64);
	REQUIRE(graph->GetScenePrepass());
}

TEST_CASE("Graph asynchronous reload commits atomically and discards both abandoned preparations")
{
	PipelineCache cache;
	int prepared = 0, committed = 0, discarded = 0;
	bool failCompanion = false;
	cache.Initialize(
	        {},
	        [](const GraphicsPipeline::Desc&) -> Expected<GraphicsPipeline> { return GraphicsPipeline{}; },
	        [&](const GraphicsPipeline::Desc&) { return gpu::ResourceRegistry::PreparedPipeline{reinterpret_cast<void*>(static_cast<std::uintptr_t>(++prepared))}; },
	        [&](gpu::ResourceRegistry::PreparedPipeline) -> Expected<GraphicsPipeline>
	        {
		        ++committed;
		        if (failCompanion && committed % 2 == 0)
		        {
			        return std::unexpected(AetherError::Engine("prepass commit failure"));
		        }
		        return GraphicsPipeline{};
	        },
	        [&](gpu::ResourceRegistry::PreparedPipeline) { ++discarded; },
	        GraphReader());
	const auto* graph = cache.Acquire(GraphTemplate());
	REQUIRE(graph);
	const auto* old = graph->GetScenePrepass();
	auto pending = cache.PrepareReload("shaders://published-graph.spv");
	REQUIRE(pending.size() == 1);
	CHECK(prepared == 2);
	failCompanion = true;
	CHECK(cache.CommitReload(std::move(pending)) == 0);
	CHECK(graph->GetScenePrepass() == old);
	failCompanion = false;
	CHECK(cache.CommitReload(cache.PrepareReload("shaders://published-graph.spv")) == 1);
	CHECK(graph->GetScenePrepass() != old);
	cache.DiscardPrepared(cache.PrepareReload("shaders://published-graph.spv"));
	CHECK(discarded == 2);
	CHECK(committed == 4);
}
