#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vulkan/VulkanContext.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	class World;
	class CommandRecorder;
	class RenderGraph;

	void SetPhysicsDebugRenderingEnabled(bool enabled);
	bool IsPhysicsDebugRenderingEnabled();

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

		void Init(VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat);
		void Shutdown(VkDevice device);

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
		void CreateWireframePipeline(VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat);
		void CreateBoxGeometry(VmaAllocator allocator);
		void CreateSphereGeometry(VmaAllocator allocator);
		void CreateCapsuleGeometry(VmaAllocator allocator);

		// Ensure m_immediateVertexBuffer can hold `vertexCount` vertices; reallocates if needed.
		void EnsureImmediateBufferCapacity(VmaAllocator allocator, std::uint32_t vertexCount);
		void DestroyImmediateBuffer(VmaAllocator allocator);

		// Append a self-test pattern (axis gizmo at origin + 1m world AABB + camera frustum)
		// to `out`. Used to verify the pipeline end-to-end.
		void AppendSelfTestPattern(std::vector<DebugVertex>& out) const;

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		bool m_enabled = false;
		bool m_selfTestEnabled = true; // on by default to surface the pipeline immediately
		PhysicsDebugColorMode m_colorMode = PhysicsDebugColorMode::None;

		World* m_world = nullptr;
		const std::vector<DebugVertex>* m_frameDebugVertices = nullptr;
		VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
		VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

		// Pre-baked unit geometries for the per-shape (physics) draw path. These
		// are positioned by a per-draw MVP push constant.
		VkBuffer m_boxVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_boxVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_boxVertexCount = 0;

		VkBuffer m_sphereVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_sphereVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_sphereVertexCount = 0;

		VkBuffer m_capsuleVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_capsuleVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_capsuleVertexCount = 0;

		// Batched vertex buffer for the per-frame vertex vector. Host-visible; uploaded
		// once per frame and drawn in a single vkCmdDraw.
		VkBuffer m_immediateVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_immediateVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_immediateCapacity = 0;
	};
} // namespace aether
