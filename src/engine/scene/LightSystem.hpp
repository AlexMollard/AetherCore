#pragma once

#include <vector>

#include <entt/entity/entity.hpp>

#include "scene/System.hpp"

namespace aether
{
	class Renderer;

	class LightSystem final : public System
	{
	public:
		explicit LightSystem(Renderer& renderer)
		      : m_renderer(renderer)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "LightSystem";
		}

		[[nodiscard]] SceneFeatureFlags RequiredFeatures() const override
		{
			return SceneFeatureFlags::Lighting3D;
		}

		void Update(World& world, float dt) override;

	private:
		Renderer& m_renderer;
		std::vector<entt::entity> m_pointLightScratch;
		std::vector<entt::entity> m_spotLightScratch;
	};
} // namespace aether
