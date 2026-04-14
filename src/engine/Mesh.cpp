#include "Mesh.hpp"

#include <cstring>

#include "MeowExceptions.hpp"

namespace aether
{
	Mesh Mesh::Create(VkDevice device, VmaAllocator allocator, std::span<const Vertex> vertices)
	{
		Mesh mesh;
		mesh.m_device = device;
		mesh.m_allocator = allocator;
		mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = bufferSize;
		bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

		// Host-visible + mapped for simplicity. A staging-buffer upload path
		// can be layered on top later without changing the Mesh API.
		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
			| VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VmaAllocationInfo allocResult{};
		const VkResult result = vmaCreateBuffer(
			allocator, &bufferInfo, &allocInfo,
			&mesh.m_buffer, &mesh.m_allocation, &allocResult);

		if (result != VK_SUCCESS)
		{
			throw VulkanError("Failed to create vertex buffer for Mesh.");
		}

		std::memcpy(allocResult.pMappedData, vertices.data(), static_cast<std::size_t>(bufferSize));

		return mesh;
	}

	Mesh Mesh::Create(VkDevice device, VmaAllocator allocator, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		Mesh mesh = Create(device, allocator, vertices);

		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());
		const VkDeviceSize indexBufferSize = sizeof(std::uint32_t) * indices.size();

		VkBufferCreateInfo indexBufferInfo{};
		indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		indexBufferInfo.size = indexBufferSize;
		indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
			| VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VmaAllocationInfo allocResult{};
		const VkResult result = vmaCreateBuffer(
			allocator, &indexBufferInfo, &allocInfo,
			&mesh.m_indexBuffer, &mesh.m_indexAllocation, &allocResult);

		if (result != VK_SUCCESS)
		{
			throw VulkanError("Failed to create index buffer for Mesh.");
		}

		std::memcpy(allocResult.pMappedData, indices.data(), static_cast<std::size_t>(indexBufferSize));

		return mesh;
	}

	Mesh::~Mesh()
	{
		Destroy();
	}

	Mesh::Mesh(Mesh&& other) noexcept
		: m_device(other.m_device)
		, m_allocator(other.m_allocator)
		, m_buffer(other.m_buffer)
		, m_allocation(other.m_allocation)
		, m_vertexCount(other.m_vertexCount)
		, m_indexBuffer(other.m_indexBuffer)
		, m_indexAllocation(other.m_indexAllocation)
		, m_indexCount(other.m_indexCount)
	{
		other.m_device = VK_NULL_HANDLE;
		other.m_allocator = nullptr;
		other.m_buffer = VK_NULL_HANDLE;
		other.m_allocation = nullptr;
		other.m_vertexCount = 0;
		other.m_indexBuffer = VK_NULL_HANDLE;
		other.m_indexAllocation = nullptr;
		other.m_indexCount = 0;
	}

	Mesh& Mesh::operator=(Mesh&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();

			m_device = other.m_device;
			m_allocator = other.m_allocator;
			m_buffer = other.m_buffer;
			m_allocation = other.m_allocation;
			m_vertexCount = other.m_vertexCount;
			m_indexBuffer = other.m_indexBuffer;
			m_indexAllocation = other.m_indexAllocation;
			m_indexCount = other.m_indexCount;

			other.m_device = VK_NULL_HANDLE;
			other.m_allocator = nullptr;
			other.m_buffer = VK_NULL_HANDLE;
			other.m_allocation = nullptr;
			other.m_vertexCount = 0;
			other.m_indexBuffer = VK_NULL_HANDLE;
			other.m_indexAllocation = nullptr;
			other.m_indexCount = 0;
		}
		return *this;
	}

	void Mesh::Destroy()
	{
		if (m_indexBuffer != VK_NULL_HANDLE && m_allocator != nullptr)
		{
			vmaDestroyBuffer(m_allocator, m_indexBuffer, m_indexAllocation);
			m_indexBuffer = VK_NULL_HANDLE;
			m_indexAllocation = nullptr;
		}
		m_indexCount = 0;
		if (m_buffer != VK_NULL_HANDLE && m_allocator != nullptr)
		{
			vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
			m_buffer = VK_NULL_HANDLE;
			m_allocation = nullptr;
		}
		m_device = VK_NULL_HANDLE;
		m_allocator = nullptr;
		m_vertexCount = 0;
	}
}
