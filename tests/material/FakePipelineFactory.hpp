#pragma once
#include "rendering/GraphicsPipeline.hpp"

struct FakePipelineFactory
{
	int buildCount = 0;

	aether::Expected<aether::GraphicsPipeline> operator()(const aether::GraphicsPipeline::Desc&)
	{
		++buildCount;
		return aether::GraphicsPipeline{};
	}
};
