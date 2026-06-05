#include "mesh/Mesh.hpp"

#include <cstring>

#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "rendering/CommandRecorder.hpp"
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
			const VkResult allocResult = vkAllocateCommandBuffers(device, &allocInfo, &cmd);
			if (allocResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(allocResult), "Mesh: failed to allocate one-time command buffer"));
			}

			const VkCommandBufferBeginInfo beginInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			const VkResult beginResult = vkBeginCommandBuffer(cmd, &beginInfo);
			if (beginResult != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(beginResult), "Mesh: failed to begin one-time command buffer"));
			}
			return cmd;
		}

		// Submit and block until the queue is idle, then free the buffer.
		void EndAndSubmitOneTimeBuffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
		{
			const VkResult endResult = vkEndCommandBuffer(cmd);
			if (endResult != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(endResult), "Mesh: failed to end one-time command buffer"));
			}
			const VkCommandBufferSubmitInfo cbInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			        .commandBuffer = cmd,
			};
			const VkSubmitInfo2 submitInfo{
			        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			        .commandBufferInfoCount = 1,
			        .pCommandBufferInfos = &cbInfo,
			};
			const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(0, "Mesh: failed to create upload fence"));
			}
			const VkResult submitResult = vkQueueSubmit2(queue, 1, &submitInfo, fence);
			if (submitResult != VK_SUCCESS)
			{
				vkDestroyFence(device, fence, nullptr);
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "Mesh: failed to submit one-time command buffer"));
			}
			(void)vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
			vkDestroyFence(device, fence, nullptr);
			vkFreeCommandBuffers(device, pool, 1, &cmd);
		}

		// Upload arbitrary bytes to a new device-local buffer via a transient staging buffer.
		// Returns the device-local buffer and its BDA; the staging buffer is destroyed after submit.
		VkBuffer UploadToDeviceLocal(VkDevice device,
		        VmaAllocator allocator,
		        VkQueue queue,
		        VkCommandPool pool,
		        VkBufferUsageFlags usage,
		        const void* data,
		        VkDeviceSize size,
		        VmaAllocation& outAllocation,
		        VkDeviceAddress& outDeviceAddress,
		        const char* debugName = nullptr)
		{
			// Staging: mapped, host-sequential-write.
			AE_EXPECT_OR_THROW(staging, UniqueBuffer::CreateMapped(allocator, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
			std::memcpy(staging.GetAllocationInfo().pMappedData, data, static_cast<std::size_t>(size));
			AE_EXPECT_OR_THROW_VOID(staging.FlushMapped());

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
			const VkResult createResult = vmaCreateBuffer(allocator, &destInfo, &destAllocInfo, &dest, &outAllocation, nullptr);
			if (createResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(createResult), "Mesh: failed to create device-local vertex buffer"));
			}
			CommandRecorder::SetObjectName(device, reinterpret_cast<std::uint64_t>(dest), VK_OBJECT_TYPE_BUFFER, debugName ? debugName : "Mesh.Buffer");

			VkCommandBuffer cmd = BeginOneTimeBuffer(device, pool);
			const VkBufferCopy region{.size = size};
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

	Mesh Mesh::CreateView(
	        VkBuffer vertexBuffer, VkBuffer indexBuffer, std::uint32_t vertexCount, std::uint32_t indexCount, VkDeviceSize vertexByteOffset, VkDeviceSize indexByteOffset, VkDeviceAddress vertexDeviceAddress, VkDeviceAddress indexDeviceAddress)
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
		mesh.m_buffer = UploadToDeviceLocal(device, allocator, uploadQueue, uploadPool, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), size, mesh.m_allocation, mesh.m_vertexDeviceAddress, "Mesh.Vertex");
		return mesh;
	}

	Mesh Mesh::Create(VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius)
	{
		Mesh mesh = Create(device, allocator, uploadQueue, uploadPool, vertices);
		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());

		const VkDeviceSize size = sizeof(std::uint32_t) * indices.size();
		mesh.m_indexBuffer = UploadToDeviceLocal(device, allocator, uploadQueue, uploadPool, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), size, mesh.m_indexAllocation, mesh.m_indexDeviceAddress, "Mesh.Index");

		if (aabbMin) mesh.m_aabbMin = glm::vec3(aabbMin[0], aabbMin[1], aabbMin[2]);
		if (aabbMax) mesh.m_aabbMax = glm::vec3(aabbMax[0], aabbMax[1], aabbMax[2]);
		if (sphereCenter) mesh.m_boundingSphere = glm::vec4(sphereCenter[0], sphereCenter[1], sphereCenter[2], sphereRadius);

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
	        m_indexDeviceAddress(other.m_indexDeviceAddress),
	        m_aabbMin(other.m_aabbMin),
	        m_aabbMax(other.m_aabbMax),
	        m_boundingSphere(other.m_boundingSphere)
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
		other.m_aabbMin = glm::vec3(0.0f);
		other.m_aabbMax = glm::vec3(0.0f);
		other.m_boundingSphere = glm::vec4(0.0f);
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
			m_aabbMin = other.m_aabbMin;
			m_aabbMax = other.m_aabbMax;
			m_boundingSphere = other.m_boundingSphere;

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
			other.m_aabbMin = glm::vec3(0.0f);
			other.m_aabbMax = glm::vec3(0.0f);
			other.m_boundingSphere = glm::vec4(0.0f);
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
