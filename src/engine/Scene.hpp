#pragma once

#include <cstddef>
#include <unordered_map>

#include <glm/glm.hpp>

namespace aether
{
	class GraphicsPipeline;
	class Mesh;
	class RenderQueue;

	// Describes a renderable object when registering it with the Scene.
	struct RenderObjectDesc
	{
		const GraphicsPipeline* pipeline   = nullptr;
		const Mesh*             mesh       = nullptr;      // null = shader-hardcoded verts
		std::uint32_t           vertexCount  = 0;          // used when mesh == nullptr
		std::uint32_t           albedoSlot = 0xFFFFFFFFu; // 0xFFFFFFFF → vertex colour fallback
	};

	// A container of persistent renderable objects. App layers register objects
	// once (OnAttach) and update their transforms (OnUpdate). The engine calls
	// FlushToQueue() each frame so app code never touches DrawCommands directly.
	class Scene
	{
	public:
		struct Handle
		{
			std::size_t id = 0;
			[[nodiscard]] bool IsValid() const { return id != 0; }
		};

		[[nodiscard]] Handle AddRenderObject(const RenderObjectDesc& desc);
		void RemoveRenderObject(Handle handle);
		void SetTransform(Handle handle, const glm::mat4& transform);
		void SetViewProjection(const glm::mat4& viewProjection);
		[[nodiscard]] const glm::mat4& GetViewProjection() const { return m_viewProjection; }

		// Engine-internal: write all registered objects as DrawCommands into the queue.
		void FlushToQueue(RenderQueue& queue) const;

	private:
		struct RenderObject
		{
			RenderObjectDesc desc;
			glm::mat4        transform{ 1.0f };
		};

		std::unordered_map<std::size_t, RenderObject> m_objects;
		std::size_t m_nextId = 1;
		glm::mat4   m_viewProjection{ 1.0f };
	};
}
