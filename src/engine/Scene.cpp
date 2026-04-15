#include "Scene.hpp"

#include "Mesh.hpp"
#include "RenderQueue.hpp"

namespace aether
{
	Scene::Handle Scene::AddRenderObject(const RenderObjectDesc& desc)
	{
		const std::size_t id = m_nextId++;
		m_objects[id] = RenderObject{ .desc = desc };
		return Handle{ .id = id };
	}

	void Scene::RemoveRenderObject(Handle handle)
	{
		if (handle.IsValid())
		{
			m_objects.erase(handle.id);
		}
	}

	void Scene::SetTransform(Handle handle, const glm::mat4& transform)
	{
		if (auto it = m_objects.find(handle.id); it != m_objects.end())
		{
			it->second.transform = transform;
		}
	}

	void Scene::SetViewProjection(const glm::mat4& viewProjection)
	{
		m_viewProjection = viewProjection;
	}

	void Scene::FlushToQueue(RenderQueue& queue) const
	{
		for (const auto& [id, obj]: m_objects)
		{
			const std::uint32_t count = (obj.desc.mesh != nullptr) ? obj.desc.mesh->GetVertexCount() : obj.desc.vertexCount;

			queue.Submit({
			        .pipeline = obj.desc.pipeline,
			        .mesh = obj.desc.mesh,
			        .vertexCount = count,
			        .instanceCount = 1,
			        .modelMatrix = obj.transform,
			        .materialIndex = obj.desc.materialIndex,
			});
		}
	}
} // namespace aether
