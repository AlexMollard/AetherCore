#include "WebModels.hpp"

#include "WebScene.hpp"
#include "assets/ModelPose.hpp"
#include "io/FileSystem.hpp"
#include "material/MaterialSerializer.hpp"
#include "material/MaterialOverrides.hpp"
#include "mesh/PrimitiveMeshData.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/Hierarchy.hpp"

namespace aether::app::web
{
	WebModels::WebModels(webgpu::WebGpuDevice& graphics, TextureRegistry& textures, WebTextureSink& sink)
	      : m_graphics(graphics), m_textures(textures), m_sink(sink)
	{
	}

	Expected<MaterialAsset> WebModels::ResolveTextures(MaterialAsset asset, const std::array<std::string, 5>& paths)
	{
		std::array<TextureHandle*, 5> handles{&asset.albedoTex, &asset.normalTex, &asset.metallicRoughnessTex, &asset.occlusionTex, &asset.emissiveTex};
		for (unsigned i = 0; i < paths.size(); ++i)
		{
			if (paths[i].empty())
			{
				*handles[i] = {};
				continue;
			}
			const auto colorSpace = i == 0 || i == 4 ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
			const auto key = std::make_pair(paths[i], colorSpace);
			if (!m_texture_cache.contains(key))
			{
				// Sprite textures keep their existing encoded-color sampling contract.
				// A distinct registry preserves the 3D color/data format identity.
				m_sink.colorManaged = true;
				const auto handle = m_textures.Acquire(paths[i], colorSpace);
				m_sink.colorManaged = false;
				if (!handle.IsValid() || handle.index == TextureHandle::kBrokenIndex)
				{
					AE_UNEXPECTED(AetherError::Asset("Browser model texture failed: " + paths[i]));
				}
				m_texture_cache.emplace(key, handle);
			}
			*handles[i] = m_texture_cache.at(key);
		}
		return asset;
	}

	Expected<MaterialAsset> WebModels::ResolveMaterial(const scene::MaterialRecord& record)
	{
		auto asset = record.asset;
		std::array<std::string, 5> paths{record.albedoPath, record.normalPath, record.metallicRoughnessPath, record.occlusionPath, record.emissivePath};
		if (!record.assetPath.empty())
		{
			AE_TRY(loaded, MaterialSerializer::Load(record.assetPath));
			const auto& preset = *loaded;
			asset = preset.material;
			if (!preset.shaderVfsPath.empty() && preset.shaderVfsPath != "shaders://gltf_mesh.spv")
			{
				AE_UNEXPECTED(AetherError::Asset("Browser custom material shaders are not implemented: " + preset.shaderVfsPath));
			}
			ApplyMaterialOverrides(asset, record.asset, record.overrides);
			const std::array<std::string, 5> linked{preset.albedoPath, preset.normalPath, preset.metallicRoughnessPath, preset.occlusionPath, preset.emissivePath};
			for (unsigned i = 0; i < paths.size(); ++i)
			{
				constexpr std::array<std::string_view, 5> keys{"albedo", "normal", "metallic_roughness", "occlusion_tex", "emissive_tex"};
				if (!IsMaterialFieldOverridden(record.overrides, keys[i]))
				{
					paths[i] = linked[i];
				}
			}
		}
		return ResolveTextures(asset, paths);
	}

