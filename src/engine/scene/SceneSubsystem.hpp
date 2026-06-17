#pragma once

#include "scene/World.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	class SceneSubsystem
	{
	public:
		static void Init();

		void Shutdown()
		{
		}

		[[nodiscard]] World& GetWorld()
		{
			return m_world;
		}

	private:
		World m_world;
	};
} // namespace aether
