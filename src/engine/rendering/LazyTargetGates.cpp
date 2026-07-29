#include "rendering/LazyTargetGates.hpp"

namespace aether
{
	bool LazyTargetGates::Update(Gate& gate, const bool requested)
	{
		gate.requested = requested;
		if (requested)
		{
			gate.idleFrames = 0;
			return !gate.enabled;
		}
		if (!gate.enabled)
		{
			return false;
		}
		if (gate.idleFrames < kReleaseFrames)
		{
			++gate.idleFrames;
		}
		return gate.idleFrames >= kReleaseFrames;
	}

	bool LazyTargetGates::CommitOne(Gate& gate)
	{
		bool next = gate.enabled;
		if (gate.requested)
		{
			next = true;
		}
		else if (gate.enabled && gate.idleFrames >= kReleaseFrames)
		{
			next = false;
		}

		const bool changed = next != gate.enabled;
		gate.enabled = next;
		gate.idleFrames = 0;
		return changed;
	}

	bool LazyTargetGates::Publish(const RenderContentSignals& signals)
	{
		// Directional cascades need both a sun and geometry that casts into them: a sunlit
		// scene with nothing but sprites in it has nothing to put in a shadow map. The
		// local atlas needs a light that was authored to cast AND geometry in front of it,
		// which is why a 2D scene full of shadow-flagged point lights allocates nothing.
		// GTAO is a screen-space effect over 3D geometry, so it needs draws.
		bool pending = Update(m_directionalShadow, signals.directionalLight && signals.shadowCasterDraws);
		pending = Update(m_localShadow, signals.localShadowLights && signals.shadowCasterDraws) || pending;
		pending = Update(m_gtao, signals.sceneDraws) || pending;
		pending = Update(m_texturePreview, signals.texturePreview) || pending;
		return pending;
	}

	bool LazyTargetGates::Commit()
	{
		bool changed = CommitOne(m_directionalShadow);
		changed = CommitOne(m_localShadow) || changed;
		changed = CommitOne(m_gtao) || changed;
		changed = CommitOne(m_texturePreview) || changed;
		return changed;
	}
} // namespace aether
