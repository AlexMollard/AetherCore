#pragma once

#include <cstddef>
#include <glm/glm.hpp>
#include <unordered_map>

namespace aether
{
	class GraphicsPipeline;
	class Mesh;
	class RenderQueue;
	class Scene;

	// Forward-declared here so the `friend` declaration below can name
	// `WorldRenderer::Flush` in MSVC (the in-class friend decl alone is not
	// enough to introduce the namespace). The real API lives in
	// rendering/WorldRenderer.hpp; this is a redundant function decl and
	// clang-tidy will flag it, but it must stay for the friend decl to compile.
	namespace WorldRenderer
	{
		void Flush(const Scene& scene, RenderQueue& queue);
	}

	// Describes a renderable object when registering it with the Scene.
	struct RenderObjectDesc
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr;    // null = shader-hardcoded verts
		std::uint32_t vertexCount = 0; // used when mesh == nullptr
		std::uint32_t materialIndex = 0xFFFFFFFFu;
	};

	// A container of persistent renderable objects. App layers register objects
	// once (OnAttach) and update their transforms (OnUpdate). The engine calls
	// WorldRenderer::Flush() each frame so app code never touches DrawCommands directly.
	class Scene
	{
	public:
		struct Handle
		{
			std::size_t id = 0;

			[[nodiscard]] bool IsValid() const
			{
				return id != 0;
			}
		};

		[[nodiscard]] Handle AddRenderObject(const RenderObjectDesc& desc);
		void RemoveRenderObject(Handle handle);
		void SetTransform(Handle handle, const glm::mat4& transform);
		void SetViewProjection(const glm::mat4& viewProjection);

		[[nodiscard]] const glm::mat4& GetViewProjection() const
		{
			return m_viewProjection;
		}

	private:
		friend void WorldRenderer::Flush(const Scene& scene, RenderQueue& queue);

		struct RenderObject
		{
			RenderObjectDesc desc;
			glm::mat4 transform{1.0f};
		};

		std::unordered_map<std::size_t, RenderObject> m_objects;
		std::size_t m_nextId = 1;
		glm::mat4 m_viewProjection{1.0f};
	};
} // namespace aether
