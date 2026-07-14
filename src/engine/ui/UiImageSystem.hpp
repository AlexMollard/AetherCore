#pragma once

namespace aether
{
	class World;
	class TextureRegistry;

	namespace UiImageSystem
	{
		void ConnectLifecycle(World& world, TextureRegistry& textures);

		void DisconnectLifecycle(World& world);
	} // namespace UiImageSystem
} // namespace aether
