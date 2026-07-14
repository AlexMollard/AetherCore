#pragma once

#include <cstdint>
#include "gpu/GpuTypes.hpp"
#include "gpu/GpuHandles.hpp"

#include "vulkan/GpuHeap.hpp"
#include "mesh/Mesh.hpp"

namespace aether
{
	class VulkanContext;

	class MeshArena
	{
	public:
		MeshArena() = default;
		~MeshArena() = default;

		struct Desc
		{
			gpu::DeviceSize vertexCapacityBytes = 256ull * 1024 * 1024;
			gpu::DeviceSize indexCapacityBytes = 128ull * 1024 * 1024;
		};

		struct Alloc
		{
			gpu::DeviceSize vertexByteOffset = 0;
			std::uint32_t vertexCount = 0;
			gpu::DeviceSize indexByteOffset = 0;
			std::uint32_t indexCount = 0;

			gpu::DeviceSize vertexByteSize = 0;
			gpu::DeviceSize indexByteSize = 0;

			[[nodiscard]] bool IsValid() const
			{
				return vertexByteSize > 0;
			}
		};

		void Initialize(const VulkanContext& ctx, const Desc& desc);
		void Shutdown();

		MeshArena(const MeshArena&) = delete;
		MeshArena& operator=(const MeshArena&) = delete;
		MeshArena(MeshArena&&) = delete;
		MeshArena& operator=(MeshArena&&) = delete;

		[[nodiscard]] Alloc Allocate(gpu::DeviceSize vertexBytes, std::uint32_t vertexCount, gpu::DeviceSize indexBytes, std::uint32_t indexCount);

		void Free(Alloc& alloc);

		[[nodiscard]] Mesh CreateView(const Alloc& alloc) const;

		[[nodiscard]] gpu::BufferHandle GetVertexBuffer() const
		{
			return m_vertexHandle;
		}

		[[nodiscard]] gpu::BufferHandle GetIndexBuffer() const
		{
			return m_indexHandle;
		}

		[[nodiscard]] gpu::Buffer GetVertexBufferRaw() const
		{
			return m_vertexHeap.GetBuffer();
		}

		[[nodiscard]] gpu::Buffer GetIndexBufferRaw() const
		{
			return m_indexHeap.GetBuffer();
		}

		[[nodiscard]] gpu::DeviceAddress GetVertexDeviceAddress() const
		{
			return m_vertexHeap.GetBaseAddress();
		}

		[[nodiscard]] gpu::DeviceAddress GetIndexDeviceAddress() const
		{
			return m_indexHeap.GetBaseAddress();
		}

		void SetMemoryTracker(class GpuMemoryTracker* tracker)
		{
			m_vertexHeap.SetMemoryTracker(tracker);
			m_indexHeap.SetMemoryTracker(tracker);
		}

	private:
		GpuHeap m_vertexHeap;
		GpuHeap m_indexHeap;
		gpu::BufferHandle m_vertexHandle{};
		gpu::BufferHandle m_indexHandle{};
	};
} // namespace aether
