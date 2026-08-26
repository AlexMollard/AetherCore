// Custom scene serde for MaterialComponent. Capture resolves texture handles to paths
// through the registries; apply re-acquires them, registers with the asset DB, and
// assigns the material - all needing the registries / load deps, so it cannot be a
// plain reflected field. Runs after the mesh serde (lower order) so the renderer exists.

#include "scene/SceneComponentSerde.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CaptureMaterial(SceneCaptureContext& c)
		{
			const auto* mc = c.world.TryGet<MaterialComponent>(c.entity);
			if (mc == nullptr)
			{
				return;
			}
			MaterialRecord mat;
			bool haveAsset = false;
			if (const auto* inst = c.world.TryGet<MaterialInstanceComponent>(c.entity))
			{
				mat.asset = inst->asset;
				haveAsset = true;
			}
			else
			{
				haveAsset = c.materials.TryDescribe(mc->handle, mat.asset);
			}
			if (!haveAsset)
			{
				return;
			}
			const auto pathOf = [&c](TextureHandle h, std::string& out)
			{
				if (h.IsValid() && h.index != TextureHandle::kBrokenIndex)
				{
					c.textures.TryGetPath(h, out);
				}
			};
			pathOf(mat.asset.albedoTex, mat.albedoPath);
			pathOf(mat.asset.normalTex, mat.normalPath);
			pathOf(mat.asset.metallicRoughnessTex, mat.metallicRoughnessPath);
			pathOf(mat.asset.occlusionTex, mat.occlusionPath);
			pathOf(mat.asset.emissiveTex, mat.emissivePath);
			c.rec.material = std::move(mat);
		}

		void ApplyMaterial(SceneApplyContext& c)
		{
			if (!c.rec.material || c.deps.assets == nullptr)
			{
				return;
			}
			MaterialAsset asset = c.rec.material->asset;
			TextureRegistry& textures = c.deps.assets->GetTextureRegistry();
			const auto acquire = [&textures](const std::string& path, TextureHandle& out, TextureColorSpace colorSpace)
			{
				out = path.empty() ? TextureHandle{} : textures.Acquire(path, colorSpace);
			};
			// Base colour and emissive encode light; the rest encode numbers and must not
			// be run through the sampler's sRGB decode.
			acquire(c.rec.material->albedoPath, asset.albedoTex, TextureColorSpace::Srgb);
			acquire(c.rec.material->normalPath, asset.normalTex, TextureColorSpace::Linear);
			acquire(c.rec.material->metallicRoughnessPath, asset.metallicRoughnessTex, TextureColorSpace::Linear);
			acquire(c.rec.material->occlusionPath, asset.occlusionTex, TextureColorSpace::Linear);
			acquire(c.rec.material->emissivePath, asset.emissiveTex, TextureColorSpace::Srgb);
			if (c.deps.assetDatabase != nullptr)
			{
				for (const std::string& texPath: {c.rec.material->albedoPath, c.rec.material->normalPath, c.rec.material->metallicRoughnessPath, c.rec.material->occlusionPath, c.rec.material->emissivePath})
				{
					if (!texPath.empty())
					{
						c.deps.assetDatabase->Register(MakeTextureSource(texPath));
					}
				}
			}
			MaterialSystem::AssignMaterial(c.world, c.entity, c.deps.assets->GetMaterialRegistry(), c.deps.assets->GetPipelineCache(), asset);
			for (const TextureHandle h: {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex})
			{
				if (h.IsValid())
				{
					textures.Release(h);
				}
			}
		}

		AE_SCENE_SERDE(Material, "Material", 20, CaptureMaterial, ApplyMaterial)
	} // namespace
} // namespace aether::app::scene
