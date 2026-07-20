#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Color.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuEnums.hpp"
#include "physics/PhysicsComponents.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether::gpu
{
	class CommandList;
}

namespace aether
{
	class World;

	class GpuDevice;

	void SetDebugRenderingEnabled(bool enabled);
	bool IsDebugRenderingEnabled();
	void SetPhysicsDebugShapesEnabled(bool enabled);
	bool IsPhysicsDebugShapesEnabled();

	// Collision-only view: scene rendering (meshes, sprites, tiles) is
	// suppressed and every collider wireframe draws - for reading collision
	// against a plain background. Session-only; never persisted.
	void SetCollisionOnlyViewEnabled(bool enabled);
	bool IsCollisionOnlyViewEnabled();

	enum class PhysicsDebugColorMode : uint8_t
	{
		None,
		ByMotionType,
	};

	// DebugVertex: per-vertex line endpoint. Matches the layout in debug_vert.slang.
	struct DebugVertex
	{
		glm::vec3 position;
		glm::vec4 color;
	};

	// thread into the RenderFramePacket. The render thread replays these (picking
	struct PhysicsDebugInstance
	{
		glm::mat4 model{1.0f};
		glm::vec4 tint{1.0f};
		PhysicsShapeType shape = PhysicsShapeType::Box;
	};

	// supplied vector. Thread-safe by construction: each thread holds its own
	inline constexpr auto kDefaultDebugColor = glm::vec4(colors::DebugYellow);

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color = kDefaultDebugColor, int segments = 16);
	void AddDebugFrustum(std::vector<DebugVertex>& out, const glm::mat4& viewProj, const glm::vec4& color = kDefaultDebugColor);
	void AddDebugAxes(std::vector<DebugVertex>& out, const glm::mat4& transform, float length = 1.0f);

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
