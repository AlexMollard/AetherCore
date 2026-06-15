#include "mesh/MeshArena.hpp"

#include "vulkan/VulkanContext.hpp"
#include "gpu/GpuHandles.hpp"

namespace aether
{
	void MeshArena::Initialize(const VulkanContext& ctx, const Desc& desc)
	{
		m_vertexHeap.Initialize(ctx, {.capacityBytes = desc.vertexCapacityBytes});
		m_indexHeap.Initialize(ctx,
		        {
		                .capacityBytes = desc.indexCapacityBytes,
		                .additionalUsage = gpu::BufferUsage::Index,
		        });
		m_vertexHandle = gpu::BufferHandle::Make(0, 1);
		m_indexHandle = gpu::BufferHandle::Make(1, 1);
	}

	void MeshArena::Shutdown()
	{
		m_vertexHeap.Shutdown();
		m_indexHeap.Shutdown();
	}

	MeshArena::Alloc MeshArena::Allocate(gpu::DeviceSize vertexBytes, std::uint32_t vertexCount, gpu::DeviceSize indexBytes, std::uint32_t indexCount)
	{
		GpuSpan<std::byte> vs = m_vertexHeap.Alloc<std::byte>(static_cast<std::uint32_t>(vertexBytes));
		if (!vs.IsValid())
		{
			return {};
		}

		GpuSpan<std::byte> is = m_indexHeap.Alloc<std::byte>(static_cast<std::uint32_t>(indexBytes));
		if (!is.IsValid())
		{
			m_vertexHeap.Free(vs);
			return {};
		}

		return Alloc{
		        m_vertexHeap.GetOffset(vs),
		        vertexCount,
		        m_indexHeap.GetOffset(is),
		        indexCount,
		        vertexBytes,
		        indexBytes,
		};
	}

	void MeshArena::Free(Alloc& alloc)
	{
		if (!alloc.IsValid())
		{
			return;
		}

		GpuSpan<std::byte> vs;
		vs.address = m_vertexHeap.GetBaseAddress() + alloc.vertexByteOffset;
		vs.count = static_cast<std::uint32_t>(alloc.vertexByteSize);

		GpuSpan<std::byte> is;
		is.address = m_indexHeap.GetBaseAddress() + alloc.indexByteOffset;
		is.count = static_cast<std::uint32_t>(alloc.indexByteSize);

		m_vertexHeap.Free(vs);
		m_indexHeap.Free(is);
		alloc = {};
	}

	Mesh MeshArena::CreateView(const Alloc& alloc) const
	{
		return Mesh::CreateView(
		        m_vertexHandle, m_indexHandle, alloc.vertexCount, alloc.indexCount, alloc.vertexByteOffset, alloc.indexByteOffset, m_vertexHeap.GetBaseAddress() + alloc.vertexByteOffset, m_indexHeap.GetBaseAddress() + alloc.indexByteOffset);
	}
} // namespace aether
