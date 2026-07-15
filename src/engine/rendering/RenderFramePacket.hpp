#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "gpu/GpuTypes.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/IUiOverlay.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneKind.hpp"

namespace aether
{
	enum class SpriteInstanceFlags : std::uint32_t
	{
		None = 0,
		FlipX = 1u << 0u,
		FlipY = 1u << 1u,
		PixelSnap = 1u << 2u,
		Masked = 1u << 3u,
	};

	struct SpriteRenderInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
		glm::vec4 color{1.0f};
		glm::vec4 sizeAndPivot{1.0f, 1.0f, 0.5f, 0.5f};
		std::uint64_t sortKey = 0;
		std::uint32_t textureIndex = 0;
		std::uint32_t entityId = 0;
		SpriteInstanceFlags flags = SpriteInstanceFlags::None;
		std::uint32_t blendMode = 0;
	};

	// Owned by RenderFramePacket and moved through the render-thread queue with
	// it. The game thread finishes extraction before submission; the render
	// thread may retain references only for the synchronous execution of that
	// packet. It must never reach back into the ECS or asset authoring objects.
	struct Render2DFrameData
	{
		std::vector<SpriteRenderInstance> sprites;
		std::vector<SpriteRenderInstance> tileInstances;
	};

	// Per-frame render data snapshot produced by the game thread and consumed by
	// the render thread. All dynamic arrays are packet-owned.
	struct RenderFramePacket
	{
		glm::mat4 view{1.0f};
		glm::mat4 proj{1.0f};
		glm::vec4 cameraWorldPos{0.0f};
		float cameraNearPlane = 0.1f;
		gpu::Extent2D renderExtent;
		bool hasCameraData = false;
		SceneFeatureFlags sceneFeatures = DefaultSceneFeatures(SceneKind::Scene3D);

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

		Render2DFrameData render2D;

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
