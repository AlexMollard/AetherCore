#include "material/MaterialSystem.hpp"

#include <entt/entt.hpp>

#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		void OnMaterialDestroyed(MaterialRegistry& registry, entt::registry& r, entt::entity e)
		{
			registry.Release(r.get<MaterialComponent>(e).handle);
		}
	} // namespace

	void MaterialSystem::ConnectLifecycle(World& world, MaterialRegistry& registry)
	{
		world.GetRegistry().on_destroy<MaterialComponent>().connect<&OnMaterialDestroyed>(registry);
	}

	void MaterialSystem::DisconnectLifecycle(World& world)
	{
		// Disconnect-all: the engine is the only listener on this signal.
		world.GetRegistry().on_destroy<MaterialComponent>().disconnect();
	}

	void MaterialSystem::AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, const MaterialAsset& asset)
	{
		auto& r = world.GetRegistry();
		const entt::entity e = World::ToEntt(entity);
		// emplace_or_replace fires on_update (not on_destroy) for an existing
		// component, so the old handle must be released explicitly here.
		if (const auto* existing = r.try_get<MaterialComponent>(e))
		{
			registry.Release(existing->handle);
		}
		const MaterialHandle h = registry.Acquire(asset);
		r.emplace_or_replace<MaterialComponent>(e, MaterialComponent{h, registry.ResolveSlot(h)});
	}
} // namespace aether
