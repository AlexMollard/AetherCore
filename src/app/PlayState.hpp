#pragma once

#include <optional>
#include <vector>

#include "scene/SceneSerializer.hpp"

namespace aether::app
{
	class PlayState
	{
	public:
		enum class Mode
		{
			Editing,
			Compiling,
			Playing,
		};

		[[nodiscard]] Mode GetMode() const
		{
			return m_mode;
		}

		[[nodiscard]] bool IsPlaying() const
		{
			return m_mode == Mode::Playing;
		}

		[[nodiscard]] bool IsCompiling() const
		{
			return m_mode == Mode::Compiling;
		}

		void SetMode(Mode mode)
		{
			m_mode = mode;
		}

		std::optional<scene::SceneDescription> stopSnapshot;
		std::vector<Entity> stopSelection;
		Entity stopSelectionPrimary{};
		// Editor scene name at play start; scripts may Scene.Load() a different
		// scene mid-play, and stop must restore the name with the snapshot.
		std::string stopSceneName;

	private:
		Mode m_mode = Mode::Editing;
	};
} // namespace aether::app
