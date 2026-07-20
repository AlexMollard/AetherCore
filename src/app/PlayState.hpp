#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/TileMapAsset.hpp"
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

		// Pause is a sub-state of Playing: the session stays live (snapshot held,
		// scripts loaded) but the simulation is frozen. Meaningless outside Playing.
		[[nodiscard]] bool IsPaused() const
		{
			return m_mode == Mode::Playing && m_paused;
		}

		void SetPaused(bool paused)
		{
			if (m_mode == Mode::Playing)
			{
				m_paused = paused;
				m_stepPending = false;
			}
		}

		// Queue exactly one simulated frame while paused (Unity-style "Step").
		// Ignored unless currently paused.
		void RequestStep()
		{
			if (IsPaused())
			{
				m_stepPending = true;
			}
		}

		[[nodiscard]] bool HasPendingStep() const
		{
			return m_stepPending;
		}

		// Called once per frame by the update loop. Returns true when the
		// simulation should advance this frame and consumes any pending step:
		//   - not Playing            -> false
		//   - Playing, not paused    -> true (free-running)
		//   - Playing, paused        -> true only when a step was requested (once)
		[[nodiscard]] bool TakeSimulationStep()
		{
			if (m_mode != Mode::Playing)
			{
				return false;
			}
			if (!m_paused)
			{
				return true;
			}
			if (m_stepPending)
			{
				m_stepPending = false;
				return true;
			}
			return false;
		}

		// Record a simulated frame's timing for the play-mode HUD. Call only when
		// TakeSimulationStep() returned true, so paused wall-clock time is excluded.
		void RecordSimulatedFrame(double deltaSeconds)
		{
			m_playElapsedSeconds += deltaSeconds;
			++m_playFrameCount;
			m_lastFrameSeconds = deltaSeconds;
		}

		[[nodiscard]] double PlayElapsedSeconds() const
		{
			return m_playElapsedSeconds;
		}

		[[nodiscard]] std::uint64_t PlayFrameCount() const
		{
			return m_playFrameCount;
		}

		[[nodiscard]] double LastFrameSeconds() const
		{
			return m_lastFrameSeconds;
		}

		// Play-speed multiplier (slow-mo < 1, fast-forward > 1). Applied to the
		// simulation delta while playing; persists across Play sessions so a chosen
		// speed survives Stop -> Play. Clamped to a sane authoring range. 0 is a
		// valid value - a full freeze (gameDt becomes 0, so the world stops but the
		// script system still ticks): this is how an in-game pause menu works
		// (Unity's Time.timeScale = 0), driven from C# via Time.Pause().
		static constexpr float kMinTimeScale = 0.0f;
		static constexpr float kMaxTimeScale = 16.0f;

		[[nodiscard]] float TimeScale() const
		{
			return m_timeScale;
		}

		void SetTimeScale(float scale)
		{
			m_timeScale = scale < kMinTimeScale ? kMinTimeScale : (scale > kMaxTimeScale ? kMaxTimeScale : scale);
		}

		void SetMode(Mode mode)
		{
			if (mode != m_mode)
			{
				// Pause/step live only inside Playing; drop them on any transition.
				m_paused = false;
				m_stepPending = false;
				// A fresh Play session (entered from Editing or Compiling) restarts
				// the HUD timing.
				if (mode == Mode::Playing)
				{
					m_playElapsedSeconds = 0.0;
					m_playFrameCount = 0;
					m_lastFrameSeconds = 0.0;
				}
			}
			m_mode = mode;
		}

		std::optional<scene::SceneDescription> stopSnapshot;
		std::vector<Entity> stopSelection;
		Entity stopSelectionPrimary{};
		// Editor scene name at play start; scripts may Scene.Load() a different
		// scene mid-play, and stop must restore the name with the snapshot.
		std::string stopSceneName;
		// Tilemap cache at play start. Tile cells live in shared TileAssetStore assets,
		// not the ECS, so script-driven paints during play must revert on Stop like the
		// ECS snapshot does. Empty when no tilemaps were loaded at play start.
		std::unordered_map<std::string, TileMapAsset> tileStopSnapshot;

	private:
		Mode m_mode = Mode::Editing;
		bool m_paused = false;
		bool m_stepPending = false;
		double m_playElapsedSeconds = 0.0;
		double m_lastFrameSeconds = 0.0;
		std::uint64_t m_playFrameCount = 0;
		float m_timeScale = 1.0f;
	};
} // namespace aether::app
