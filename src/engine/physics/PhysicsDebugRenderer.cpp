#include "physics/PhysicsDebugRenderer.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>

#include "Color.hpp"

#include "rendering/RenderGraph.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Logger.hpp"
#include "scene/World.hpp"
#include "scene/System.hpp"
#include "physics/PhysicsComponents.hpp"
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

		glm::vec4 GetColorForMotionType(const PhysicsMotionType motionType)
		{
			using colors::DebugRed, colors::DebugGreen, colors::DebugBlue, colors::DebugYellow;
			switch (motionType)
			{
				case PhysicsMotionType::Static:
					return DebugRed;
				case PhysicsMotionType::Kinematic:
					return DebugGreen;
				case PhysicsMotionType::Dynamic:
					return DebugBlue;
			}
			return DebugYellow;
		}

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

	static bool s_debugRenderingEnabled = false;
	static bool s_physicsDebugShapesEnabled = false;

	void SetDebugRenderingEnabled(bool enabled)
	{
		s_debugRenderingEnabled = enabled;
	}

	bool IsDebugRenderingEnabled()
	{
		return s_debugRenderingEnabled;
	}

	void SetPhysicsDebugShapesEnabled(bool enabled)
	{
		s_physicsDebugShapesEnabled = enabled;
	}

	bool IsPhysicsDebugShapesEnabled()
	{
		return s_physicsDebugShapesEnabled;
	}

	static bool s_collisionOnlyViewEnabled = false;

	void SetCollisionOnlyViewEnabled(bool enabled)
	{
		s_collisionOnlyViewEnabled = enabled;
	}

	bool IsCollisionOnlyViewEnabled()
	{
		return s_collisionOnlyViewEnabled;
	}

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
		};

		m_pipelineHandle = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
	}

	void PhysicsDebugRenderer::CreateBoxGeometry()
	{
		const std::array<glm::vec3, 24> kBoxEdges = {
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
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
		for (int i = 0; i < kSegments; ++i)
		{
			const float theta = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float dx = std::cos(theta);
			const float dz = std::sin(theta);

			glm::vec3 prev = glm::vec3{0.0f, kHalfHeight + kRadius, 0.0f};

			for (int j = 1; j <= kDomeSteps; ++j)
			{
				const float phi = (static_cast<float>(j) * kHalfPi) / kDomeSteps;
				const glm::vec3 p{kRadius * std::sin(phi) * dx, kHalfHeight + kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			for (int j = 0; j < kDomeSteps; ++j)
			{
				const float y = kHalfHeight - static_cast<float>(j + 1) * (2.0f * kHalfHeight) / kDomeSteps;
				const glm::vec3 p{kRadius * dx, y, kRadius * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			for (int j = 1; j <= kDomeSteps; ++j)
			{
				const float phi = kHalfPi + (static_cast<float>(j) * kHalfPi) / kDomeSteps;
				const glm::vec3 p{kRadius * std::sin(phi) * dx, -kHalfHeight - kRadius * std::cos(phi), kRadius * std::sin(phi) * dz};
				vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
				vertices.push_back(DebugVertex{.position = p, .color = kWhite});
				prev = p;
			}

			const glm::vec3 southPole{0.0f, -kHalfHeight - kRadius, 0.0f};
			vertices.push_back(DebugVertex{.position = prev, .color = kWhite});
			vertices.push_back(DebugVertex{.position = southPole, .color = kWhite});
		}

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;
			const float cx1 = kRadius * std::cos(theta1);
			const float cz1 = kRadius * std::sin(theta1);
			const float cx2 = kRadius * std::cos(theta2);
			const float cz2 = kRadius * std::sin(theta2);

			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, kHalfHeight, cz2}, .color = kWhite});

			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, -kHalfHeight, cz2}, .color = kWhite});
		}

		m_capsuleVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_capsuleVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.CapsuleGeometry");
	}

	void PhysicsDebugRenderer::CreateCylinderGeometry()
	{
		std::vector<DebugVertex> vertices;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
		constexpr int kSegments = 16;
		constexpr float kRadius = 0.5f;
		constexpr float kHalfHeight = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = (static_cast<float>(i) * kTwoPi) / kSegments;
			const float theta2 = (static_cast<float>(i + 1) * kTwoPi) / kSegments;
			const float cx1 = kRadius * std::cos(theta1);
			const float cz1 = kRadius * std::sin(theta1);
			const float cx2 = kRadius * std::cos(theta2);
			const float cz2 = kRadius * std::sin(theta2);

			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, kHalfHeight, cz2}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx2, -kHalfHeight, cz2}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, kHalfHeight, cz1}, .color = kWhite});
			vertices.push_back(DebugVertex{.position = glm::vec3{cx1, -kHalfHeight, cz1}, .color = kWhite});
		}

		m_cylinderVertexCount = static_cast<std::uint32_t>(vertices.size());
		m_cylinderVertexHandle = CreateStaticVertexBuffer(vertices, "PhysicsDebug.CylinderGeometry");
	}

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
	{
		out.push_back(DebugVertex{.position = a, .color = color});
		out.push_back(DebugVertex{.position = b, .color = color});
	}

	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color)
	{
		const glm::vec3 c000{min.x, min.y, min.z};
		const glm::vec3 c100{max.x, min.y, min.z};
		const glm::vec3 c010{min.x, max.y, min.z};
		const glm::vec3 c110{max.x, max.y, min.z};
		const glm::vec3 c001{min.x, min.y, max.z};
		const glm::vec3 c101{max.x, min.y, max.z};
		const glm::vec3 c011{min.x, max.y, max.z};
		const glm::vec3 c111{max.x, max.y, max.z};

		AddDebugLine(out, c000, c100, color);
		AddDebugLine(out, c100, c101, color);
		AddDebugLine(out, c101, c001, color);
		AddDebugLine(out, c001, c000, color);
		AddDebugLine(out, c010, c110, color);
		AddDebugLine(out, c110, c111, color);
		AddDebugLine(out, c111, c011, color);
		AddDebugLine(out, c011, c010, color);
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

		AddDebugLine(out, corners[0], corners[1], color);
		AddDebugLine(out, corners[1], corners[2], color);
		AddDebugLine(out, corners[2], corners[3], color);
		AddDebugLine(out, corners[3], corners[0], color);
		AddDebugLine(out, corners[4], corners[5], color);
		AddDebugLine(out, corners[5], corners[6], color);
		AddDebugLine(out, corners[6], corners[7], color);
		AddDebugLine(out, corners[7], corners[4], color);
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

		auto addCircle = [&](const glm::vec3& axisA, const glm::vec3& axisB)
		{
			for (int i = 0; i < clamped; ++i)
			{
				const float t1 = static_cast<float>(i) * kTwoPi / static_cast<float>(clamped);
				const float t2 = static_cast<float>(i + 1) * kTwoPi / static_cast<float>(clamped);
				const glm::vec3 p1 = center + radius * (axisA * std::cos(t1) + axisB * std::sin(t1));
				const glm::vec3 p2 = center + radius * (axisA * std::cos(t2) + axisB * std::sin(t2));
				AddDebugLine(out, p1, p2, color);
			}
		};

		addCircle({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		addCircle({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
		addCircle({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
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
		if (!s_physicsDebugShapesEnabled && !s_collisionOnlyViewEnabled)
		{
			return;
		}
		world.View<ColliderComponent, PhysicsStateComponent, RigidBodyComponent>().each(
		        [&](entt::entity /*entity*/, const ColliderComponent& shape, const PhysicsStateComponent& state, const RigidBodyComponent& rigid)
		        {
			        const glm::vec4 tint = m_colorMode == PhysicsDebugColorMode::ByMotionType ? GetColorForMotionType(rigid.motionType) : glm::vec4(colors::DebugYellow);
			        const glm::mat4 model = glm::translate(glm::mat4(1.0f), state.currPosition) * glm::mat4(state.currRotation) * glm::mat4(glm::scale(glm::mat4(1.0f), state.scale));
			        out.push_back({.model = model, .tint = tint, .shape = shape.shape});
		        });
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
