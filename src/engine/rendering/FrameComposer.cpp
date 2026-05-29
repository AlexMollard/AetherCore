#include "rendering/FrameComposer.hpp"

namespace aether
{
	FrameConstants FrameComposer::ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj) const
	{
		FrameConstants fc{};

		if (packet.hasCameraData)
		{
			fc.view = packet.view;
			fc.proj = packet.proj;
			fc.viewProj = packet.proj * packet.view;
			fc.cameraWorldPos = packet.cameraWorldPos;
		}
		else
		{
			fc.viewProj = fallbackViewProj;
		}

		fc.materialBufferAddr = packet.materialBufferAddr;
		fc.elapsedTime = packet.elapsedTime;
		fc.sunDirectionIntensity = packet.sunDirectionIntensity;
		fc.ambientColor = packet.ambientColor;
		fc.sunColor = packet.sunColor;
		fc.skyHorizonColor = packet.skyHorizonColor;
		fc.skyZenithColor = packet.skyZenithColor;
		fc.skyVoidColor = packet.skyVoidColor;
		return fc;
	}

	void FrameComposer::ApplyNoCameraLightingFallback(FrameConstants& fc) const
	{
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
	}
} // namespace aether
