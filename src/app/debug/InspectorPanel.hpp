#pragma once

#include <string>
#include <vector>

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
		static constexpr std::size_t kMaxSceneRows = 80;

		std::string_view GetName() const override
		{
			return "Inspector";
		}

		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		static bool IsAlive(const World& world, Entity entity);
		static std::string ComponentSummary(const World& world, Entity entity);
		static std::string SceneEntityLabel(const World& world, Entity entity);
		static std::vector<Entity> CollectSceneEntities(const World& world);

		Entity m_selectedSceneEntity;
	};
} // namespace aether::app
