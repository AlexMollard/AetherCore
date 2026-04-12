#pragma once

#include <cstdint>

namespace meow
{
	class MeowCore;
	class Scene;
	class World;
	class Input;
}

namespace meow::app
{
	struct LayerContext
	{
		meow::MeowCore& engine;
		double deltaTimeSeconds = 0.0;
		std::uint64_t frameIndex = 0;
		meow::Scene* scene = nullptr;
		meow::World* world = nullptr;
		meow::Input* input = nullptr;
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