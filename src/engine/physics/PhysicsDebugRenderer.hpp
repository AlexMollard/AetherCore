#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuEnums.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether::gpu
{
	class CommandList;
} // namespace aether::gpu

namespace aether
{
	class World;
	class RenderGraph;

	class GpuDevice;

	void SetDebugRenderingEnabled(bool enabled);
	bool IsDebugRenderingEnabled();

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

	// Immediate-mode debug primitive builders. Append DebugVertex entries to the
	// supplied vector. Thread-safe by construction: each thread holds its own
	// std::vector and the render graph consumes it via the RenderFramePacket.
	// Positions are in world space; the shader applies the camera's viewProj.
	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f});
	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f});
	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f});
	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f}, int segments = 16);
	void AddDebugFrustum(std::vector<DebugVertex>& out, const glm::mat4& viewProj, const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f});
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

		void SetWorld(World* world)
		{
			m_world = world;
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
		//   3. physics shape components (boxes/spheres/capsules) from the World
		void RegisterPass(RenderGraph& graph);

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
		bool m_selfTestEnabled = true; // on by default to surface the pipeline immediately
		PhysicsDebugColorMode m_colorMode = PhysicsDebugColorMode::None;

		World* m_world = nullptr;
		const std::vector<DebugVertex>* m_frameDebugVertices = nullptr;
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

		// Batched vertex buffer for the per-frame vertex vector. Host-visible
		// (CpuToGpu memory); uploaded once per frame and drawn in a single draw.
		gpu::BufferHandle m_immediateVertexHandle = {};
		std::uint32_t m_immediateCapacity = 0;
	};
} // namespace aether
