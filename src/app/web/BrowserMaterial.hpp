#pragma once

#include <string>
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app::web
{
	// Browser textures are owned by WebModels, rather than the native AssetManager.
	// Keep the requested URI until that registry resolves it on the game thread.
	struct BrowserMaterialTexture
	{
		std::string path;
		bool dirty = true;
	};

	template<typename Resolver>
	Expected<void> ResolveBrowserMaterialTexture(World& world, Entity entity, Resolver&& resolve)
	{
		auto* request = world.TryGet<BrowserMaterialTexture>(entity);
		auto* material = world.TryGet<MaterialInstanceComponent>(entity);
		if (!request || !request->dirty || !material)
		{
			return {};
		}
		TextureHandle handle;
		if (!request->path.empty())
		{
			AE_TRY(texture, resolve(request->path));
			handle = *texture;
		}
		material->asset.albedoTex = handle;
		request->dirty = false;
		return {};
	}
}
