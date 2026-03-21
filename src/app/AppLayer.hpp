#pragma once

#include <cstdint>

namespace meow
{
	class MeowCore;
	class RenderQueue;
	class Scene;
}

namespace meow::app
{
	struct LayerContext
	{
		meow::MeowCore& engine;
		double deltaTimeSeconds = 0.0;
		std::uint64_t frameIndex = 0;
		meow::RenderQueue* renderQueue = nullptr;
		meow::Scene* scene = nullptr;
	};

	class AppLayer
	{
	public:
		virtual ~AppLayer() = default;

		virtual void OnAttach(LayerContext& context);
		virtual void OnDetach(LayerContext& context);
		virtual void OnUpdate(LayerContext& context);
		virtual void OnGui(LayerContext& context);
	};
}