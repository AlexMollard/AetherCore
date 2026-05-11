#include "mesh/Mesh.hpp"

#include <cstring>

#include "utils/AetherExceptions.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	namespace
	{
		// Allocate + begin a one-time command buffer from the given pool.
		VkCommandBuffer BeginOneTimeBuffer(VkDevice device, VkCommandPool pool)
		{
			const VkCommandBufferAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			vkAllocateCommandBuffers(device, &allocInfo, &cmd);

			const VkCommandBufferBeginInfo beginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			vkBeginCommandBuffer(cmd, &beginInfo);
			return cmd;
		}

		// Submit and block until the queue is idle, then free the buffer.
		void EndAndSubmitOneTimeBuffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
		{
			vkEndCommandBuffer(cmd);
			const VkSubmitInfo submitInfo{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &cmd,
			};
			vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);
			vkFreeCommandBuffers(device, pool, 1, &cmd);
		}

		// Upload arbitrary bytes to a new device-local buffer via a transient staging buffer.
		// Returns the device-local buffer and its BDA; the staging buffer is destroyed after submit.
		VkBuffer UploadToDeviceLocal(VkDevice device, VmaAllocator allocator, VkQueue queue, VkCommandPool pool, VkBufferUsageFlags usage, const void* data, VkDeviceSize size, VmaAllocation& outAllocation, VkDeviceAddress& outDeviceAddress)
		{
			// Staging: mapped, host-sequential-write.
			UniqueBuffer staging = UniqueBuffer::CreateMapped(allocator, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
			std::memcpy(staging.GetAllocationInfo().pMappedData, data, static_cast<std::size_t>(size));
			vmaFlushAllocation(allocator, staging.GetAllocation(), 0, VK_WHOLE_SIZE);

			// Destination: device-local with shader device address support.
			const VkBufferCreateInfo destInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = size,
				.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			};
			const VmaAllocationCreateInfo destAllocInfo{
				.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
			};
			VkBuffer dest = VK_NULL_HANDLE;
			vmaCreateBuffer(allocator, &destInfo, &destAllocInfo, &dest, &outAllocation, nullptr);

			VkCommandBuffer cmd = BeginOneTimeBuffer(device, pool);
			const VkBufferCopy region{ .size = size };
			vkCmdCopyBuffer(cmd, staging.Get(), dest, 1, &region);
			EndAndSubmitOneTimeBuffer(device, pool, queue, cmd);

			const VkBufferDeviceAddressInfo addrInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = dest,
			};
			outDeviceAddress = vkGetBufferDeviceAddress(device, &addrInfo);

			return dest;
		}
	} // namespace

	Mesh Mesh::CreateView(VkBuffer vertexBuffer, VkBuffer indexBuffer, std::uint32_t vertexCount, std::uint32_t indexCount, VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset, VkDeviceAddress vertexDeviceAddress, VkDeviceAddress indexDeviceAddress)
	{
		Mesh mesh;
		// m_allocator intentionally left null - Destroy() skips vmaDestroyBuffer for views.
		mesh.m_buffer = vertexBuffer;
		mesh.m_vertexCount = vertexCount;
		mesh.m_vertexByteOffset = vertexByteOffset;
		mesh.m_vertexDeviceAddress = vertexDeviceAddress;
		mesh.m_indexBuffer = indexBuffer;
		mesh.m_indexCount = indexCount;
		mesh.m_indexByteOffset = indexByteOffset;
		mesh.m_indexDeviceAddress = indexDeviceAddress;
		return mesh;
	}

	Mesh Mesh::Create(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, std::span<const Vertex> vertices)
	{
		AE_PROFILE_ZONE_N("Mesh::Upload");
		Mesh mesh;
		mesh.m_device = device;
		mesh.m_allocator = allocator;
		mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkDeviceSize size = sizeof(Vertex) * vertices.size();
		mesh.m_buffer = UploadToDeviceLocal(device, allocator, uploadQueue, uploadPool, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), size, mesh.m_allocation, mesh.m_vertexDeviceAddress);
		return mesh;
	}

	Mesh Mesh::Create(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		Mesh mesh = Create(device, allocator, uploadQueue, uploadPool, vertices);
		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());

		const VkDeviceSize size = sizeof(std::uint32_t) * indices.size();
		mesh.m_indexBuffer = UploadToDeviceLocal(device, allocator, uploadQueue, uploadPool, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), size, mesh.m_indexAllocation, mesh.m_indexDeviceAddress);
		return mesh;
	}

	Mesh::~Mesh()
	{
		Destroy();
	}

	Mesh::Mesh(Mesh&& other) noexcept
	      : m_device(other.m_device),
	        m_allocator(other.m_allocator),
	        m_buffer(other.m_buffer),
	        m_allocation(other.m_allocation),
	        m_vertexCount(other.m_vertexCount),
	        m_indexBuffer(other.m_indexBuffer),
	        m_indexAllocation(other.m_indexAllocation),
	        m_indexCount(other.m_indexCount),
	        m_vertexByteOffset(other.m_vertexByteOffset),
	        m_indexByteOffset(other.m_indexByteOffset),
	        m_vertexDeviceAddress(other.m_vertexDeviceAddress),
	        m_indexDeviceAddress(other.m_indexDeviceAddress)
	{
		other.m_device = VK_NULL_HANDLE;
		other.m_allocator = nullptr;
		other.m_buffer = VK_NULL_HANDLE;
		other.m_allocation = nullptr;
		other.m_vertexCount = 0;
		other.m_indexBuffer = VK_NULL_HANDLE;
		other.m_indexAllocation = nullptr;
		other.m_indexCount = 0;
		other.m_vertexByteOffset = 0;
		other.m_indexByteOffset = 0;
		other.m_vertexDeviceAddress = 0;
		other.m_indexDeviceAddress = 0;
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
			m_vertexByteOffset = other.m_vertexByteOffset;
			m_indexByteOffset = other.m_indexByteOffset;
			m_vertexDeviceAddress = other.m_vertexDeviceAddress;
			m_indexDeviceAddress = other.m_indexDeviceAddress;

			other.m_device = VK_NULL_HANDLE;
			other.m_allocator = nullptr;
			other.m_buffer = VK_NULL_HANDLE;
			other.m_allocation = nullptr;
			other.m_vertexCount = 0;
			other.m_indexBuffer = VK_NULL_HANDLE;
			other.m_indexAllocation = nullptr;
			other.m_indexCount = 0;
			other.m_vertexByteOffset = 0;
			other.m_indexByteOffset = 0;
			other.m_vertexDeviceAddress = 0;
			other.m_indexDeviceAddress = 0;
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
		m_vertexByteOffset = 0;
		m_indexByteOffset = 0;
	}
} // namespace aether
