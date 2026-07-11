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
	// the render thread. All fields are captured from game state BEFORE the render
	// thread starts executing, so there are no races with the next simulation tick.
	struct RenderFramePacket
	{
		// Camera matrices snapshotted at end of simulation.
		glm::mat4 view{1.0f};
		glm::mat4 proj{1.0f};
		glm::vec4 cameraWorldPos{0.0f};
		float cameraNearPlane = 0.1f;
		gpu::Extent2D renderExtent{};
		bool hasCameraData = false;

		// Lighting state snapshotted at end of simulation.
		glm::vec4 sunDirectionIntensity{0.0f, -1.0f, 0.0f, 1.0f};
		glm::vec4 ambientColor{0.2f, 0.2f, 0.2f, 1.0f};
		glm::vec4 sunColor{1.0f};
		glm::vec4 skyHorizonColor{1.0f};
		glm::vec4 skyZenithColor{0.5f, 0.7f, 1.0f, 1.0f};
		glm::vec4 skyVoidColor{0.0f};
		bool directionalShadowEnabled = false;

		// Local light lists snapshotted to avoid data race between game thread
		// (SetPointLights/SetSpotLights) and render thread reads.
		std::vector<Renderer::PointLight> pointLights;
		std::vector<Renderer::SpotLight> spotLights;

		// Debug primitive vertices accumulated on the game thread (DebugLayer, etc.)
		// and consumed by the $PhysicsDebug pass on the render thread. Lock-free:
		// the channel transfer is the synchronization point.
		std::vector<DebugVertex> debugVertices;

		// Collider wireframe instances extracted from the ECS on the producer
		// (PhysicsDebugRenderer::ExtractShapes) so the $PhysicsDebug pass never reads
		// the world. Empty unless physics-debug shapes are enabled.
		std::vector<PhysicsDebugInstance> physicsDebugShapes;

		// Whether the $PhysicsDebug pass should draw this frame. The producer reads
		// the debug toggle and puts the decision here; the render thread branches on
		// this packet field, never on a live global. (For lists like debugVertices /
		// physicsDebugShapes the toggle is expressed as data presence instead.)
		bool debugRenderingEnabled = false;

		// Stable GPU resource addresses.
		std::uint64_t materialBufferAddr = 0;
		std::uint64_t effectParamBufferAddr = 0;

		// Optional UI-overlay draw data snapshotted on the game thread and
		// consumed by the render thread after world/runtime UI/debug geometry.
		// Null whenever no IUiOverlay is installed (the shipped GameRuntime
		// never installs one - see AetherCore::SetUiOverlay). Opaque to the
		// engine core; only the overlay that produced it interprets it.
		std::unique_ptr<IUiOverlayFrameData> uiOverlay;

		// Frame identity - render thread uses these for GPU buffer slot selection.
		std::uint64_t frameIndex = 0;
		std::uint32_t drawSlot = 0;

		// Elapsed simulation time in seconds (for time-based shader effects).
		float elapsedTime = 0.0f;
	};
} // namespace aether
