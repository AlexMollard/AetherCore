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

	// Occluder2D::flags bits.
	inline constexpr std::uint32_t kOccluder2DFlipX = 1u << 0u;
	inline constexpr std::uint32_t kOccluder2DFlipY = 1u << 1u;
	// Capsule form (scripts): posHalfSize = (a.xy, radius), uvRect.xy = b; no texture is sampled.
	inline constexpr std::uint32_t kOccluder2DCapsule = 1u << 2u;

	// One shadow caster. Tiles use the quad form: the pass draws the cell and samples the tile's own
	// texture, so occlusion follows the ARTWORK's alpha (sub-tile) rather than the whole cell. Scripts
	// use the capsule form for transient strokes/trails.
	// Mirrors GpuOccluder2D in shaders/occluder2d.slang (48 bytes).
	struct Occluder2D
	{
		glm::vec4 posHalfSize{0.0f};              // quad: xy = world centre, z = half cell | capsule: xy = a, z = radius
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // quad: atlas region (image space) | capsule: xy = b
		std::uint32_t textureIndex = 0;
		std::uint32_t flags = 0; // see kOccluder2D* above
		std::uint32_t pad0 = 0;
		std::uint32_t pad1 = 0;
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

		// Shadow occluders for 2D lighting: one entry per SOLID tile cell. The Light2D pass rasterises
		// these into an occluder mask (sampling each tile's texture, so occlusion is sub-tile) and
		// ray-marches it for shadows. Backgrounds and sprites do not occlude.
		std::vector<Occluder2D> occluders;
	};

	// Deterministic sort of the merged 2D instance stream (stable, by sortKey).
	void Finalize2DFrame(Render2DFrameData& frame);

	// Where a project-registered custom render pass is injected into the frame graph. Both stages draw
	// into the scene HDR colour (before tonemap); more insertion points can be added as needed.
	enum class CustomPassStage : std::uint8_t
	{
		BehindScene2D = 0, // before sprites/tiles
		OverScene2D = 1,   // after sprites/tiles
	};

	// One submission of a project-registered custom pass for this frame. The engine knows nothing
	// about what the pass draws: it binds `data` (a raw float4 buffer the project shader interprets)
	// plus params/colours as push constants and runs the named project shader over a fullscreen quad
	// at the requested stage. See shaders' generic CustomPass push contract.
	struct CustomPassDraw
	{
		std::string shader;                               // project shader stem -> shaders://<shader>.spv
		CustomPassStage stage = CustomPassStage::OverScene2D;
		std::vector<glm::vec4> data;                      // script-filled records (shader-defined layout)
		glm::vec4 params{0.0f};
		glm::vec4 color0{0.0f};
		glm::vec4 color1{0.0f};
	};

	// Packet-owned snapshot of every custom pass submitted by scripts this frame.
	struct RenderCustomPassData
	{
		std::vector<CustomPassDraw> passes;
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

		// 2D light-map settings (from Light2DSettingsComponent, else defaults). Decoupled from the shared
		// ambientColor so 2D mood doesn't disturb 3D. rgb = ambient floor; shadowParams = (strength, softness).
		glm::vec4 light2DAmbient{0.03f, 0.04f, 0.06f, 1.0f};
		glm::vec4 light2DShadowParams{0.94f, 1.0f, 0.0f, 0.0f};
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
		RenderCustomPassData renderCustom;

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
