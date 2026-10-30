#include "scripting/interop/InteropCommon.hpp"

#include <string>
#include <cstring>
#include <limits>
#include "scene/WorldStreaming.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "net/NetSpawn.hpp"
#include "systems/ScriptComponentSystem.hpp"
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
	aether::app::scene::AdoptStreamingSpawn(world, ActiveContext(), e);
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
	aether::app::scene::AdoptStreamingSpawn(ActiveWorld(), ActiveContext(), root);
	return root.id;
	});
}

AE_SCRIPT_API std::uint32_t aether_net_instantiate_predicted(const char* name, Vec3 pos)
{
	return SafeExport([&] -> std::uint32_t
	{
		if (name == nullptr || ActiveContext().services == nullptr)
			return 0;
		auto* scripts = static_cast<aether::app::ScriptComponentSystem*>(ActiveWorld().FindSystem("ScriptComponentSystem"));
		if (scripts == nullptr)
			return 0;
		const auto prefab = aether::app::scene::ReadPrefabFile(name);
		if (!prefab)
			return 0;
		std::vector<aether::Entity> created;
		const auto root = aether::app::scene::InstantiatePrefab(*prefab, ActiveWorld(),
		        aether::app::scene::MakeApplySceneDeps(*ActiveContext().services),
		        glm::translate(glm::mat4(1.0f), ToGlm(pos)), &created, /*markTransient=*/true);
		aether::app::scene::AdoptStreamingSpawn(ActiveWorld(), ActiveContext(), root);
		if (root.IsValid())
		{
			for (const aether::Entity entity: created)
				aether::net::MarkPredicted(ActiveWorld(), entity);
			scripts->AttachPredicted(ActiveWorld(), created);
		}
		return root.id;
	});
}

AE_SCRIPT_API bool aether_streaming_set_source(const char* id, Vec3 position, float loadRadius, float prefetchRadius, std::uint32_t priority)
{
	return SafeExport([&]() -> bool
	{
		if (!g_activeContext || !g_activeContext->streaming || !id || priority > 3) return false;
		aether::streaming::Source source{id, ToGlm(position), loadRadius, prefetchRadius, static_cast<aether::io::IOPriority>(priority)};
		return static_cast<bool>(g_activeContext->streaming->SetSource(std::move(source)));
	});
}

AE_SCRIPT_API bool aether_streaming_remove_source(const char* id)
{
	return SafeExport([&]() -> bool { return g_activeContext && g_activeContext->streaming && id && g_activeContext->streaming->RemoveSource(id); });
}

AE_SCRIPT_API std::uint64_t aether_streaming_request(const char* region, std::uint32_t priority, bool activate)
{
	return SafeExport([&]() -> std::uint64_t
	{
		if (!g_activeContext || !g_activeContext->streaming || !region || priority > 3) return 0;
		return g_activeContext->streaming->Request(region, activate, static_cast<aether::io::IOPriority>(priority));
	});
}

AE_SCRIPT_API bool aether_streaming_cancel(std::uint64_t request)
{
	return SafeExport([&]() -> bool { return g_activeContext && g_activeContext->streaming && g_activeContext->streaming->Cancel(request); });
}

AE_SCRIPT_API bool aether_streaming_release(std::uint64_t request)
{
	return SafeExport([&]() -> bool { return g_activeContext && g_activeContext->streaming && g_activeContext->streaming->Release(request); });
}

AE_SCRIPT_API std::uint32_t aether_streaming_get_state(std::uint64_t request)
{
	return SafeExport([&]() -> std::uint32_t
	{
		return g_activeContext && g_activeContext->streaming ? static_cast<std::uint32_t>(g_activeContext->streaming->GetRequestState(request)) : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_streaming_diagnostics(char* buffer, std::uint32_t capacity)
{
	return SafeExport([&]() -> std::uint32_t
	{
		if (buffer && capacity) buffer[0] = '\0';
		if (!g_activeContext || !g_activeContext->streaming) return 0;
		const auto json = g_activeContext->streaming->GetDiagnosticsJson();
		if (json.size() >= std::numeric_limits<std::uint32_t>::max()) return 0;
		const auto required = static_cast<std::uint32_t>(json.size() + 1);
		if (buffer && capacity >= required) std::memcpy(buffer, json.c_str(), required);
		return required;
	});
}
