#pragma once

#include <cstdint>

namespace aether
{
	// What the frame actually contains, measured on the game thread once the render
	// queues have been filled and the lights extracted.
	//
	// Half a gigabyte of shadow, AO and preview targets is allocated or released off the
	// back of these, so every field describes render CONTENT: what was submitted, what
	// lights came out of the world, what a host tool asked to see. Scene feature flags
	// say nothing here and must never appear - every scene in this engine carries every
	// feature flag, so a flag can never tell you whether a scene has a shadow caster in
	// it. Gate on a flag and you get a scene that silently renders without shadows.
	struct RenderContentSignals
	{
		// The main render queue received at least one 3D mesh draw.
		bool sceneDraws = false;
		// The shadow render queues received at least one shadow-casting mesh draw.
		bool shadowCasterDraws = false;
		// A directional light with non-zero intensity is lighting the scene.
		bool directionalLight = false;
		// At least one extracted point or spot light has its shadow flag set.
		bool localShadowLights = false;
		// A GPU texture preview is being displayed by a host tool right now.
		bool texturePreview = false;
	};

	// Decides which lazily-allocated render target groups this session should be holding.
	//
	// Growing is instant: the frame that first shows the content is at most one frame
	// ahead of the targets existing, and one unshadowed frame is a far cheaper mistake
	// than a stall. Shrinking waits for the content to stay away for kReleaseFrames, so a
	// session that flickers between states - a Play/Stop round trip, a light toggled off
	// and on, a load that briefly empties the world - keeps its targets instead of paying
	// a graph rebuild each way.
	//
	// Pure state, no GPU: creating and destroying the resources is the caller's job.
	class LazyTargetGates
	{
	public:
		// ~2 seconds at 60 fps. Long enough to swallow a scene load or a Play/Stop round
		// trip, short enough that a second editor on the same GPU gets the memory back
		// promptly.
		static constexpr std::uint32_t kReleaseFrames = 120;

		// Feeds one frame of measured content in. Returns true when a gate wants to move
		// and the caller should schedule a commit.
		[[nodiscard]] bool Publish(const RenderContentSignals& signals);

		// Applies whatever Publish asked for. Returns true when any gate changed, which
		// is the caller's cue to create/release targets and rebuild the render graph.
		bool Commit();

		[[nodiscard]] bool DirectionalShadowTargets() const
		{
			return m_directionalShadow.enabled;
		}

		[[nodiscard]] bool LocalShadowTargets() const
		{
			return m_localShadow.enabled;
		}

		[[nodiscard]] bool GtaoTargets() const
		{
			return m_gtao.enabled;
		}

		[[nodiscard]] bool TexturePreviewTarget() const
		{
			return m_texturePreview.enabled;
		}

	private:
		struct Gate
		{
			// Committed state: what the current render graph was built against.
			bool enabled = false;
			// What the most recent frame's content asks for.
			bool requested = false;
			// Consecutive frames the content has been absent while still enabled.
			std::uint32_t idleFrames = 0;
		};

		[[nodiscard]] static bool Update(Gate& gate, bool requested);
		[[nodiscard]] static bool CommitOne(Gate& gate);

		Gate m_directionalShadow;
		Gate m_localShadow;
		Gate m_gtao;
		Gate m_texturePreview;
	};
} // namespace aether
