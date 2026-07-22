#pragma once

#include <array>
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
		// Pixel-art sampling: the shader snaps UVs to texel centres, emulating
		// a nearest-neighbour sampler (set from the atlas' filter metadata).
		NearestFilter = 1u << 4u,
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
		// Sprites AND tile instances: tile extraction appends here so one sort
		// interleaves both by sort key. Finalize2DFrame must run after the last
		// producer and before submission.
		std::vector<SpriteRenderInstance> sprites;
	};

	// Deterministic sort of the merged 2D instance stream (stable, by sortKey).
	void Finalize2DFrame(Render2DFrameData& frame);

	// One drawn ink capsule (world-space segment a->b, half-thickness `width`). The ink
	// field pass SDF-unions all of these into a single continuous wet-ink layer. 32 bytes,
	// laid out to match the InkSegment struct in shaders/ink_field.slang.
	struct InkSegmentGpu
	{
		glm::vec2 a{0.0f};   // world endpoint A
		glm::vec2 b{0.0f};   // world endpoint B
		float width = 0.2f;  // half-thickness, world units
		float alpha = 1.0f;  // 0..1 age fade
		float glow = 1.0f;   // rim-glow strength
		float ghost = 0.0f;  // 0 = solid ink, 1 = unanchored (red, crumbling)
	};
	static_assert(sizeof(InkSegmentGpu) == 32);

	// Packet-owned snapshot of the live ink field for the render thread.
	struct RenderInkFrameData
	{
		std::vector<InkSegmentGpu> segments;
		glm::vec4 bodyColor{0.05f, 0.09f, 0.13f, 1.0f}; // dark ink body
		glm::vec4 rimColor{0.30f, 0.85f, 1.0f, 1.0f};   // cyan wet rim
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

		// Camera-owned background, composited WYSIWYG in the tonemap pass.
		// backgroundMode mirrors CameraBackground (0 solid, 1 gradient, 2 sky).
		// Stops pack xyz = display-space colour, w = position (0..1).
		static constexpr std::uint32_t kMaxBackgroundStops = 8;
		std::uint32_t backgroundMode = 2; // 2 = SkyGradient (composite disabled)
		float backgroundAngleRadians = 0.0f;
		std::uint32_t backgroundStopCount = 0;
		std::array<glm::vec4, kMaxBackgroundStops> backgroundStops{};

		// Local light lists snapshotted to avoid data race between game thread
		std::vector<Renderer::PointLight> pointLights;
		std::vector<Renderer::SpotLight> spotLights;

		// Debug primitive vertices accumulated on the game thread (DebugLayer, etc.)
		std::vector<DebugVertex> debugVertices;

		// (PhysicsDebugRenderer::ExtractShapes) so the $PhysicsDebug pass never reads
		std::vector<PhysicsDebugInstance> physicsDebugShapes;

		Render2DFrameData render2D;
		RenderInkFrameData renderInk;

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
