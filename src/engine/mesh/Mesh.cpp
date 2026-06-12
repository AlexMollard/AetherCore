#include "mesh/Mesh.hpp"

#include <cstring>

#include "gpu/ResourceRegistry.hpp"
#include "gpu/UploadContext.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	Mesh Mesh::CreateView(gpu::BufferHandle vertexBuffer,
	        gpu::BufferHandle indexBuffer,
	        std::uint32_t vertexCount,
	        std::uint32_t indexCount,
	        gpu::DeviceSize vertexByteOffset,
	        gpu::DeviceSize indexByteOffset,
	        gpu::DeviceAddress vertexDeviceAddress,
	        gpu::DeviceAddress indexDeviceAddress)
	{
		Mesh mesh;
		mesh.m_aliveSentinel = Mesh::kAliveSentinel;
		// m_allocator intentionally left null - Destroy() uses ResourceRegistry.
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

	Mesh Mesh::Create(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices)
	{
		AE_PROFILE_ZONE_N("Mesh::Upload");
		const gpu::DeviceSize size = sizeof(Vertex) * vertices.size();

		// Staging mapped buffer
		gpu::MappedBufferDesc stagingDesc{};
		stagingDesc.size = size;
		stagingDesc.usage = gpu::BufferUsage::TransferSrc;
		stagingDesc.debugName = "Mesh.Staging";
		gpu::BufferHandle stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer(stagingDesc);
		if (!stagingHandle.IsValid())
		{
			return {};
		}

		gpu::MappedBufferView stagingView = gpu::ResourceRegistry::ResolveMappedBuffer(stagingHandle);
		if (stagingView.mappedPtr == nullptr)
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return {};
		}

		std::memcpy(stagingView.mappedPtr, vertices.data(), static_cast<std::size_t>(size));
		gpu::ResourceRegistry::FlushMappedBuffer(stagingHandle, 0, size);

		// Device-local buffer with BDA
		gpu::BufferDesc bufferDesc{};
		bufferDesc.size = size;
		bufferDesc.usage = gpu::BufferUsage::Vertex | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		bufferDesc.debugName = "Mesh.Vertex";
		gpu::BufferHandle bufferHandle = gpu::ResourceRegistry::CreateBuffer(bufferDesc);
		if (!bufferHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return {};
		}

		// Copy staging → device-local
		uploadContext.CopyBuffer(stagingHandle, bufferHandle, size);

		// Free staging
		gpu::ResourceRegistry::Destroy(stagingHandle);

		// Retrieve device address
		gpu::ResourceRegistry::ResolvedBuffer resolved = gpu::ResourceRegistry::ResolveBuffer(bufferHandle);

		Mesh mesh;
		mesh.m_aliveSentinel = Mesh::kAliveSentinel;
		mesh.m_buffer = bufferHandle;
		mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());
		mesh.m_vertexDeviceAddress = resolved.deviceAddress;
		return mesh;
	}

	Mesh Mesh::Create(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius)
	{
		Mesh mesh = Create(uploadContext, vertices);
		mesh.m_indexCount = static_cast<std::uint32_t>(indices.size());

		const gpu::DeviceSize indexSize = sizeof(std::uint32_t) * indices.size();

		// Staging mapped buffer for indices
		gpu::MappedBufferDesc stagingDesc{};
		stagingDesc.size = indexSize;
		stagingDesc.usage = gpu::BufferUsage::TransferSrc;
		stagingDesc.debugName = "Mesh.IndexStaging";
		gpu::BufferHandle stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer(stagingDesc);
		if (!stagingHandle.IsValid())
		{
			return mesh;
		}

		gpu::MappedBufferView stagingView = gpu::ResourceRegistry::ResolveMappedBuffer(stagingHandle);
		if (stagingView.mappedPtr == nullptr)
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return mesh;
		}

		std::memcpy(stagingView.mappedPtr, indices.data(), static_cast<std::size_t>(indexSize));
		gpu::ResourceRegistry::FlushMappedBuffer(stagingHandle, 0, indexSize);

		gpu::BufferDesc bufferDesc{};
		bufferDesc.size = indexSize;
		bufferDesc.usage = gpu::BufferUsage::Index | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		bufferDesc.debugName = "Mesh.Index";
		gpu::BufferHandle bufferHandle = gpu::ResourceRegistry::CreateBuffer(bufferDesc);
		if (!bufferHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(stagingHandle);
			return mesh;
		}

		uploadContext.CopyBuffer(stagingHandle, bufferHandle, indexSize);
		gpu::ResourceRegistry::Destroy(stagingHandle);

		gpu::ResourceRegistry::ResolvedBuffer resolved = gpu::ResourceRegistry::ResolveBuffer(bufferHandle);

		mesh.m_indexBuffer = bufferHandle;
		mesh.m_indexDeviceAddress = resolved.deviceAddress;

		if (aabbMin)
		{
			mesh.m_aabbMin = glm::vec3(aabbMin[0], aabbMin[1], aabbMin[2]);
		}
		if (aabbMax)
		{
			mesh.m_aabbMax = glm::vec3(aabbMax[0], aabbMax[1], aabbMax[2]);
		}
		if (sphereCenter)
		{
			mesh.m_boundingSphere = glm::vec4(sphereCenter[0], sphereCenter[1], sphereCenter[2], sphereRadius);
		}

		return mesh;
	}

	Mesh::~Mesh()
	{
		Destroy();
	}

	Mesh::Mesh(Mesh&& other) noexcept
	      : m_device(other.m_device),
	        m_allocator(other.m_allocator),
	        m_buffer(std::move(other.m_buffer)),
	        m_vertexCount(other.m_vertexCount),
	        m_indexBuffer(std::move(other.m_indexBuffer)),
	        m_indexCount(other.m_indexCount),
	        m_vertexByteOffset(other.m_vertexByteOffset),
	        m_indexByteOffset(other.m_indexByteOffset),
	        m_vertexDeviceAddress(other.m_vertexDeviceAddress),
	        m_indexDeviceAddress(other.m_indexDeviceAddress),
	        m_aabbMin(other.m_aabbMin),
	        m_aabbMax(other.m_aabbMax),
	        m_boundingSphere(other.m_boundingSphere),
	        m_aliveSentinel(other.m_aliveSentinel),
	        m_generation(other.m_generation)
	{
		other.m_device = nullptr;
		other.m_allocator = nullptr;
		other.m_buffer = {};
		other.m_vertexCount = 0;
		other.m_indexBuffer = {};
		other.m_indexCount = 0;
		other.m_vertexByteOffset = 0;
		other.m_indexByteOffset = 0;
		other.m_vertexDeviceAddress = 0;
		other.m_indexDeviceAddress = 0;
		other.m_aabbMin = glm::vec3(0.0f);
		other.m_aabbMax = glm::vec3(0.0f);
		other.m_boundingSphere = glm::vec4(0.0f);
		other.m_aliveSentinel = 0;
		other.m_generation = 0;
	}

	Mesh& Mesh::operator=(Mesh&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();

			m_device = other.m_device;
			m_allocator = other.m_allocator;
			m_buffer = std::move(other.m_buffer);
			m_vertexCount = other.m_vertexCount;
			m_indexBuffer = std::move(other.m_indexBuffer);
			m_indexCount = other.m_indexCount;
			m_vertexByteOffset = other.m_vertexByteOffset;
			m_indexByteOffset = other.m_indexByteOffset;
			m_vertexDeviceAddress = other.m_vertexDeviceAddress;
			m_indexDeviceAddress = other.m_indexDeviceAddress;
			m_aabbMin = other.m_aabbMin;
			m_aabbMax = other.m_aabbMax;
			m_boundingSphere = other.m_boundingSphere;
			m_aliveSentinel = other.m_aliveSentinel;
			m_generation = other.m_generation;

			other.m_device = nullptr;
			other.m_allocator = nullptr;
			other.m_buffer = {};
			other.m_vertexCount = 0;
			other.m_indexBuffer = {};
			other.m_indexCount = 0;
			other.m_vertexByteOffset = 0;
			other.m_indexByteOffset = 0;
			other.m_vertexDeviceAddress = 0;
			other.m_indexDeviceAddress = 0;
			other.m_aabbMin = glm::vec3(0.0f);
			other.m_aabbMax = glm::vec3(0.0f);
			other.m_boundingSphere = glm::vec4(0.0f);
			other.m_aliveSentinel = 0;
			other.m_generation = 0;
		}
		return *this;
	}

	void Mesh::Destroy()
	{
		++m_generation;
		if (m_indexBuffer.IsValid() && m_allocator != nullptr)
		{
			gpu::ResourceRegistry::Destroy(m_indexBuffer);
			m_indexBuffer = {};
		}
		m_indexCount = 0;
		if (m_buffer.IsValid() && m_allocator != nullptr)
		{
			gpu::ResourceRegistry::Destroy(m_buffer);
			m_buffer = {};
		}
		m_allocator = nullptr;
		m_vertexCount = 0;
		// device intentionally left valid; the engine may still query it.
	}
} // namespace aether
