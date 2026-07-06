#pragma once

#include <string>
#include <utility>

#include "scene/World.hpp"

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

		// Name of the most recently loaded scene (without extension), for editor UI
		// such as the status bar. Empty until the first scene is applied.
		[[nodiscard]] const std::string& GetCurrentScene() const
		{
			return m_currentScene;
		}

		void SetCurrentScene(std::string name)
		{
			m_currentScene = std::move(name);
		}

	private:
		World m_world;
		std::string m_currentScene;
	};
} // namespace aether
