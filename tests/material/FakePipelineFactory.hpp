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
