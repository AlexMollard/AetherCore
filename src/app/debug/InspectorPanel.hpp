#pragma once

#include <string>

#include "debug/DebugPanel.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
} // namespace aether

namespace aether::app
{
	class InspectorPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Inspector";
		}

		void OnImGui(LayerContext& context) override;

	private:
		static bool IsAlive(const World& world, Entity entity);
		static std::string ComponentSummary(const World& world, Entity entity);
	};
} // namespace aether::app
