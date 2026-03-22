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
			glm::vec3 normal;
			glm::vec4 tangent;  // xyz = tangent direction, w = bitangent sign (+1 or -1)
			glm::vec2 uv;
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
		static Mesh Create(VkDevice device, VmaAllocator allocator, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);
		void Destroy();

		[[nodiscard]] bool          IsValid()        const { return m_buffer != VK_NULL_HANDLE; }
		[[nodiscard]] bool          IsIndexed()      const { return m_indexBuffer != VK_NULL_HANDLE; }
		[[nodiscard]] VkBuffer      GetBuffer()      const { return m_buffer; }
		[[nodiscard]] VkBuffer      GetIndexBuffer() const { return m_indexBuffer; }
		[[nodiscard]] std::uint32_t GetVertexCount() const { return m_vertexCount; }
		[[nodiscard]] std::uint32_t GetIndexCount()  const { return m_indexCount; }

	private:
		VkDevice      m_device = VK_NULL_HANDLE;
		VmaAllocator  m_allocator = nullptr;
		VkBuffer      m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = nullptr;
		std::uint32_t m_vertexCount = 0;
		VkBuffer      m_indexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_indexAllocation = nullptr;
		std::uint32_t m_indexCount = 0;
	};
}
