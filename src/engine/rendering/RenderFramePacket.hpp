#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/Render2DFrameData.hpp"
#include "gpu/MeshPass.hpp"
#include "gpu/GpuTypes.hpp"
#include "physics/PhysicsDebugData.hpp"
#include "rendering/IUiOverlay.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneKind.hpp"

namespace aether
{
	// Where a project-registered custom render pass is injected into the frame graph. Both stages draw
	// into the scene HDR colour (before tonemap); more insertion points can be added as needed.
	enum class CustomPassStage : std::uint8_t
	{
		BehindScene2D = 0, // before sprites/tiles
		OverScene2D = 1,   // after sprites/tiles, before the 2D light map (so the pass is lit/shadowed)
		// After the 2D light map: the pass is NOT darkened by lighting. For things that emit their own
		// light (a glowing ink stroke, a spell, a magic projectile) - they still light and shadow the
		// world via their own submitted lights/occluders, but keep their full brightness themselves.
		EmissiveOverLight2D = 2,
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
		// When this frame sampled input, as a steady_clock epoch count. Carried all the way to
		// the present so the present-wait thread can report true input-to-photon latency; it
		// is the only place both ends of that journey are known.
		std::int64_t latchTimeNs = 0;
		// The extent the SCENE is rendered at. Scaled by graphics.renderScale, so clustered
		// light binning and the post-process chain agree with the actual target size.
		gpu::Extent2D renderExtent;
		// The extent the UI is laid out and drawn at - never scaled, because UI goes straight
		// to the swapchain after the upscale. Equals renderExtent inside the editor viewport,
		// where the UI is composited into the scene texture instead.
		gpu::Extent2D uiExtent;
		bool hasCameraData = false;
		SceneFeatureFlags sceneFeatures = DefaultSceneFeatures(SceneKind::Scene3D);

		// True when the main render queue received at least one 3D mesh draw this frame.
		// Render-side "is there anything 3D here" decisions key off this, never off
		// sceneFeatures: every scene carries every feature flag, so a flag can never tell
		// you what a scene actually contains.
		bool hasSceneDraws = false;

		// Towards-sun convention (matches FrameConstants usage); overwritten from the
		// renderer every frame anyway.
		glm::vec4 sunDirectionIntensity{0.0f, 1.0f, 0.0f, 1.0f};
		glm::vec4 ambientColor{0.2f, 0.2f, 0.2f, 1.0f};

		// 2D light-map settings (from Light2DSettingsComponent, else defaults). Decoupled from the shared
		// ambientColor so 2D mood doesn't disturb 3D. rgb = ambient floor; shadowParams = (strength, softness).
		glm::vec4 light2DAmbient{0.03f, 0.04f, 0.06f, 1.0f};
		glm::vec4 light2DShadowParams{0.94f, 1.0f, 0.0f, 0.0f};
		// A scene opts into the 2D light map by having lights OR a Light2DSettings component. With
		// settings but no lights the map still runs, so the scene sits at its authored ambient (dark)
		// instead of rendering unlit - otherwise a lightless scene looks washed-out next to lit ones.
		bool light2DHasSettings = false;
		glm::vec4 sunColor{1.0f};
		glm::vec4 skyHorizonColor{1.0f};
		glm::vec4 skyZenithColor{0.5f, 0.7f, 1.0f, 1.0f};
		glm::vec4 skyVoidColor{0.0f};
		glm::vec4 fogParams{0.0f, 0.08f, 0.6f, 0.9f};
		glm::vec4 skyParams{0.0f, 2.5f, 0.0f, 0.0f};
		glm::vec4 shadingParams{1.0f, 0.0f, 0.0f, 1.0f};
		// PS2 object lighting (kFlagObjectLit materials), from the scene environment.
		glm::vec4 objectAmbient{1.0f, 1.0f, 1.0f, 1.0f};
		glm::vec4 objectLight0Direction{0.0f, 1.0f, 0.0f, 0.0f};
		glm::vec4 objectLight0Color{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 objectLight1Direction{0.0f, 1.0f, 0.0f, 0.0f};
		glm::vec4 objectLight1Color{0.0f, 0.0f, 0.0f, 0.0f};
		// Contact blob shadows under object-lit actors, nearest the camera first (xyz = bounds
		// centre, w = bounds radius). Only the first kMaxBlobShadows reach the GPU.
		std::vector<glm::vec4> blobShadows;
		// How many of blobShadows (at the front) are character actors': they get a contact core.
		std::uint32_t blobCharacterCount = 0;
		// Darkness volumes, two vec4 per volume (see FrameConstants::darknessVolumes), nearest
		// the camera first. Only the first kMaxDarknessVolumes reach the GPU.
		std::vector<glm::vec4> darknessVolumes;
		bool contactShadows = false;
		// 0 splits the cascades evenly across the view distance, 1 logarithmically.
		float shadowSplitLambda = 0.65f;
		bool directionalShadowEnabled = false;

		// Camera-owned background, composited WYSIWYG in the tonemap pass.
		// backgroundMode mirrors CameraBackground (0 solid, 1 gradient, 2 sky).
		// Stops pack xyz = display-space colour, w = position (0..1).
		static constexpr std::uint32_t kMaxBackgroundStops = 8;
		// Depth of field from the main camera: x = focus distance, y = the optics folded
		// into one coefficient, z = the pixel ceiling on the blur circle, w = enabled.
		glm::vec4 dofParams{10.0f, 0.0f, 0.0f, 0.0f};
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
		Render3DFrameData render3D;
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
