#pragma once

namespace aether
{
	class World;
	class EffectParamBuffer;

	namespace EffectSystem
	{
		void ConnectLifecycle(World& world, EffectParamBuffer& buffer);

		void DisconnectLifecycle(World& world);
	} // namespace EffectSystem
} // namespace aether
