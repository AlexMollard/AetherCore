#include "scripting/interop/InteropCommon.hpp"

#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

// foundation). Everything here runs on the producer/game thread inside a managed

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API void aether_entity_set_parent(std::uint32_t child, std::uint32_t parent)
{
	SafeExport([&] -> void
	{
	// SetParent emplaces HierarchyComponent on both sides, so both ids must be
	// alive (parent 0 detaches to root and stays legal) before touching the registry.
	if (!EntityAlive(child) || (parent != 0 && !EntityAlive(parent)))
	{
		return;
	}
	aether::ecs::SetParent(ActiveWorld(), aether::Entity{child}, aether::Entity{parent});
	});
}

AE_SCRIPT_API std::uint32_t aether_entity_get_parent(std::uint32_t id)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	return h != nullptr ? h->parent.id : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_entity_child_count(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	return h != nullptr ? static_cast<std::int32_t>(h->children.size()) : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_entity_child_at(std::uint32_t id, std::int32_t index)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* h = ActiveWorld().TryGet<aether::HierarchyComponent>(aether::Entity{id});
	if (h == nullptr || index < 0 || static_cast<std::size_t>(index) >= h->children.size())
	{
		return 0;
	}
	return h->children[static_cast<std::size_t>(index)].id;
	});
}

AE_SCRIPT_API void aether_entity_set_active(std::uint32_t id, std::int32_t active)
{
	SafeExport([&] -> void
	{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.GetRegistry().valid(aether::World::ToEntt(e)))
	{
		return;
	}
	if (active != 0)
	{
		world.Remove<aether::DisabledComponent>(e);
	}
	else
	{
		world.EmplaceOrReplace<aether::DisabledComponent>(e);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_entity_is_active(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.GetRegistry().valid(aether::World::ToEntt(e)))
	{
		return 0;
	}
	return aether::ecs::IsActiveInHierarchy(world, e) ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_scene_load(const char* name)
{
	SafeExport([&] -> void
	{
	if (name == nullptr || name[0] == '\0')
	{
		return;
	}
	// Deferred: the switch runs after the script update completes (see
	// ScriptComponentSystem) - never inside this callback.
	aether::app::scripting::ActiveContext().pendingSceneLoad = name;
	});
}

AE_SCRIPT_API std::uint32_t aether_scene_find_by_name(const char* name)
{
	return SafeExport([&] -> std::uint32_t
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
	});
}

AE_SCRIPT_API std::uint32_t aether_scene_create_entity(const char* name, Vec3 pos)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& world = ActiveWorld();
	const aether::Entity e = world.Create();
	world.Emplace<aether::NameComponent>(e, aether::NameComponent{.name = name != nullptr ? name : "Entity"});
	world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), ToGlm(pos))});
	// Scene.Create is a runtime-only script API (no editor authoring path reaches it),
	// so a save mid-Play must never bake it in - see Entity.MarkTransient's own doc.
	world.Emplace<aether::SceneTransientComponent>(e);
	return e.id;
	});
}

AE_SCRIPT_API std::uint32_t aether_scene_instantiate_prefab(const char* name, Vec3 pos)
{
	return SafeExport([&] -> std::uint32_t
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
	// Scene.Instantiate is a runtime-only script API (no editor authoring path reaches
	// it), so a save mid-Play must never bake the spawn in.
	const aether::Entity root = aether::app::scene::InstantiatePrefab(*prefab, ActiveWorld(), aether::app::scene::MakeApplySceneDeps(*services), xform, nullptr, /*markTransient=*/true);
	return root.id;
	});
}
