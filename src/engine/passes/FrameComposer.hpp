#pragma once

#include <glm/glm.hpp>

#include "rendering/FrameConstants.hpp"
#include "rendering/RenderThread.hpp"

namespace aether
{
	// Builds per-frame CPU constants from immutable frame packets.
	class FrameComposer
	{
	public:
		[[nodiscard]] FrameConstants ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj) const;

		void ApplyNoCameraLightingFallback(FrameConstants& fc) const;
	};
} // namespace aether
