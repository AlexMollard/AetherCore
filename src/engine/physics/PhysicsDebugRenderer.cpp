#include "gpu/BindlessManager.hpp"
#include "physics/PhysicsDebugRenderer.hpp"

#include <cstddef>
#include <cstring>

#include "rendering/RenderGraph.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Push-constant layout: { uint64 frameAddr, vec4 tint, mat4 model }
		struct DebugPc
		{
			std::uint64_t frameAddr;
			glm::vec4 tintColor;
			glm::mat4 model;
		};

		static_assert(sizeof(DebugPc) == 88);


		gpu::BufferHandle CreateStaticVertexBuffer(std::span<const DebugVertex> vertices, const char* debugName)
		{
			const gpu::MappedBufferDesc desc{
			        .size = static_cast<gpu::DeviceSize>(vertices.size() * sizeof(DebugVertex)),
			        .usage = gpu::BufferUsage::Vertex,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = debugName,
			};
			gpu::BufferHandle handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!handle.IsValid())
			{
				return handle;
			}
			const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(handle);
			std::memcpy(mapped.mappedPtr, vertices.data(), vertices.size() * sizeof(DebugVertex));
			return handle;
		}
	} // namespace


	PhysicsDebugRenderer::~PhysicsDebugRenderer()
	{
		Shutdown();
	}

	PhysicsDebugRenderer::PhysicsDebugRenderer(PhysicsDebugRenderer&& rhs) noexcept
	      : m_enabled(rhs.m_enabled),
	        m_colorMode(rhs.m_colorMode),
	        m_frameDebugVertices(rhs.m_frameDebugVertices),
	        m_frameShapes(rhs.m_frameShapes),
	        m_frameDebugEnabled(rhs.m_frameDebugEnabled),
	        m_colorFormat(rhs.m_colorFormat),
	        m_depthFormat(rhs.m_depthFormat),
	        m_pipelineHandle(rhs.m_pipelineHandle),
	        m_boxVertexHandle(rhs.m_boxVertexHandle),
	        m_boxVertexCount(rhs.m_boxVertexCount),
	        m_sphereVertexHandle(rhs.m_sphereVertexHandle),
	        m_sphereVertexCount(rhs.m_sphereVertexCount),
	        m_capsuleVertexHandle(rhs.m_capsuleVertexHandle),
	        m_capsuleVertexCount(rhs.m_capsuleVertexCount),
	        m_cylinderVertexHandle(rhs.m_cylinderVertexHandle),
	        m_cylinderVertexCount(rhs.m_cylinderVertexCount),
	        m_immediateVertexHandle(rhs.m_immediateVertexHandle),
	        m_immediateCapacity(rhs.m_immediateCapacity)
	{
		rhs.m_pipelineHandle = {};
		rhs.m_boxVertexHandle = {};
		rhs.m_sphereVertexHandle = {};
		rhs.m_capsuleVertexHandle = {};
		rhs.m_cylinderVertexHandle = {};
		rhs.m_immediateVertexHandle = {};
		rhs.m_immediateCapacity = 0;
	}

	PhysicsDebugRenderer& PhysicsDebugRenderer::operator=(PhysicsDebugRenderer&& rhs) noexcept
	{
		if (this != &rhs)
		{
			Shutdown();
			m_enabled = rhs.m_enabled;
			m_colorMode = rhs.m_colorMode;
			m_frameDebugVertices = rhs.m_frameDebugVertices;
			m_frameShapes = rhs.m_frameShapes;
			m_frameDebugEnabled = rhs.m_frameDebugEnabled;
			m_colorFormat = rhs.m_colorFormat;
			m_depthFormat = rhs.m_depthFormat;
			m_pipelineHandle = rhs.m_pipelineHandle;
			m_boxVertexHandle = rhs.m_boxVertexHandle;
			m_boxVertexCount = rhs.m_boxVertexCount;
			m_sphereVertexHandle = rhs.m_sphereVertexHandle;
			m_sphereVertexCount = rhs.m_sphereVertexCount;
			m_capsuleVertexHandle = rhs.m_capsuleVertexHandle;
			m_capsuleVertexCount = rhs.m_capsuleVertexCount;
			m_cylinderVertexHandle = rhs.m_cylinderVertexHandle;
			m_cylinderVertexCount = rhs.m_cylinderVertexCount;
			m_immediateVertexHandle = rhs.m_immediateVertexHandle;
			m_immediateCapacity = rhs.m_immediateCapacity;

			rhs.m_pipelineHandle = {};
			rhs.m_boxVertexHandle = {};
			rhs.m_sphereVertexHandle = {};
			rhs.m_capsuleVertexHandle = {};
			rhs.m_cylinderVertexHandle = {};
			rhs.m_immediateVertexHandle = {};
			rhs.m_immediateCapacity = 0;
		}
		return *this;
	}

	void PhysicsDebugRenderer::Init(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		m_colorFormat = colorFormat;
		m_depthFormat = depthFormat;
		CreateWireframePipeline(gpu, colorFormat, depthFormat);
		CreateBoxGeometry();
		CreateSphereGeometry();
		CreateCapsuleGeometry();
		CreateCylinderGeometry();
		m_immediateCapacity = 0;
		m_enabled = true;
		m_colorMode = PhysicsDebugColorMode::ByMotionType;
	}

	void PhysicsDebugRenderer::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_boxVertexHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_boxVertexHandle);
			m_boxVertexHandle = {};
		}
		if (m_sphereVertexHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_sphereVertexHandle);
			m_sphereVertexHandle = {};
		}
		if (m_capsuleVertexHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_capsuleVertexHandle);
			m_capsuleVertexHandle = {};
		}
		if (m_cylinderVertexHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_cylinderVertexHandle);
			m_cylinderVertexHandle = {};
		}
		DestroyImmediateBuffer();
		if (m_pipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_pipelineHandle);
			m_pipelineHandle = {};
		}
		m_immediateCapacity = 0;
	}

	void PhysicsDebugRenderer::CreateWireframePipeline(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat)
	{
		constexpr gpu::VertexInputBinding kBindings[]{{
		        .binding = 0,
		        .stride = sizeof(DebugVertex),
		}};

		constexpr gpu::VertexInputAttribute kAttribs[]{
		        gpu::VertexInputAttribute{
		                .location = 0,
		                .binding = 0,
		                .format = gpu::Format::R32G32B32Sfloat,
		                .offset = static_cast<std::uint32_t>(offsetof(DebugVertex, position)),
		        },
		        gpu::VertexInputAttribute{
		                .location = 1,
		                .binding = 0,
		                .format = gpu::Format::R32G32B32A32Sfloat,
		                .offset = static_cast<std::uint32_t>(offsetof(DebugVertex, color)),
		        },
		};

		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://debug_vert.spv",
		        .fragmentVfsPath = "shaders://debug_frag.spv",
		        .vertexEntry = "main",
		        .fragmentEntry = "main",
		        .colorFormat = colorFormat,
		        .depthFormat = depthFormat,
		        .depthTestEnable = true,
		        .depthWriteEnable = false,
		        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		        .blendEnable = true,
		        .topology = gpu::PrimitiveTopology::LineList,
		        .polygonMode = gpu::PolygonMode::Line,
		        .vertexBindings = kBindings,
		        .vertexAttributes = kAttribs,
		        .lineWidthDynamic = true,
		        .debugName = "PhysicsDebug.Pipeline",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};

		m_pipelineHandle = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
	}

	void PhysicsDebugRenderer::CreateBoxGeometry()
	{
		const auto& vertices = GetPhysicsDebugGeometry(PhysicsShapeType::Box);
		m_boxVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_boxVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.BoxGeometry");
	}

	void PhysicsDebugRenderer::CreateSphereGeometry()
	{
		const auto& vertices = GetPhysicsDebugGeometry(PhysicsShapeType::Sphere);
		m_sphereVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_sphereVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.SphereGeometry");
	}

	void PhysicsDebugRenderer::CreateCapsuleGeometry()
	{
		const auto& vertices = GetPhysicsDebugGeometry(PhysicsShapeType::Capsule);
		m_capsuleVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_capsuleVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.CapsuleGeometry");
	}

	void PhysicsDebugRenderer::CreateCylinderGeometry()
	{
		const auto& vertices = GetPhysicsDebugGeometry(PhysicsShapeType::Cylinder);
		m_cylinderVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_cylinderVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.CylinderGeometry");
	}

	void PhysicsDebugRenderer::EnsureImmediateBufferCapacity(std::uint32_t vertexCount)
	{
		constexpr std::uint32_t kInitialImmediateCapacity = 4096;

		if (m_immediateCapacity >= vertexCount && m_immediateVertexHandle.IsValid())
		{
			return;
		}

		std::uint32_t newCapacity = m_immediateCapacity == 0 ? kInitialImmediateCapacity : m_immediateCapacity;
		while (newCapacity < vertexCount)
		{
			newCapacity *= 2;
		}

		DestroyImmediateBuffer();

		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(newCapacity) * sizeof(DebugVertex),
		        .usage = gpu::BufferUsage::Vertex,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "PhysicsDebug.Immediate",
		};
		m_immediateVertexHandle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_immediateVertexHandle.IsValid())
		{
			AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to allocate immediate vertex buffer ({} verts)", newCapacity);
			m_immediateCapacity = 0;
			return;
		}
		m_immediateCapacity = newCapacity;
	}

	void PhysicsDebugRenderer::DestroyImmediateBuffer()
	{
		if (m_immediateVertexHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_immediateVertexHandle);
			m_immediateVertexHandle = {};
		}
	}

	void PhysicsDebugRenderer::DrawImmediateDebugPrimitives(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr)
	{
		if (m_frameDebugVertices == nullptr || m_frameDebugVertices->empty())
		{
			return;
		}
		const std::vector<DebugVertex>* drawList = m_frameDebugVertices;

		const auto immediateCount = static_cast<std::uint32_t>(drawList->size());
		EnsureImmediateBufferCapacity(immediateCount);
		if (!m_immediateVertexHandle.IsValid())
		{
			return;
		}

		const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(m_immediateVertexHandle);
		std::memcpy(mapped.mappedPtr, drawList->data(), static_cast<std::size_t>(immediateCount) * sizeof(DebugVertex));
		gpu::ResourceRegistry::FlushMappedBuffer(m_immediateVertexHandle, 0, static_cast<gpu::DeviceSize>(immediateCount) * sizeof(DebugVertex));

		const DebugPc pc{.frameAddr = frameConstantsAddr, .tintColor = glm::vec4(1.0f), .model = glm::mat4(1.0f)};
		cmd.PushDataRaw(0, std::as_bytes(std::span{&pc, 1}));

		cmd.BindVertexBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_immediateVertexHandle));
		cmd.Draw(immediateCount, 1, 0, 0);
	}

	void PhysicsDebugRenderer::ExtractShapes(const World& world, std::vector<PhysicsDebugInstance>& out) const
	{
		ExtractPhysicsDebugShapes(world, out, m_colorMode);
	}

	void PhysicsDebugRenderer::DrawPhysicsDebugShapes(gpu::CommandList& cmd, std::uint64_t frameConstantsAddr) const
	{
		// nothing. The render thread checks no flag and reads no global state - the
		if (m_frameShapes == nullptr)
		{
			return;
		}

		for (const PhysicsDebugInstance& inst: *m_frameShapes)
		{
			gpu::BufferHandle vertexHandle{};
			std::uint32_t vertexCount = 0;
			switch (inst.shape)
			{
				case PhysicsShapeType::Box:
					vertexHandle = m_boxVertexHandle;
					vertexCount = m_boxVertexCount;
					break;
				case PhysicsShapeType::Sphere:
					vertexHandle = m_sphereVertexHandle;
					vertexCount = m_sphereVertexCount;
					break;
				case PhysicsShapeType::Capsule:
					vertexHandle = m_capsuleVertexHandle;
					vertexCount = m_capsuleVertexCount;
					break;
				case PhysicsShapeType::Cylinder:
					vertexHandle = m_cylinderVertexHandle;
					vertexCount = m_cylinderVertexCount;
					break;
				case PhysicsShapeType::ConvexHull:
				case PhysicsShapeType::Mesh:
					// No baked gizmo mesh for a mesh-sourced shape yet - vertexHandle stays
					// invalid, so the IsValid() check below skips drawing it, same as any
					// other unhandled case would have before this switch became exhaustive.
					break;
			}

			if (!vertexHandle.IsValid() || vertexCount == 0)
			{
				continue;
			}

			const DebugPc pc{.frameAddr = frameConstantsAddr, .tintColor = inst.tint, .model = inst.model};
			cmd.PushDataRaw(0, std::as_bytes(std::span{&pc, 1}));
			cmd.BindVertexBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(vertexHandle));
			cmd.Draw(vertexCount, 1, 0, 0);
		}
	}

	void PhysicsDebugRenderer::RegisterPass(RenderGraph& graph, RGImage color, RGImage depth, gpu::Extent2D extent)
	{
		if (!m_enabled)
		{
			return;
		}

		if (!color.IsValid())
		{
			color = aether::RenderGraph::GetSwapchainColor();
		}
		if (!depth.IsValid())
		{
			depth = aether::RenderGraph::GetSwapchainDepth();
		}

		auto pass = graph.AddPass("$Debug");
		if (extent.width != 0 && extent.height != 0)
		{
			pass.SetExtent(extent);
		}
		pass.WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .WriteDepth(depth, gpu::LoadOp::Load, gpu::StoreOp::DontCare)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (!m_frameDebugEnabled || !m_pipelineHandle.IsValid())
			                {
				                return;
			                }

			                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(m_pipelineHandle);
			                gpu::CommandList& cmd = ctx.recorder;
			                cmd.BindPipeline(resolved.state);
			                cmd.SetLineWidth(2.0f);

			                DrawImmediateDebugPrimitives(cmd, ctx.frameConstantsAddr);
			                DrawPhysicsDebugShapes(cmd, ctx.frameConstantsAddr);
		                });
	}
} // namespace aether
