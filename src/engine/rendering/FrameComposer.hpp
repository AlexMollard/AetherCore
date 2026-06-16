#pragma once

#include <glm/glm.hpp>

#include "rendering/FrameConstants.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	// Builds per-frame CPU constants from immutable frame packets.
	class FrameComposer
	{
	public:
		[[nodiscard]] static FrameConstants ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj);

		static void ApplyNoCameraLightingFallback(FrameConstants& fc);
	};
} // namespace aether
