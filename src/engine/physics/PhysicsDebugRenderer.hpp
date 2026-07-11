#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Color.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuEnums.hpp"
#include "physics/PhysicsComponents.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether::gpu
{
	class CommandList;
} // namespace aether::gpu

namespace aether
{
	class World;

	class GpuDevice;

	void SetDebugRenderingEnabled(bool enabled);
	bool IsDebugRenderingEnabled();
	void SetPhysicsDebugShapesEnabled(bool enabled);
	bool IsPhysicsDebugShapesEnabled();

	enum class PhysicsDebugColorMode : uint8_t
	{
		None,
		ByMotionType, // Static=red, Dynamic=blue, Kinematic=green
	};

	// DebugVertex: per-vertex line endpoint. Matches the layout in debug_vert.slang.
	struct DebugVertex
	{
		glm::vec3 position;
		glm::vec4 color;
	};

	// One collider's wireframe instance, EXTRACTED from the ECS on the producer
	// thread into the RenderFramePacket. The render thread replays these (picking
	// the static per-shape vertex buffer by `shape`) so it never reads the world.
	struct PhysicsDebugInstance
	{
		glm::mat4 model{1.0f};
		glm::vec4 tint{1.0f};
		PhysicsShapeType shape = PhysicsShapeType::Box;
	};

	// Immediate-mode debug primitive builders. Append DebugVertex entries to the
	// supplied vector. Thread-safe by construction: each thread holds its own
	// std::vector and the render graph consumes it via the RenderFramePacket.
	// Positions are in world space; the shader applies the camera's viewProj.
	inline constexpr auto kDefaultDebugColor = glm::vec4(colors::DebugYellow);

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color = kDefaultDebugColor, int segments = 16);
	void AddDebugFrustum(std::vector<DebugVertex>& out, const glm::mat4& viewProj, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugAxes(std::vector<DebugVertex>& out, const glm::mat4& transform, float length = 1.0f);

	class PhysicsDebugRenderer
	{
	public:
		PhysicsDebugRenderer() = default;
		~PhysicsDebugRenderer();

		PhysicsDebugRenderer(const PhysicsDebugRenderer&) = delete;
		PhysicsDebugRenderer& operator=(const PhysicsDebugRenderer&) = delete;
		PhysicsDebugRenderer(PhysicsDebugRenderer&&) noexcept;
		PhysicsDebugRenderer& operator=(PhysicsDebugRenderer&&) noexcept;

		void Init(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown();

		void SetEnabled(bool enabled)
		{
			m_enabled = enabled;
		}

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		void SetColorMode(PhysicsDebugColorMode mode)
		{
			m_colorMode = mode;
		}

		[[nodiscard]] PhysicsDebugColorMode GetColorMode() const
		{
			return m_colorMode;
		}

		// Producer-thread extract: walk the ECS colliders and append a wireframe
		// instance (model matrix + tint + shape) for each into `out`. Called from
		// AetherCore::PrepareFrame; the render thread never touches the world.
		// No-op when physics-debug shapes are disabled.
		void ExtractShapes(const World& world, std::vector<PhysicsDebugInstance>& out) const;

		// Per-frame pointer to the extracted collider instances (lives in
		// RenderFramePacket::physicsDebugShapes). Set by ExecuteRenderFrame before
		// RenderGraph::Execute; the packet's channel transfer is the sync point.
		void SetFramePhysicsShapes(const std::vector<PhysicsDebugInstance>* shapes)
		{
			m_frameShapes = shapes;
		}

		// Whether the $PhysicsDebug pass draws this frame - copied from the packet
		// by ExecuteRenderFrame, so the render thread branches on this per-frame
		// snapshot instead of reading the mutable debug-toggle global.
		void SetFrameDebugEnabled(bool enabled)
		{
			m_frameDebugEnabled = enabled;
		}

		// Per-frame pointer to the immediate-mode debug vertex vector (lives in
		// RenderFramePacket::debugVertices). Set by the engine's ExecuteRenderFrame
		// before RenderGraph::Execute runs. Lock-free: the channel transfer of the
		// packet is the synchronization point.
		void SetFrameDebugVertices(const std::vector<DebugVertex>* vertices)
		{
			m_frameDebugVertices = vertices;
		}

		// Register the $Debug pass. The pass body draws (in this order):
		//   1. game-thread immediate-mode primitives from RenderFramePacket::debugVertices
		//   2. self-test pattern (gizmo + world AABB) when m_selfTestEnabled
		//   3. physics shape components (boxes/spheres/capsules) from the World when enabled
		void RegisterPass(RenderGraph& graph, RGImage color = {}, RGImage depth = {}, gpu::Extent2D extent = {});

		// Toggle the always-on self-test pattern. Useful for diagnosing whether
		// the pipeline is alive independent of any caller-supplied primitives.
		void SetSelfTestEnabled(bool enabled)
		{
			m_selfTestEnabled = enabled;
		}

		[[nodiscard]] bool IsSelfTestEnabled() const
		{
			return m_selfTestEnabled;
		}

	private:
		void CreateWireframePipeline(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat);
		void CreateBoxGeometry();
		void CreateSphereGeometry();
		void CreateCapsuleGeometry();
		void CreateCylinderGeometry();

		// Ensure the immediate vertex buffer can hold `vertexCount` vertices;
		// reallocates (destroy + create) if needed. The handle is owned by the
		// ResourceRegistry; this function's responsibility is the size policy.
		void EnsureImmediateBufferCapacity(std::uint32_t vertexCount);
		void DestroyImmediateBuffer();

		void DrawImmediateDebugPrimitives(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr);
		void DrawPhysicsDebugShapes(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr) const;

		// Append a self-test pattern (axis gizmo at origin + 1m world AABB + camera frustum)
		// to `out`. Used to verify the pipeline end-to-end.
		static void AppendSelfTestPattern(std::vector<DebugVertex>& out);

		bool m_enabled = false;
		bool m_selfTestEnabled = false;
		PhysicsDebugColorMode m_colorMode = PhysicsDebugColorMode::None;

		const std::vector<DebugVertex>* m_frameDebugVertices = nullptr;
		const std::vector<PhysicsDebugInstance>* m_frameShapes = nullptr;
		bool m_frameDebugEnabled = false;
		gpu::Format m_colorFormat = gpu::Format::Undefined;
		gpu::Format m_depthFormat = gpu::Format::Undefined;

		gpu::PipelineHandle m_pipelineHandle = {};

		// Pre-baked unit geometries for the per-shape (physics) draw path. These
		// are positioned by a per-draw MVP push constant. Lifetime is owned by
		// the ResourceRegistry (single m_pendingDestructions ring).
		gpu::BufferHandle m_boxVertexHandle = {};
		std::uint32_t m_boxVertexCount = 0;

		gpu::BufferHandle m_sphereVertexHandle = {};
		std::uint32_t m_sphereVertexCount = 0;

		gpu::BufferHandle m_capsuleVertexHandle = {};
		std::uint32_t m_capsuleVertexCount = 0;

		gpu::BufferHandle m_cylinderVertexHandle = {};
		std::uint32_t m_cylinderVertexCount = 0;

		// Batched vertex buffer for the per-frame vertex vector. Host-visible
		// (CpuToGpu memory); uploaded once per frame and drawn in a single draw.
		gpu::BufferHandle m_immediateVertexHandle = {};
		std::uint32_t m_immediateCapacity = 0;
	};
} // namespace aether
