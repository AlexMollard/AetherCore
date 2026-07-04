#include "material/EffectSystem.hpp"

#include <entt/entt.hpp>

#include "material/EffectParamBuffer.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		void OnEffectParamsDestroyed(EffectParamBuffer& buffer, entt::registry& r, entt::entity e)
		{
			buffer.FreeSlot(r.get<EffectParamsComponent>(e).paramSlot);
		}
	} // namespace

	void EffectSystem::ConnectLifecycle(World& world, EffectParamBuffer& buffer)
	{
		world.GetRegistry().on_destroy<EffectParamsComponent>().connect<&OnEffectParamsDestroyed>(buffer);
	}

	void EffectSystem::DisconnectLifecycle(World& world)
	{
		world.GetRegistry().on_destroy<EffectParamsComponent>().disconnect();
	}
} // namespace aether
