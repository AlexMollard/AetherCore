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

		// Owns the one connection ConnectLifecycle made, so DisconnectLifecycle can
		// drop exactly that sink. sink.disconnect() detaches EVERY observer on the
		// signal, and disconnect<&Fn>(buffer) needs the buffer the API no longer
		// receives; release() needs neither. No-op until something was connected.
		entt::connection s_effectParamsConnection{};
	} // namespace

	void EffectSystem::ConnectLifecycle(World& world, EffectParamBuffer& buffer)
	{
		s_effectParamsConnection = world.GetRegistry().on_destroy<EffectParamsComponent>().connect<&OnEffectParamsDestroyed>(buffer);
	}

	void EffectSystem::DisconnectLifecycle(World&)
	{
		s_effectParamsConnection.release();
	}

} // namespace aether
