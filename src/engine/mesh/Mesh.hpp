#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include "gpu/GpuTypes.hpp"
#include "gpu/GpuHandles.hpp"

namespace aether
{
	// GPU-resident vertex buffer. Created via AetherCore::CreateMesh - app code
	// never touches VMA or VkBuffer directly.
	class Mesh
	{
	public:
		struct Vertex
		{
			glm::vec3 position;
			glm::vec3 normal;
			glm::vec4 tangent; // xyz = tangent direction, w = bitangent sign
			glm::vec2 uv;
			glm::vec2 uv2; // secondary UV (lightmaps, detail maps)
			glm::vec3 color;
			glm::uvec4 jointIndices{0u, 0u, 0u, 0u};
			glm::vec4 jointWeights{1.0f, 0.0f, 0.0f, 0.0f};
		};

		Mesh() = default;
		~Mesh();

		Mesh(const Mesh&) = delete;
		Mesh& operator=(const Mesh&) = delete;

		Mesh(Mesh&&) noexcept;
		Mesh& operator=(Mesh&&) noexcept;

		static constexpr std::uint32_t kAliveSentinel = 0xDEADBEEFu;

		[[nodiscard]] bool IsAlive() const
		{
			return m_aliveSentinel == kAliveSentinel;
		}

		[[nodiscard]] std::uint32_t GetGeneration() const
		{
			return m_generation;
		}

		// Engine-internal factory used by AetherCore::CreateMesh.
		// Uploads via a staging buffer to device-local memory; call after the upload
		// pool is created (blocks until the queue is idle).
		static Mesh Create(gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, std::span<const Vertex> vertices);
		static Mesh Create(gpu::Device device,
		        gpu::Allocator allocator,
		        gpu::Queue uploadQueue,
		        gpu::CommandPool uploadPool,
		        std::span<const Vertex> vertices,
		        std::span<const std::uint32_t> indices,
		        const float* aabbMin = nullptr,
		        const float* aabbMax = nullptr,
		        const float* sphereCenter = nullptr,
		        float sphereRadius = 0.0f);

		// Create a non-owning view into an externally managed buffer (e.g. MeshArena / GpuHeap).
		// The returned Mesh does NOT free the backing memory when destroyed (allocator is null).
		// Pass device addresses so the vertex shader can fetch vertices via BDA.
		static Mesh CreateView(gpu::BufferHandle vertexBuffer,
		        gpu::BufferHandle indexBuffer,
		        std::uint32_t vertexCount,
		        std::uint32_t indexCount,
		        gpu::DeviceSize vertexByteOffset = 0,
		        gpu::DeviceSize indexByteOffset = 0,
		        gpu::DeviceAddress vertexDeviceAddress = 0,
		        gpu::DeviceAddress indexDeviceAddress = 0);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_buffer != VK_NULL_HANDLE;
		}

		[[nodiscard]] bool IsIndexed() const
		{
			return m_indexBuffer != VK_NULL_HANDLE;
		}

		[[nodiscard]] gpu::BufferHandle GetBuffer() const
		{
			return m_buffer;
		}

		[[nodiscard]] gpu::BufferHandle GetIndexBuffer() const
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

		// Byte offset within the vertex buffer (for MeshArena views).
		[[nodiscard]] gpu::DeviceSize GetVertexByteOffset() const
		{
			return m_vertexByteOffset;
		}

		// Byte offset within the index buffer (for MeshArena / GpuHeap views).
		[[nodiscard]] gpu::DeviceSize GetIndexByteOffset() const
		{
			return m_indexByteOffset;
		}

		// Buffer device address of the vertex data - pass directly to DrawInstanceData.vertexBufferAddr.
		[[nodiscard]] gpu::DeviceAddress GetVertexDeviceAddress() const
		{
			return m_vertexDeviceAddress;
		}

		// Buffer device address of the index data (for future BDA index fetch if needed).
		[[nodiscard]] gpu::DeviceAddress GetIndexDeviceAddress() const
		{
			return m_indexDeviceAddress;
		}

		// Local-space bounding sphere (xyz=center, w=radius).
		[[nodiscard]] glm::vec4 GetBoundingSphere() const
		{
			return m_boundingSphere;
		}

		// Local-space AABB (min/max).
		[[nodiscard]] glm::vec3 GetAABBMin() const
		{
			return m_aabbMin;
		}

		[[nodiscard]] glm::vec3 GetAABBMax() const
		{
			return m_aabbMax;
		}

	private:
		gpu::Device m_device = nullptr;
		VmaAllocator m_allocator = nullptr;
		gpu::BufferHandle m_buffer = VK_NULL_HANDLE;
		VmaAllocation m_allocation = nullptr;
		std::uint32_t m_vertexCount = 0;
		gpu::BufferHandle m_indexBuffer = VK_NULL_HANDLE;
		VmaAllocation m_indexAllocation = nullptr;
		std::uint32_t m_indexCount = 0;
		gpu::DeviceSize m_vertexByteOffset = 0;
		gpu::DeviceSize m_indexByteOffset = 0;
		gpu::DeviceAddress m_vertexDeviceAddress = 0;
		gpu::DeviceAddress m_indexDeviceAddress = 0;

		// Local-space bounding volume (from mesh header).
		glm::vec3 m_aabbMin{0.0f};
		glm::vec3 m_aabbMax{0.0f};
		glm::vec4 m_boundingSphere{0.0f, 0.0f, 0.0f, 0.0f}; // xyz=center, w=radius
		std::uint32_t m_aliveSentinel = kAliveSentinel;
		std::uint32_t m_generation = 0;
	};
} // namespace aether
