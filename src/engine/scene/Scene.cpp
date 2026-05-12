#include "scene/Scene.hpp"

#include "mesh/Mesh.hpp"

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
} // namespace aether
