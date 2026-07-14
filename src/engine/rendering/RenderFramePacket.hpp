#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "gpu/GpuTypes.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/IUiOverlay.hpp"
#include "rendering/Renderer.hpp"

namespace aether
{
	// Per-frame render data snapshot produced by the game thread and consumed by
	struct RenderFramePacket
	{
		glm::mat4 view{1.0f};
		glm::mat4 proj{1.0f};
		glm::vec4 cameraWorldPos{0.0f};
		float cameraNearPlane = 0.1f;
		gpu::Extent2D renderExtent;
		bool hasCameraData = false;

		glm::vec4 sunDirectionIntensity{0.0f, -1.0f, 0.0f, 1.0f};
		glm::vec4 ambientColor{0.2f, 0.2f, 0.2f, 1.0f};
		glm::vec4 sunColor{1.0f};
		glm::vec4 skyHorizonColor{1.0f};
		glm::vec4 skyZenithColor{0.5f, 0.7f, 1.0f, 1.0f};
		glm::vec4 skyVoidColor{0.0f};
		bool directionalShadowEnabled = false;

		// Local light lists snapshotted to avoid data race between game thread
		std::vector<Renderer::PointLight> pointLights;
		std::vector<Renderer::SpotLight> spotLights;

		// Debug primitive vertices accumulated on the game thread (DebugLayer, etc.)
		std::vector<DebugVertex> debugVertices;

		// (PhysicsDebugRenderer::ExtractShapes) so the $PhysicsDebug pass never reads
		std::vector<PhysicsDebugInstance> physicsDebugShapes;

		// the debug toggle and puts the decision here; the render thread branches on
		bool debugRenderingEnabled = false;

		std::uint64_t materialBufferAddr = 0;
		std::uint64_t effectParamBufferAddr = 0;

		// Optional UI-overlay draw data snapshotted on the game thread and
		std::unique_ptr<IUiOverlayFrameData> uiOverlay;

		// Frame identity - render thread uses these for GPU buffer slot selection.
		std::uint64_t frameIndex = 0;
		std::uint32_t drawSlot = 0;

		float elapsedTime = 0.0f;
	};
} // namespace aether
