#pragma once

#include <cstdint>
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

		void SetViewProj(const glm::mat4& viewProj)
		{
			m_viewProj = viewProj;
		}

		void RegisterPass(RenderGraph& graph);

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		bool m_enabled = false;
		PhysicsDebugColorMode m_colorMode = PhysicsDebugColorMode::None;

		World* m_world = nullptr;
		glm::mat4 m_viewProj{1.0f};
		VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
		VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

		VkPipeline m_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

		VkBuffer m_boxVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_boxVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_boxVertexCount = 0;

		VkBuffer m_sphereVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_sphereVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_sphereVertexCount = 0;

		VkBuffer m_capsuleVertexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_capsuleVertexAlloc = VK_NULL_HANDLE;
		std::uint32_t m_capsuleVertexCount = 0;

		void CreateWireframePipeline(VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat);
		void CreateBoxGeometry(VmaAllocator allocator);
		void CreateSphereGeometry(VmaAllocator allocator);
		void CreateCapsuleGeometry(VmaAllocator allocator);
	};
} // namespace aether
