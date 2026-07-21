// Custom scene serde for UI Image. Unlike the other UI components (pure reflected
// data), UI Image holds a texture handle resolved to a path on capture and re-acquired
// through the load deps on apply, so it stays bespoke and lives here.

#include "scene/SceneComponentSerde.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CaptureUiImage(SceneCaptureContext& c)
		{
			const auto* im = c.world.TryGet<ui::UIImage>(c.entity);
			if (im == nullptr)
			{
				return;
			}
			UIImageRecord ir;
			ir.color = im->color;
			ir.cornerRadius = im->cornerRadius;
			ir.pixelArt = im->pixelArt;
			ir.texturePath = im->texturePath;
			c.rec.uiImage = std::move(ir);
		}

		void ApplyUiImage(SceneApplyContext& c)
		{
			if (!c.rec.uiImage)
			{
				return;
			}
			ui::UIImage im;
			im.color = c.rec.uiImage->color;
			im.cornerRadius = c.rec.uiImage->cornerRadius;
			im.pixelArt = c.rec.uiImage->pixelArt;
			im.texturePath = c.rec.uiImage->texturePath;
			im.textureDirty = !im.texturePath.empty();
			if (!im.texturePath.empty() && c.deps.assetDatabase != nullptr)
			{
				c.deps.assetDatabase->Register(MakeTextureSource(im.texturePath));
			}
			c.world.Emplace<ui::UIImage>(c.entity, im);
		}

		AE_SCENE_SERDE(UiImage, "UI Image", 40, CaptureUiImage, ApplyUiImage)
	} // namespace
} // namespace aether::app::scene
