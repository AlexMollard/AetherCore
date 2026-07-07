#include "ui/UiImageSystem.hpp"

#include <entt/entt.hpp>

#include "material/TextureRegistry.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether
{
	namespace
	{
		void OnUiImageDestroyed(TextureRegistry& textures, entt::registry& r, entt::entity e)
		{
			const ui::UIImage& img = r.get<ui::UIImage>(e);
			if (img.texture.IsValid())
			{
				textures.Release(img.texture);
			}
		}
	} // namespace

	void UiImageSystem::ConnectLifecycle(World& world, TextureRegistry& textures)
	{
		world.GetRegistry().on_destroy<ui::UIImage>().connect<&OnUiImageDestroyed>(textures);
	}

	void UiImageSystem::DisconnectLifecycle(World& world)
	{
		world.GetRegistry().on_destroy<ui::UIImage>().disconnect();
	}
} // namespace aether
