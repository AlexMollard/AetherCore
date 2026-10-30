#pragma once

#include <cstdint>
#include <vector>
#include "physics/PhysicsDebugShared.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether::gpu
{
	class CommandList;
}

namespace aether
{
	class World;

	class GpuDevice;


	class PhysicsDebugRenderer
	{
	public:
		PhysicsDebugRenderer() = default;
		~PhysicsDebugRenderer();

		PhysicsDebugRenderer(const PhysicsDebugRenderer&) = delete;
		PhysicsDebugRenderer& operator=(const PhysicsDebugRenderer&) = delete;
		PhysicsDebugRenderer(PhysicsDebugRenderer&&) noexcept;
		PhysicsDebugRenderer& operator=(PhysicsDebugRenderer&&) noexcept;

		void Init(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown();

		void SetEnabled(bool enabled)
		{
			m_enabled = enabled;
		}

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled;
		}

		void SetColorMode(PhysicsDebugColorMode mode)
		{
			m_colorMode = mode;
		}

		[[nodiscard]] PhysicsDebugColorMode GetColorMode() const
		{
			return m_colorMode;
		}

		// Producer-thread extract: walk the ECS colliders and append a wireframe
		void ExtractShapes(const World& world, std::vector<PhysicsDebugInstance>& out) const;

		void SetFramePhysicsShapes(const std::vector<PhysicsDebugInstance>* shapes)
		{
			m_frameShapes = shapes;
		}

		// by ExecuteRenderFrame, so the render thread branches on this per-frame
		void SetFrameDebugEnabled(bool enabled)
		{
			m_frameDebugEnabled = enabled;
		}

		// before RenderGraph::Execute runs. Lock-free: the channel transfer of the
		void SetFrameDebugVertices(const std::vector<DebugVertex>* vertices)
		{
			m_frameDebugVertices = vertices;
		}

		//   1. game-thread immediate-mode primitives from RenderFramePacket::debugVertices
		void RegisterPass(RenderGraph& graph, RGImage color = {}, RGImage depth = {}, gpu::Extent2D extent = {});

	private:
		void CreateWireframePipeline(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat);
		void CreateBoxGeometry();
		void CreateSphereGeometry();
		void CreateCapsuleGeometry();
		void CreateCylinderGeometry();

		void EnsureImmediateBufferCapacity(std::uint32_t vertexCount);
		void DestroyImmediateBuffer();

		void DrawImmediateDebugPrimitives(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr);
		void DrawPhysicsDebugShapes(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr) const;

		bool m_enabled = false;
		PhysicsDebugColorMode m_colorMode = PhysicsDebugColorMode::None;

		const std::vector<DebugVertex>* m_frameDebugVertices = nullptr;
		const std::vector<PhysicsDebugInstance>* m_frameShapes = nullptr;
		bool m_frameDebugEnabled = false;
		gpu::Format m_colorFormat = gpu::Format::Undefined;
		gpu::Format m_depthFormat = gpu::Format::Undefined;

		gpu::PipelineHandle m_pipelineHandle = {};

		// are positioned by a per-draw MVP push constant. Lifetime is owned by
		gpu::BufferHandle m_boxVertexHandle = {};
		std::uint32_t m_boxVertexCount = 0;

		gpu::BufferHandle m_sphereVertexHandle = {};
		std::uint32_t m_sphereVertexCount = 0;

		gpu::BufferHandle m_capsuleVertexHandle = {};
		std::uint32_t m_capsuleVertexCount = 0;

		gpu::BufferHandle m_cylinderVertexHandle = {};
		std::uint32_t m_cylinderVertexCount = 0;

		gpu::BufferHandle m_immediateVertexHandle = {};
		std::uint32_t m_immediateCapacity = 0;
	};
} // namespace aether
