#pragma once

namespace aether
{
	class World;
	class EffectParamBuffer;

	namespace EffectSystem
	{
		// Connect on_destroy<EffectParamsComponent> so an entity's effect-param slot
		// is freed (deferred) through EffectParamBuffer when the component/entity is
		// destroyed. Mirrors MaterialSystem::ConnectLifecycle.
		void ConnectLifecycle(World& world, EffectParamBuffer& buffer);

		// Disconnect the hook before the buffer is torn down.
		void DisconnectLifecycle(World& world);
	} // namespace EffectSystem
} // namespace aether
