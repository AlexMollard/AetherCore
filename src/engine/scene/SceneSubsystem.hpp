#pragma once

#include "scene/Scene.hpp"
#include "scene/World.hpp"

namespace aether { class ServiceContainer; }

namespace aether
{
	// Owns the ECS world and the legacy scene container.
	class SceneSubsystem
	{
	public:
		void Init();

		void Shutdown()
		{
		}

		[[nodiscard]] World& GetWorld()
		{
			return m_world;
		}

		[[nodiscard]] Scene& GetScene()
		{
			return m_scene;
		}

	private:
		World m_world;
		Scene m_scene;
	};
} // namespace aether