	Expected<void> WebModels::Apply(World& world, Entity entity, const scene::EntityRecord& record)
	{
		if (!record.mesh)
		{
			if (record.skinned || record.material)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser material or skin has no mesh: " + record.name));
			}
			return {};
		}
		Binding binding{entity, 0};
		MaterialAsset material;
		if (record.mesh->kind == MeshSourceComponent::Kind::Primitive)
		{
			const auto& name = record.mesh->path;
			if (!m_primitives.contains(name))
			{
				const auto primitive = scene::detail::PrimitiveFromName(name);
				if (!primitive)
				{
					AE_UNEXPECTED(AetherError::Asset("Unknown browser primitive: " + name));
				}
				const auto mesh = GeneratePrimitiveMesh(*primitive);
				if (!mesh || !m_graphics.UploadMesh(m_next_slot, mesh->vertices, mesh->indices))
				{
					AE_UNEXPECTED(AetherError::Asset("Browser primitive upload failed: " + name + ": " + m_graphics.MeshUploadError()));
				}
				m_primitives.emplace(name, m_next_slot++);
			}
			binding.slot = m_primitives.at(name);
		}
		else
		{
			const auto& path = record.mesh->path;
			if (!m_models.contains(path))
			{
				AE_TRY(data, assets::GltfAsset::LoadFromVfsPath(path));
				AE_TRY(pose, assets::EvaluateModelPose(*data));
				Model model{std::move(*data), {}};
				for (const auto& primitive: model.data.primitives)
				{
					if (!m_graphics.UploadMesh(m_next_slot, primitive.vertices, primitive.indices))
					{
						AE_UNEXPECTED(AetherError::Asset("Browser model upload failed: " + path + ": " + m_graphics.MeshUploadError()));
					}
					model.slots.push_back(m_next_slot++);
				}
				m_models.emplace(path, std::move(model));
			}
			auto& model = m_models.at(path);
			if (record.mesh->primitiveIndex >= model.slots.size())
			{
				AE_UNEXPECTED(AetherError::Asset("Browser model primitive index outside cooked model"));
			}
			binding.model = &model;
			binding.slot = model.slots[record.mesh->primitiveIndex];
			const auto& primitive = model.data.primitives[record.mesh->primitiveIndex];
			binding.skin = primitive.skinIndex;
			if (binding.skin < -1 || (binding.skin >= 0 && static_cast<std::size_t>(binding.skin) >= model.data.skins.size()))
			{
				AE_UNEXPECTED(AetherError::Asset("Browser model primitive skin index outside cooked model"));
			}
			if (primitive.materialIndex >= 0)
			{
				if (static_cast<std::size_t>(primitive.materialIndex) >= model.data.materials.size())
				{
					AE_UNEXPECTED(AetherError::Asset("Browser model material index outside cooked model"));
				}
				const auto& source = model.data.materials[primitive.materialIndex];
				material.baseColorFactor = source.baseColorFactor;
				material.metallicFactor = source.metallicFactor;
				material.roughnessFactor = source.roughnessFactor;
				material.emissiveFactor = source.emissiveFactor;
				material.alphaCutoff = source.alphaCutoff;
				material.doubleSided = source.doubleSided;
				material.alphaBlend = source.alphaBlend;
				material.alphaMask = source.alphaMask;
				material.additiveBlend = source.additiveBlend;
				material.modulateVertexColor = source.modulateVertexColor;
				material.uvScroll = source.uvScroll;
				material.foliage = source.foliage;
				material.bakedLighting = source.bakedLighting;
				material.sky = source.sky;
				material.lightShaft = source.lightShaft;
				material.objectLit = source.objectLit;
				material.gem = source.gem;
				material.sheen = source.sheen;
				material.water = source.water;
				material.character = source.character;
				if (!source.shaderVfsPath.empty() && source.shaderVfsPath != "shaders://gltf_mesh.spv")
				{
					AE_UNEXPECTED(AetherError::Asset("Browser custom model shaders are not implemented: " + source.shaderVfsPath));
				}
				AE_TRY(resolved, ResolveTextures(material, {source.albedoPath, source.normalPath, source.metallicRoughnessPath, source.occlusionPath, source.emissivePath}));
				material = *resolved;
			}
		}
		if (record.material)
		{
			AE_TRY(resolved, ResolveMaterial(*record.material));
			material = *resolved;
		}
		if (record.skinned)
		{
			if (!binding.model || binding.skin < 0)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser skinned record has no cooked skin"));
			}
			auto& skin = world.Emplace<SkinnedMeshComponent>(entity);
			skin.skinIndex = binding.skin;
			const auto clipCount = binding.model->data.animations.size();
			skin.clipIndex = clipCount ? std::min<std::uint32_t>(record.skinned->clipIndex, static_cast<std::uint32_t>(clipCount - 1)) : 0;
			skin.animTime = record.skinned->animTime;
			skin.playbackSpeed = record.skinned->playbackSpeed;
			skin.looping = record.skinned->looping;
		}
		world.Emplace<MeshSourceComponent>(entity, *record.mesh);
		world.Emplace<MaterialInstanceComponent>(entity, MaterialInstanceComponent{material});
		if (record.meshRenderer)
		{
			world.Emplace<MeshRendererComponent>(entity, MeshRendererComponent{record.meshRendererVisible, record.meshRendererCastShadows});
		}
		m_bindings.push_back(binding);
		return {};
	}

	Expected<void> WebModels::Extract(World& world, RenderFramePacket& packet)
	{
		std::erase_if(m_bindings, [&world](const auto& binding) { return !world.GetRegistry().valid(World::ToEntt(binding.entity)); });
		for (const auto& binding: m_bindings)
		{
			const auto entity = binding.entity;
			const auto* renderer = world.TryGet<MeshRendererComponent>(entity);
			if (!renderer || !renderer->visible || world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			MeshRenderInstance draw;
			draw.world = world.Get<TransformComponent>(entity).localToWorld;
			draw.mesh = binding.slot;
			draw.material = world.Get<MaterialInstanceComponent>(entity).asset;
			const std::array<TextureHandle, 5> handles{draw.material.albedoTex, draw.material.normalTex, draw.material.metallicRoughnessTex, draw.material.occlusionTex, draw.material.emissiveTex};
			for (unsigned i = 0; i < handles.size(); ++i)
			{
				if (handles[i].IsValid())
				{
					draw.textures[i] = m_textures.ResolveSlot(handles[i]);
				}
			}
			if (binding.model && binding.skin >= 0)
			{
				std::optional<std::uint32_t> clip;
				float time = 0;
				bool loop = true;
				if (const auto* skin = world.TryGet<SkinnedMeshComponent>(entity))
				{
					if (!binding.model->data.animations.empty())
					{
						clip = skin->clipIndex;
					}
					time = skin->animTime;
					loop = skin->looping;
				}
				AE_TRY(pose, assets::EvaluateModelPose(binding.model->data, clip, time, loop));
				draw.skinMatrices = pose->skins[binding.skin];
			}
			packet.render3D.meshes.push_back(std::move(draw));
		}
		packet.hasSceneDraws = packet.hasSceneDraws || !packet.render3D.meshes.empty();
		return {};
	}

	void WebModels::ClearEntities()
	{
		m_bindings.clear();
	}
} // namespace aether::app::web
