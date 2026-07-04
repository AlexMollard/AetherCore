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
		// Acquire BEFORE releasing the old handle: a same-content reassignment
		// then dedups onto the live slot (refcount 1->2->1, no GPU write, no
		// slot churn) instead of freeing and immediately rewriting a slot that
		// in-flight frames may still be reading.
		MaterialHandle old{};
		if (const auto* existing = r.try_get<MaterialComponent>(e))
		{
			old = existing->handle;
		}
		const MaterialHandle h = registry.Acquire(asset);
		registry.Release(old); // no-op for an invalid handle
		// emplace_or_replace fires on_update (not on_destroy) for an existing
		// component, so the release above is the only one.
		r.emplace_or_replace<MaterialComponent>(e, MaterialComponent{h, registry.ResolveSlot(h)});
	}
} // namespace aether
