#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace aether
{
	// GPU-resident vertex buffer. Created via AetherCore::CreateMesh — app code
	// never touches VMA or VkBuffer directly.
	class Mesh
	{
	public:
		struct Vertex
		{
			glm::vec3 position;
			glm::vec3 normal;
			glm::vec4 tangent; // xyz = tangent direction, w = bitangent sign (+1 or -1)
			glm::vec2 uv;
			glm::vec3 color;
			glm::uvec4 jointIndices{ 0u, 0u, 0u, 0u };
			glm::vec4 jointWeights{ 1.0f, 0.0f, 0.0f, 0.0f };
		};

		Mesh() = default;
		~Mesh();

		Mesh(const Mesh&) = delete;
		Mesh& operator=(const Mesh&) = delete;

		Mesh(Mesh&&) noexcept;
		Mesh& operator=(Mesh&&) noexcept;

		// Engine-internal factory used by AetherCore::CreateMesh.
		// Uploads via a staging buffer to device-local memory; call after the upload
		// pool is created (blocks until the queue is idle).
		static Mesh Create(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, std::span<const Vertex> vertices);
		static Mesh Create(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices);

		// Create a non-owning view into an externally managed buffer (e.g. MeshArena).
		// The returned Mesh does NOT free the backing memory when destroyed (allocator is null).
		static Mesh CreateView(VkBuffer vertexBuffer, VkBuffer indexBuffer, std::uint32_t vertexCount, std::uint32_t indexCount, VkDeviceSize vertexByteOffset = 0, VkDeviceSize indexByteOffset = 0);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_buffer != VK_NULL_HANDLE;
		}

		[[nodiscard]] bool IsIndexed() const
		{
			return m_indexBuffer != VK_NULL_HANDLE;
		}

		[[nodiscard]] VkBuffer GetBuffer() const
		{
			return m_buffer;
		}

		[[nodiscard]] VkBuffer GetIndexBuffer() const
		{
			return m_indexBuffer;
		}

		[[nodiscard]] std::uint32_t GetVertexCount() const
		{
			return m_vertexCount;
		}

		[[nodiscard]] std::uint32_t GetIndexCount() const
		{
			return m_indexCount;
		}

		// Byte offset within the vertex buffer to pass to vkCmdBindVertexBuffers.
		// Zero for standalone Mesh objects; non-zero for MeshArena views.
		[[nodiscard]] VkDeviceSize GetVertexByteOffset() const
		{
			return m_vertexByteOffset;
		}

		// Byte offset within the index buffer to pass to vkCmdBindIndexBuffer.
		// Zero for standalone Mesh objects; non-zero for MeshArena views.
		[[nodiscard]] VkDeviceSize GetIndexByteOffset() const
		{
			return m_indexByteOffset;
		}

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = nullptr;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = nullptr;
		std::uint32_t m_vertexCount = 0;
		VkBuffer m_indexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_indexAllocation = nullptr;
		std::uint32_t m_indexCount = 0;
		VkDeviceSize m_vertexByteOffset = 0;
		VkDeviceSize m_indexByteOffset = 0;
	};
} // namespace aether
