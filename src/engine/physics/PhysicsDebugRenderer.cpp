#include "physics/PhysicsDebugRenderer.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>

#include "rendering/RenderGraph.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Logger.hpp"
#include "scene/World.hpp"
#include "scene/System.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	namespace
	{
		glm::vec4 GetColorForMotionType(const PhysicsMotionType motionType)
		{
			switch (motionType)
			{
				case PhysicsMotionType::Static:
					return {1.0f, 0.2f, 0.2f, 1.0f}; // Red
				case PhysicsMotionType::Kinematic:
					return {0.2f, 1.0f, 0.2f, 1.0f}; // Green
				case PhysicsMotionType::Dynamic:
					return {0.2f, 0.4f, 1.0f, 1.0f}; // Blue
			}
			return {1.0f, 1.0f, 0.0f, 1.0f}; // Yellow fallback
		}

		// Engine-side wrapper around gpu::ResourceRegistry::CreateMappedBuffer
		// for a static vertex buffer. The buffer is host-visible and uploaded
		// once at creation. Returns the handle (caller stores it as a member).
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

	static bool s_debugRenderingEnabled = true; // F6 toggles from DebugLayer

	void SetDebugRenderingEnabled(bool enabled)
	{
		s_debugRenderingEnabled = enabled;
	}

	bool IsDebugRenderingEnabled()
	{
		return s_debugRenderingEnabled;
	}

	PhysicsDebugRenderer::~PhysicsDebugRenderer()
	{
		Shutdown();
	}

	PhysicsDebugRenderer::PhysicsDebugRenderer(PhysicsDebugRenderer&& rhs) noexcept
	      : m_enabled(rhs.m_enabled),
	        m_selfTestEnabled(rhs.m_selfTestEnabled),
	        m_colorMode(rhs.m_colorMode),
	        m_world(rhs.m_world),
	        m_frameDebugVertices(rhs.m_frameDebugVertices),
	        m_colorFormat(rhs.m_colorFormat),
	        m_depthFormat(rhs.m_depthFormat),
	        m_pipelineHandle(rhs.m_pipelineHandle),
	        m_boxVertexHandle(rhs.m_boxVertexHandle),
	        m_boxVertexCount(rhs.m_boxVertexCount),
	        m_sphereVertexHandle(rhs.m_sphereVertexHandle),
	        m_sphereVertexCount(rhs.m_sphereVertexCount),
	        m_capsuleVertexHandle(rhs.m_capsuleVertexHandle),
	        m_capsuleVertexCount(rhs.m_capsuleVertexCount),
	        m_immediateVertexHandle(rhs.m_immediateVertexHandle),
	        m_immediateCapacity(rhs.m_immediateCapacity)
	{
		rhs.m_pipelineHandle = {};
		rhs.m_boxVertexHandle = {};
		rhs.m_sphereVertexHandle = {};
		rhs.m_capsuleVertexHandle = {};
		rhs.m_immediateVertexHandle = {};
		rhs.m_immediateCapacity = 0;
	}

	PhysicsDebugRenderer& PhysicsDebugRenderer::operator=(PhysicsDebugRenderer&& rhs) noexcept
	{
		if (this != &rhs)
		{
			Shutdown();
			m_enabled = rhs.m_enabled;
			m_selfTestEnabled = rhs.m_selfTestEnabled;
			m_colorMode = rhs.m_colorMode;
			m_world = rhs.m_world;
			m_frameDebugVertices = rhs.m_frameDebugVertices;
			m_colorFormat = rhs.m_colorFormat;
			m_depthFormat = rhs.m_depthFormat;
			m_pipelineHandle = rhs.m_pipelineHandle;
			m_boxVertexHandle = rhs.m_boxVertexHandle;
			m_boxVertexCount = rhs.m_boxVertexCount;
			m_sphereVertexHandle = rhs.m_sphereVertexHandle;
			m_sphereVertexCount = rhs.m_sphereVertexCount;
			m_capsuleVertexHandle = rhs.m_capsuleVertexHandle;
			m_capsuleVertexCount = rhs.m_capsuleVertexCount;
			m_immediateVertexHandle = rhs.m_immediateVertexHandle;
			m_immediateCapacity = rhs.m_immediateCapacity;

			rhs.m_pipelineHandle = {};
			rhs.m_boxVertexHandle = {};
			rhs.m_sphereVertexHandle = {};
			rhs.m_capsuleVertexHandle = {};
			rhs.m_immediateVertexHandle = {};
			rhs.m_immediateCapacity = 0;
		}
		return *this;
	}

	void PhysicsDebugRenderer::Init(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat)
	{
		m_colorFormat = colorFormat;
		m_depthFormat = depthFormat;
		CreateWireframePipeline(gpu, colorFormat, depthFormat);
		CreateBoxGeometry();
		CreateSphereGeometry();
		CreateCapsuleGeometry();
		m_immediateCapacity = 0;
		m_enabled = true;
		m_colorMode = PhysicsDebugColorMode::ByMotionType;
	}

	void PhysicsDebugRenderer::Shutdown()
	{
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

		constexpr std::uint32_t kPushConstantSize = sizeof(std::uint64_t) + sizeof(glm::vec4) + sizeof(glm::mat4);

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
		        .pushConstantSize = kPushConstantSize,
		        .pushConstantStages = gpu::ShaderStage::Vertex,
		        .topology = gpu::PrimitiveTopology::LineList,
		        .polygonMode = gpu::PolygonMode::Line,
		        .vertexBindings = kBindings,
		        .vertexAttributes = kAttribs,
		        .lineWidthDynamic = true,
		        .debugName = "PhysicsDebug.Pipeline",
		};

		m_pipelineHandle = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), gpu.GetPipelineCache(), desc);
	}

	void PhysicsDebugRenderer::CreateBoxGeometry()
	{
		const std::array<glm::vec3, 24> kBoxEdges = {
		        // Bottom face (4 edges)
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        // Top face (4 edges)
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        // Vertical edges (4 edges)
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		};

		std::array<DebugVertex, 24> vertices;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
		for (std::size_t i = 0; i < kBoxEdges.size(); ++i)
		{
			vertices[i] = DebugVertex{.position = kBoxEdges[i], .color = kWhite};
		}

		m_boxVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_boxVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.BoxGeometry");
	}

	void PhysicsDebugRenderer::CreateSphereGeometry()
	{
		std::vector<DebugVertex> vertices;
		vertices.reserve(static_cast<size_t>(64 * 3));

		constexpr int kSegments = 16;
		constexpr float kTubeRadius = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;
		constexpr float kPi = std::numbers::pi_v<float>;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;

			for (int j = 0; j < kSegments; ++j)
			{
				const float phi1 = (static_cast<float>(j) * kPi) / kSegments;
				const float phi2 = (static_cast<float>(j + 1) * kPi) / kSegments;

				vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta1), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta1)}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta2), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta2)}, .color = kWhite});

				vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta1), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta1)}, .color = kWhite});
				vertices.push_back(DebugVertex{.position = glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta2), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta2)}, .color = kWhite});
			}
		}

		m_sphereVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_sphereVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.SphereGeometry");
	}

	void PhysicsDebugRenderer::CreateCapsuleGeometry()
	{
		std::vector<DebugVertex> vertices;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

		constexpr int kSegments = 12;
		constexpr int kDomeSteps = 3;
		constexpr float kRadius = 0.5f;
		constexpr float kHalfHeight = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;
		constexpr float kHalfPi = 1.57079632679f;
		// Meridian lines: north pole -> top dome -> cylinder -> bottom dome -> south pole.
		for (int i = 0; i < kSegments; ++i)
		{
			const float theta = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float dx = std::cos(theta);
			const float dz = std::sin(theta);

			// Build waypoints: north pole, top dome points, bottom dome points, south pole.
			// Cylinder top = domePoints[kDomeSteps-1], cylinder bottom = bottomDome[0].
			glm::vec3 prev = glm::vec3{0.0f, kHalfHeight + kRadius, 0.0f}; // north pole

			// Top hemisphere: north pole -> cylinder top.
			for (int j = 1; j <= kDomeSteps; ++j)
			{
				const float phi = (static_cast<float>(j) * kHalfPi) / kDomeSteps;
				const glm::vec3 p{kRadius * std::sin(phi) * dx, kHalfHeight + kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			// Cylinder: top -> bottom.
			for (int j = 0; j < kDomeSteps; ++j)
			{
				const float y = kHalfHeight - static_cast<float>(j + 1) * (2.0f * kHalfHeight) / kDomeSteps;
				const glm::vec3 p{kRadius * dx, y, kRadius * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			// Bottom hemisphere: cylinder bottom -> south pole.
			for (int j = 1; j <= kDomeSteps; ++j)
			{
				const float phi = kHalfPi + (static_cast<float>(j) * kHalfPi) / kDomeSteps;
				const glm::vec3 p{kRadius * std::sin(phi) * dx, -kHalfHeight - kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			// South pole (final point closing the meridian).
			const glm::vec3 southPole{0.0f, -kHalfHeight - kRadius, 0.0f};
			vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
			vertices.push_back(DebugVertex{.position = southPole, .color = kWhite});
		}

		// Horizontal rings at cylinder top and bottom.
		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;
			const float cx1 = kRadius * std::cos(theta1);
			const float cz1 = kRadius * std::sin(theta1);
			const float cx2 = kRadius * std::cos(theta2);
			const float cz2 = kRadius * std::sin(theta2);

			// Top ring.
			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, kHalfHeight, cz2}, .color = kWhite});

			// Bottom ring.
			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, -kHalfHeight, cz2}, .color = kWhite});
		}

		m_capsuleVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_capsuleVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.CapsuleGeometry");
	}

	// -- Free-function debug primitive builders -------------------------------

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
	{
		out.push_back(DebugVertex{.position = a, .color = color});
		out.push_back(DebugVertex{.position = b, .color = color});
	}

	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color)
	{
		// 8 corners
		const glm::vec3 c000{min.x, min.y, min.z};
		const glm::vec3 c100{max.x, min.y, min.z};
		const glm::vec3 c010{min.x, max.y, min.z};
		const glm::vec3 c110{max.x, max.y, min.z};
		const glm::vec3 c001{min.x, min.y, max.z};
		const glm::vec3 c101{max.x, min.y, max.z};
		const glm::vec3 c011{min.x, max.y, max.z};
		const glm::vec3 c111{max.x, max.y, max.z};

		// 4 bottom
		AddDebugLine(out, c000, c100, color);
		AddDebugLine(out, c100, c101, color);
		AddDebugLine(out, c101, c001, color);
		AddDebugLine(out, c001, c000, color);
		// 4 top
		AddDebugLine(out, c010, c110, color);
		AddDebugLine(out, c110, c111, color);
		AddDebugLine(out, c111, c011, color);
		AddDebugLine(out, c011, c010, color);
		// 4 verticals
		AddDebugLine(out, c000, c010, color);
		AddDebugLine(out, c100, c110, color);
		AddDebugLine(out, c101, c111, color);
		AddDebugLine(out, c001, c011, color);
	}

	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color)
	{
		const glm::mat3 rot{rotation};
		const std::array<glm::vec3, 8> corners{
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, halfExtents.z},
		};

		// 4 bottom
		AddDebugLine(out, corners[0], corners[1], color);
		AddDebugLine(out, corners[1], corners[2], color);
		AddDebugLine(out, corners[2], corners[3], color);
		AddDebugLine(out, corners[3], corners[0], color);
		// 4 top
		AddDebugLine(out, corners[4], corners[5], color);
		AddDebugLine(out, corners[5], corners[6], color);
		AddDebugLine(out, corners[6], corners[7], color);
		AddDebugLine(out, corners[7], corners[4], color);
		// 4 verticals
		AddDebugLine(out, corners[0], corners[4], color);
		AddDebugLine(out, corners[1], corners[5], color);
		AddDebugLine(out, corners[2], corners[6], color);
		AddDebugLine(out, corners[3], corners[7], color);
	}

	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color, int segments)
	{
		constexpr float kPi = std::numbers::pi_v<float>;
		const float kTwoPi = 2.0f * kPi;
		const int clamped = segments < 4 ? 4 : segments;

		auto pointOnCircle = [&](float theta, float phi) -> glm::vec3
		{
			return center + radius * glm::vec3{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
		};

		// 3 great circles (XY, XZ, YZ planes) for a recognizable sphere outline.
		// XY plane (phi=pi/2)
		for (int i = 0; i < clamped; ++i)
		{
			const float t1 = static_cast<float>(i) * kTwoPi / clamped;
			const float t2 = static_cast<float>(i + 1) * kTwoPi / clamped;
			AddDebugLine(out, pointOnCircle(t1, kPi * 0.5f), pointOnCircle(t2, kPi * 0.5f), color);
		}
		// XZ plane
		for (int i = 0; i < clamped; ++i)
		{
			const float p1 = static_cast<float>(i) * kPi / clamped;
			const float p2 = static_cast<float>(i + 1) * kPi / clamped;
			AddDebugLine(out, pointOnCircle(0.0f, p1), pointOnCircle(0.0f, p2), color);
		}
		// YZ plane
		for (int i = 0; i < clamped; ++i)
		{
			const float p1 = static_cast<float>(i) * kPi / clamped;
			const float p2 = static_cast<float>(i + 1) * kPi / clamped;
			AddDebugLine(out, pointOnCircle(kPi * 0.5f, p1), pointOnCircle(kPi * 0.5f, p2), color);
		}
	}

	void AddDebugFrustum(std::vector<DebugVertex>& out, const glm::mat4& viewProj, const glm::vec4& color)
	{
		const glm::mat4 inv = glm::inverse(viewProj);

		// NDC cube corners (clip space)
		const std::array<glm::vec4, 8> ndc{
		        glm::vec4{-1.0f, -1.0f, -1.0f, 1.0f},
		        glm::vec4{1.0f, -1.0f, -1.0f, 1.0f},
		        glm::vec4{1.0f, 1.0f, -1.0f, 1.0f},
		        glm::vec4{-1.0f, 1.0f, -1.0f, 1.0f},
		        glm::vec4{-1.0f, -1.0f, 1.0f, 1.0f},
		        glm::vec4{1.0f, -1.0f, 1.0f, 1.0f},
		        glm::vec4{-1.0f, 1.0f, 1.0f, 1.0f},
		        glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
		};

		std::array<glm::vec3, 8> world;
		for (std::size_t i = 0; i < ndc.size(); ++i)
		{
			const glm::vec4 h = inv * ndc[i];
			world[i] = glm::vec3(h) / h.w;
		}

		// Near face (z=-1): 0,1,2,3
		AddDebugLine(out, world[0], world[1], color);
		AddDebugLine(out, world[1], world[2], color);
		AddDebugLine(out, world[2], world[3], color);
		AddDebugLine(out, world[3], world[0], color);
		// Far face (z=+1): 4,5,6,7
		AddDebugLine(out, world[4], world[5], color);
		AddDebugLine(out, world[5], world[6], color);
		AddDebugLine(out, world[6], world[7], color);
		AddDebugLine(out, world[7], world[4], color);
		// Connecting edges
		AddDebugLine(out, world[0], world[4], color);
		AddDebugLine(out, world[1], world[5], color);
		AddDebugLine(out, world[2], world[6], color);
		AddDebugLine(out, world[3], world[7], color);
	}

	void AddDebugAxes(std::vector<DebugVertex>& out, const glm::mat4& transform, float length)
	{
		const glm::vec3 origin = glm::vec3(transform[3]);
		const glm::vec3 xAxis = glm::vec3(transform[0]) * length;
		const glm::vec3 yAxis = glm::vec3(transform[1]) * length;
		const glm::vec3 zAxis = glm::vec3(transform[2]) * length;
		AddDebugLine(out, origin, origin + xAxis, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
		AddDebugLine(out, origin, origin + yAxis, glm::vec4(0.2f, 1.0f, 0.2f, 1.0f));
		AddDebugLine(out, origin, origin + zAxis, glm::vec4(0.2f, 0.4f, 1.0f, 1.0f));
	}

	void PhysicsDebugRenderer::AppendSelfTestPattern(std::vector<DebugVertex>& out)
	{
		// DIAGNOSTIC: a fullscreen NDC diamond. Pushed with the bypass flag
		// (tint.w == 2.0). If this is visible the pipeline is alive and writes
		// to the swapchain color; the issue is geometry/transform. If not,
		// the color attachment or pipeline is fundamentally broken.
		// World-space sanity pattern: RGB axes + a 1m wireframe AABB at the origin.
		AddDebugAxes(out, glm::mat4(1.0f), 1.0f);
		AddDebugAabb(out, glm::vec3(-0.5f), glm::vec3(0.5f), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
	}

	void PhysicsDebugRenderer::EnsureImmediateBufferCapacity(std::uint32_t vertexCount)
	{
		constexpr std::uint32_t kInitialImmediateCapacity = 4096;

		if (m_immediateCapacity >= vertexCount && m_immediateVertexHandle.IsValid())
		{
			return;
		}

		// Grow geometrically to amortize realloc cost.
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

	void PhysicsDebugRenderer::RegisterPass(RenderGraph& graph)
	{
		if (!m_enabled)
		{
			return;
		}

		auto color = graph.GetSwapchainColor();
		auto depth = graph.GetSwapchainDepth();

		graph.AddPass("$Debug")
		        .WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .WriteDepth(depth, gpu::LoadOp::Load, gpu::StoreOp::DontCare)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (!s_debugRenderingEnabled || !m_pipelineHandle.IsValid())
			                {
				                return;
			                }

			                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(m_pipelineHandle);
			                gpu::CommandList& cmd = ctx.recorder;

			                // Push-constant layout: { uint64 frameAddr, vec4 tint, mat4 model }
			                struct DebugPc
			                {
				                std::uint64_t frameAddr;
				                glm::vec4 tintColor;
				                glm::mat4 model;
			                };
			                static_assert(sizeof(DebugPc) == 88);

			                cmd.BindPipeline(resolved.pipeline, resolved.layout);
			                cmd.SetLineWidth(2.0f);

			                // 1) Immediate-mode batched debug primitives.
			                // Combine the per-frame debugVertices (set by the engine from
			                // RenderFramePacket::debugVertices) with the optional self-test
			                // pattern. Both are world-space; we apply identity model and
			                // white tint so per-vertex colors pass through unchanged.
			                std::vector<DebugVertex> scratch;
			                std::vector<DebugVertex>* drawList = nullptr;
			                if (m_frameDebugVertices != nullptr && !m_frameDebugVertices->empty())
			                {
				                drawList = const_cast<std::vector<DebugVertex>*>(m_frameDebugVertices);
			                }
			                if (m_selfTestEnabled)
			                {
				                if (drawList == nullptr)
				                {
					                scratch.reserve(64);
					                drawList = &scratch;
				                }
				                AppendSelfTestPattern(*drawList);
			                }
			                if (drawList != nullptr && !drawList->empty())
			                {
				                const auto immediateCount = static_cast<std::uint32_t>(drawList->size());
				                EnsureImmediateBufferCapacity(immediateCount);
				                if (m_immediateVertexHandle.IsValid())
				                {
					                const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(m_immediateVertexHandle);
					                std::memcpy(mapped.mappedPtr, drawList->data(), static_cast<std::size_t>(immediateCount) * sizeof(DebugVertex));
					                // Host-visible Coherent memory doesn't strictly need a
					                // flush, but the registry's helper is a no-op in that
					                // case and flushes the MAPPED range for non-coherent
					                // pools, so it's safe to call unconditionally.
					                gpu::ResourceRegistry::FlushMappedBuffer(m_immediateVertexHandle, 0, static_cast<gpu::DeviceSize>(immediateCount) * sizeof(DebugVertex));

					                // White tint, identity model: per-vertex colors pass through unchanged.
					                const DebugPc pc{.frameAddr = ctx.frameConstantsAddr, .tintColor = glm::vec4(1.0f), .model = glm::mat4(1.0f)};
					                cmd.PushConstantsRaw(resolved.layout, gpu::ShaderStage::Vertex, 0, std::as_bytes(std::span{&pc, 1}));

					                cmd.BindVertexBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_immediateVertexHandle));
					                cmd.Draw(immediateCount, 1, 0, 0);
				                }
			                }

			                // 2) Physics shape components (boxes/spheres/capsules).
			                // Each entity is one draw with its own model matrix; tint carries the motion-type color
			                // which multiplies into the per-vertex white color baked into the unit geometry.
			                if (m_world != nullptr)
			                {
				                m_world->View<PhysicsDebugShapeComponent, PhysicsStateComponent, RigidBodyComponent>().each(
				                        [&](entt::entity /*entity*/, const PhysicsDebugShapeComponent& shape, const PhysicsStateComponent& state, const RigidBodyComponent& rigid)
				                        {
					                        glm::vec4 tint{1.0f, 1.0f, 0.0f, 1.0f};

					                        if (m_colorMode == PhysicsDebugColorMode::ByMotionType)
					                        {
						                        tint = GetColorForMotionType(rigid.motionType);
					                        }

					                        const glm::mat4 model = glm::translate(glm::mat4(1.0f), state.currPosition) * glm::mat4(state.currRotation) * glm::mat4(glm::scale(glm::mat4(1.0f), state.scale));

					                        gpu::BufferHandle vertexHandle{};
					                        std::uint32_t vertexCount = 0;

					                        switch (shape.shapeType)
					                        {
						                        case PhysicsShapeType::Box:
						                        {
							                        vertexHandle = m_boxVertexHandle;
							                        vertexCount = m_boxVertexCount;
							                        break;
						                        }
						                        case PhysicsShapeType::Sphere:
						                        {
							                        vertexHandle = m_sphereVertexHandle;
							                        vertexCount = m_sphereVertexCount;
							                        break;
						                        }
						                        case PhysicsShapeType::Capsule:
						                        {
							                        vertexHandle = m_capsuleVertexHandle;
							                        vertexCount = m_capsuleVertexCount;
							                        break;
						                        }
					                        }

					                        if (!vertexHandle.IsValid() || vertexCount == 0)
					                        {
						                        return;
					                        }

					                        const DebugPc pc{.frameAddr = ctx.frameConstantsAddr, .tintColor = tint, .model = model};
					                        cmd.PushConstantsRaw(resolved.layout, gpu::ShaderStage::Vertex, 0, std::as_bytes(std::span{&pc, 1}));

					                        cmd.BindVertexBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(vertexHandle));
					                        cmd.Draw(vertexCount, 1, 0, 0);
				                        });
			                }
		                });
	}
} // namespace aether
