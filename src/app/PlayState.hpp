#pragma once

#include <optional>
#include <vector>

#include "scene/SceneSerializer.hpp"

namespace aether::app
{
	// Editor simulation state, registered as a service by Application.
	//
	// Editing: physics/animation/day-night systems and script on_update are
	// frozen (Application gates World::UpdateSystems; ScriptedSceneLayer gates
	// CallOnUpdate). Pending physics body descriptors still flush so loaded or
	// newly created entities get pickable bodies. Camera, picking, gizmo and
	// the inspector stay live.
	//
	// Playing: everything simulates. `stopSnapshot` holds the scene captured
	// when Play was pressed; Stop restores it in-place and returns to Editing,
	// preserving unsaved authored entity ids and editor references.
	class PlayState
	{
	public:
		enum class Mode
		{
			Editing,
			Compiling, // Play pressed; the async script build is running. Sim stays
			           // frozen (as in Editing) and the editor stays interactive until
			           // the build completes and we transition to Playing.
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

		// True while waiting on the async script build before Play begins.
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

	private:
		Mode m_mode = Mode::Editing;
	};
} // namespace aether::app
