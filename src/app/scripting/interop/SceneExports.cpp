#include "scripting/interop/InteropCommon.hpp"

#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

// Entity & scene manipulation exported to C# (Module 01: the Unity-style
// foundation). Everything here runs on the producer/game thread inside a managed
// call, so structural changes (create/destroy/reparent) are safe: the render
// thread reads only the RenderFramePacket, never the live ECS.

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// ── Hierarchy ───────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_entity_set_parent(std::uint32_t child, std::uint32_t parent)
{
	aether::ecs::SetParent(ActiveWorld(), aether::Entity{child}, aether::Entity{parent});
}

AE_SCRIPT_API std::uint32_t aether_entity_get_parent(std::uint32_t id)
{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	return h != nullptr ? h->parent.id : 0;
}

AE_SCRIPT_API std::int32_t aether_entity_child_count(std::uint32_t id)
{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	return h != nullptr ? static_cast<std::int32_t>(h->children.size()) : 0;
}

AE_SCRIPT_API std::uint32_t aether_entity_child_at(std::uint32_t id, std::int32_t index)
{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	if (h == nullptr || index < 0 || static_cast<std::size_t>(index) >= h->children.size())
	{
		return 0;
	}
	return h->children[static_cast<std::size_t>(index)].id;
}

// ── Active (enable / disable) ─────────────────────────────────────────────────

AE_SCRIPT_API void aether_entity_set_active(std::uint32_t id, std::int32_t active)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.GetRegistry().valid(aether::World::ToEntt(e)))
	{
		return;
	}
	if (active != 0)
	{
		world.Remove<aether::DisabledComponent>(e); // safe if absent
	}
	else
	{
		world.EmplaceOrReplace<aether::DisabledComponent>(e);
	}
}

// Active in hierarchy: false if the entity or any ancestor is disabled.
AE_SCRIPT_API std::int32_t aether_entity_is_active(std::uint32_t id)
{
	const auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.GetRegistry().valid(aether::World::ToEntt(e)))
	{
		return 0;
	}
	return aether::ecs::IsActiveInHierarchy(world, e) ? 1 : 0;
}

// ── Find ──────────────────────────────────────────────────────────────────────

AE_SCRIPT_API std::uint32_t aether_scene_find_by_name(const char* name)
{
	if (name == nullptr)
	{
		return 0;
	}
	const std::string target = name;
	auto& world = ActiveWorld();
	for (const auto handle: world.View<aether::NameComponent>())
	{
		if (world.GetRegistry().get<aether::NameComponent>(handle).name == target)
		{
			return aether::World::FromEntt(handle).id;
		}
	}
	return 0;
}

// ── Create / instantiate ──────────────────────────────────────────────────────

AE_SCRIPT_API std::uint32_t aether_scene_create_entity(const char* name, Vec3 pos)
{
	auto& world = ActiveWorld();
	const aether::Entity e = world.Create();
	world.Emplace<aether::NameComponent>(e, aether::NameComponent{.name = name != nullptr ? name : "Entity"});
	world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), ToGlm(pos))});
	return e.id;
}

// Instantiate a prefab (.prefab.toml) into the live scene, placing its root at
// `pos`. Returns the root entity id (0 if the prefab is missing/empty). The
// instantiated entities are registered for scene cleanup by ApplySceneDeps.
AE_SCRIPT_API std::uint32_t aether_scene_instantiate_prefab(const char* name, Vec3 pos)
{
	if (name == nullptr)
	{
		return 0;
	}
	aether::ServiceContainer* services = ActiveContext().services;
	if (services == nullptr)
	{
		return 0;
	}
	const auto prefab = aether::app::scene::ReadPrefabFile(name);
	if (!prefab)
	{
		return 0;
	}
	const glm::mat4 xform = glm::translate(glm::mat4(1.0f), ToGlm(pos));
	const aether::Entity root = aether::app::scene::InstantiatePrefab(*prefab, ActiveWorld(), aether::app::scene::MakeApplySceneDeps(*services), xform);
	return root.id;
}
