#pragma once

namespace aether
{
	class World;
	class TextureRegistry;

	namespace UiImageSystem
	{
		// Connect the entt on_destroy hook so a ui::UIImage's acquired texture
		// handle is released through TextureRegistry when the component/entity is
		// destroyed (e.g. ReplaceScene's destroy-all sweep). Mirrors
		// MaterialSystem::ConnectLifecycle / EffectSystem::ConnectLifecycle. Call
		// once at engine setup, after the registry exists.
		void ConnectLifecycle(World& world, TextureRegistry& textures);

		// Disconnect the hook before the registry is torn down so late world
		// teardown cannot release into a dead registry.
		void DisconnectLifecycle(World& world);
	} // namespace UiImageSystem
} // namespace aether
