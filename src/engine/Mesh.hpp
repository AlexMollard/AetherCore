#pragma once

#include <cstdint>
#include <span>

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace meow
{
	// GPU-resident vertex buffer. Created via MeowCore::CreateMesh — app code
	// never touches VMA or VkBuffer directly.
	class Mesh
	{
	public:
		struct Vertex
		{
			glm::vec3 position;
			glm::vec3 color;
		};

		Mesh() = default;
		~Mesh();

		Mesh(const Mesh&) = delete;
		Mesh& operator=(const Mesh&) = delete;

		Mesh(Mesh&&) noexcept;
		Mesh& operator=(Mesh&&) noexcept;

		// Engine-internal factory used by MeowCore::CreateMesh.
		static Mesh Create(VkDevice device, VmaAllocator allocator, std::span<const Vertex> vertices);
		void Destroy();

		[[nodiscard]] bool          IsValid()        const { return m_buffer != VK_NULL_HANDLE; }
		[[nodiscard]] VkBuffer      GetBuffer()      const { return m_buffer; }
		[[nodiscard]] std::uint32_t GetVertexCount() const { return m_vertexCount; }

	private:
		VkDevice      m_device = VK_NULL_HANDLE;
		VmaAllocator  m_allocator = nullptr;
		VkBuffer      m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = nullptr;
		std::uint32_t m_vertexCount = 0;
	};
}
