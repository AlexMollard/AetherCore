#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include "gpu/GpuTypes.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/UploadContext.hpp"

namespace aether
{
	// never touches VMA or backend buffer types directly.
	class Mesh
	{
	public:
		struct Vertex
		{
			glm::vec3 position;
			glm::vec3 normal;
			glm::vec4 tangent;
			glm::vec2 uv;
			glm::vec2 uv2;
			glm::vec3 color;
			glm::uvec4 jointIndices{0u, 0u, 0u, 0u};
			glm::vec4 jointWeights{1.0f, 0.0f, 0.0f, 0.0f};
		};

		static_assert(sizeof(Vertex) == 100, "Mesh::Vertex layout changed - update shaders/include/MeshVertex.slangh.");
		static_assert(offsetof(Vertex, position) == 0);
		static_assert(offsetof(Vertex, normal) == 12);
		static_assert(offsetof(Vertex, tangent) == 24);
		static_assert(offsetof(Vertex, uv) == 40);
		static_assert(offsetof(Vertex, uv2) == 48);
		static_assert(offsetof(Vertex, color) == 56);
		static_assert(offsetof(Vertex, jointIndices) == 68);
		static_assert(offsetof(Vertex, jointWeights) == 84);

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

		static Mesh Create(gpu::UploadContext& uploadContext, std::span<const Vertex> vertices);
		static Mesh Create(gpu::UploadContext& uploadContext,
		        std::span<const Vertex> vertices,
		        std::span<const std::uint32_t> indices,
		        const float* aabbMin = nullptr,
		        const float* aabbMax = nullptr,
		        const float* sphereCenter = nullptr,
		        float sphereRadius = 0.0f);

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
			return m_buffer.IsValid();
		}

		[[nodiscard]] bool IsIndexed() const
		{
			return m_indexBuffer.IsValid();
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

		[[nodiscard]] gpu::DeviceSize GetVertexByteOffset() const
		{
			return m_vertexByteOffset;
		}

		[[nodiscard]] gpu::DeviceSize GetIndexByteOffset() const
		{
			return m_indexByteOffset;
		}

		[[nodiscard]] gpu::DeviceAddress GetVertexDeviceAddress() const
		{
			return m_vertexDeviceAddress;
		}

		[[nodiscard]] gpu::DeviceAddress GetIndexDeviceAddress() const
		{
			return m_indexDeviceAddress;
		}

		[[nodiscard]] glm::vec4 GetBoundingSphere() const
		{
			return m_boundingSphere;
		}

		[[nodiscard]] glm::vec3 GetAABBMin() const
		{
			return m_aabbMin;
		}

		[[nodiscard]] glm::vec3 GetAABBMax() const
		{
			return m_aabbMax;
		}

	private:
		bool m_ownsBuffers = false;
		gpu::BufferHandle m_buffer{};
		std::uint32_t m_vertexCount = 0;
		gpu::BufferHandle m_indexBuffer{};
		std::uint32_t m_indexCount = 0;
		gpu::DeviceSize m_vertexByteOffset = 0;
		gpu::DeviceSize m_indexByteOffset = 0;
		gpu::DeviceAddress m_vertexDeviceAddress = 0;
		gpu::DeviceAddress m_indexDeviceAddress = 0;

		glm::vec3 m_aabbMin{0.0f};
		glm::vec3 m_aabbMax{0.0f};
		glm::vec4 m_boundingSphere{0.0f, 0.0f, 0.0f, 0.0f};
		std::uint32_t m_aliveSentinel = kAliveSentinel;
		std::uint32_t m_generation = 0;
	};
} // namespace aether
