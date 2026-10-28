#include "WebModels.hpp"

#include "WebScene.hpp"
#include "assets/ModelPose.hpp"
#include "io/FileSystem.hpp"
#include "material/MaterialSerializer.hpp"
#include "material/MaterialOverrides.hpp"
#include "mesh/PrimitiveMeshData.hpp"
#include "mesh/GrassMeshData.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "scene/SceneSerializerDetail.hpp"
#include "scene/Hierarchy.hpp"
#include "gpu/WebMaterialShader.hpp"
#include "rendering/SceneEffectExtraction.hpp"
#include "scene/GrassBenderExtraction.hpp"
#include <set>

namespace aether::app::web
{
	WebModels::WebModels(webgpu::WebGpuDevice& graphics, TextureRegistry& textures, WebTextureSink& sink)
	      : m_graphics(graphics), m_textures(textures), m_sink(sink)
	{
	}

	Expected<std::uint32_t> WebModels::ResolveShader(std::string_view uri)
	{
		if (uri.empty() || uri == "shaders://gltf_mesh.spv" || uri == "shaders://plasma.spv" || uri == "shaders://molten.spv")
		{
			return UINT32_MAX;
		}
		const std::string key(uri);
		if (const auto found = m_shaders.find(key); found != m_shaders.end())
		{
			return found->second;
		}
		if (!uri.starts_with("shaders://") || !uri.ends_with(".spv") || m_shaders.size() >= 128)
		{
			AE_UNEXPECTED(AetherError::Asset("WEB_SHADER_URI: expected shaders://<relative>.spv and at most 128 custom materials: " + key));
		}
		const auto relative = uri.substr(10, uri.size() - 14);
		if (relative.empty() || relative.find('\\') != std::string_view::npos || relative.find(':') != std::string_view::npos || relative.find('\0') != std::string_view::npos)
		{
			AE_UNEXPECTED(AetherError::Asset("WEB_SHADER_URI: invalid relative shader path: " + key));
		}
		for (std::size_t start = 0; start <= relative.size();)
		{
			const auto end = relative.find('/', start);
			const auto segment = relative.substr(start, end == std::string_view::npos ? end : end - start);
			if (segment.empty() || segment == "." || segment == "..")
			{
				AE_UNEXPECTED(AetherError::Asset("WEB_SHADER_URI: shader path escapes its package: " + key));
			}
			if (end == std::string_view::npos)
			{
				break;
			}
			start = end + 1;
		}
		const std::string cooked = "project://assets/shaders/" + std::string(relative) + ".wshader";
		AE_TRY(bytes, io::FileSystem::ReadFile(cooked));
		const auto source = gpu::DecodeWebMaterialShader(*bytes);
		if (!source)
		{
			AE_UNEXPECTED(AetherError::Asset(source.error() + ": " + cooked));
		}
		const auto slot = static_cast<std::uint32_t>(m_shaders.size());
		if (!m_graphics.UploadShader(slot, *source))
		{
			AE_UNEXPECTED(AetherError::Asset("WEB_SHADER_PIPELINE: " + key + ": " + m_graphics.MeshUploadError()));
		}
		m_shaders.emplace(key, slot);
		return slot;
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
				AE_TRY(slot, ResolveShader(preset.shaderVfsPath));
				asset.templateDesc.shaderVfsPath = preset.shaderVfsPath == "shaders://plasma.spv"   ? "shaders://plasma.spv"
				                                   : preset.shaderVfsPath == "shaders://molten.spv" ? "shaders://molten.spv"
				                                                                                    : std::string_view(m_shaders.find(preset.shaderVfsPath)->first);
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
			if (world.Has<GrassComponent>(entity))
			{
				if (record.skinned)
				{
					AE_UNEXPECTED(AetherError::Asset("Browser grass cannot have a skin: " + record.name));
				}
				MaterialAsset material;
				material.doubleSided = true;
				material.modulateVertexColor = true;
				material.foliage = false;
				if (record.material)
				{
					AE_TRY(resolved, ResolveMaterial(*record.material));
					material = *resolved;
				}
				world.EmplaceOrReplace<MaterialInstanceComponent>(entity, MaterialInstanceComponent{material});
				world.EmplaceOrReplace<MeshRendererComponent>(entity, MeshRendererComponent{
				        record.meshRenderer ? record.meshRendererVisible : true,
				        record.meshRenderer ? record.meshRendererCastShadows : false});
				return {};
			}
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
				const auto slot = TakeSlot();
				m_primitives.emplace(name, slot);
				m_pending_uploads.push_back({name, slot, 0, false});
			}
			binding.slot = m_primitives.at(name);
			const auto primitive = *scene::detail::PrimitiveFromName(name);
			binding.localSphere = glm::vec4(0.0f, 0.0f, 0.0f, primitive == PrimitiveMesh::Cube ? 0.8660254f : primitive == PrimitiveMesh::Sphere ? 0.5f : 0.70710678f);
		}
		else
		{
			const auto& path = record.mesh->path;
			if (!m_models.contains(path))
			{
				AE_TRY(data, assets::GltfAsset::LoadFromVfsPath(path));
				AE_TRY(pose, assets::EvaluateModelPose(*data));
				Model model{std::move(*data), {}, {}};
				for (const auto& animation: model.data.animations)
				{
					float duration = 0.0f;
					for (const auto& channel: animation.channels)
					{
						if (!channel.times.empty())
						{
							duration = std::max(duration, channel.times.back());
						}
					}
					model.clipDurations.push_back(duration);
				}
				for (std::size_t i = 0; i < model.data.primitives.size(); ++i)
				{
					const auto slot = TakeSlot();
					model.slots.push_back(slot);
					m_pending_uploads.push_back({path, slot, i, true});
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
			binding.localSphere = glm::vec4(primitive.sphereCenter[0], primitive.sphereCenter[1], primitive.sphereCenter[2], primitive.sphereRadius);
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
					AE_TRY(slot, ResolveShader(source.shaderVfsPath));
					material.templateDesc.shaderVfsPath = source.shaderVfsPath == "shaders://plasma.spv"   ? "shaders://plasma.spv"
					                                      : source.shaderVfsPath == "shaders://molten.spv" ? "shaders://molten.spv"
					                                                                                       : std::string_view(m_shaders.find(source.shaderVfsPath)->first);
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
		packet.grassBenders = ExtractGrassBenders(world, glm::vec3(packet.cameraWorldPos));
		std::erase_if(m_bindings, [&world](const auto& binding) {
			return binding.grass || !world.GetRegistry().valid(World::ToEntt(binding.entity));
		});
		m_graphics.CollectRetiredMeshes(m_free_grass_slots);
		const auto virtualCount = static_cast<std::size_t>(std::count_if(m_pending_uploads.begin(), m_pending_uploads.end(), [](const auto& pending) { return pending.slot >= 1024; }));
		std::size_t pendingBytes = 0;
		for (const auto& pending: m_pending_uploads)
		{
			if (pending.model)
			{
				const auto& primitive = m_models.at(pending.source).data.primitives[pending.primitive];
				pendingBytes += primitive.vertices.size() * sizeof(MeshVertex) + primitive.indices.size() * sizeof(std::uint32_t);
			}
			else
			{
				const auto mesh = GeneratePrimitiveMesh(*scene::detail::PrimitiveFromName(pending.source));
				if (!mesh) { AE_UNEXPECTED(AetherError::Asset("Browser primitive generation failed: " + pending.source)); }
				pendingBytes += mesh->vertices.size() * sizeof(MeshVertex) + mesh->indices.size() * sizeof(std::uint32_t);
			}
		}
		if (!m_graphics.MeshRetirementPending())
		{
			const auto neededSlots = virtualCount > m_free_grass_slots.size() ? virtualCount - m_free_grass_slots.size() : 0;
			const auto available = m_graphics.AvailableMeshUploadBytes();
			const auto neededBytes = pendingBytes > available ? pendingBytes - available : 0;
			std::size_t freedBytes = 0;
			std::vector<std::uint32_t> slots;
			for (auto& [key, resident]: m_grass)
			{
				for (auto it = resident.chunks.begin(); it != resident.chunks.end() && (slots.size() < neededSlots || freedBytes < neededBytes);)
				{
					if (it->second.slot == UINT32_MAX) { ++it; continue; }
					slots.push_back(it->second.slot);
					freedBytes += it->second.bytes;
					it = resident.chunks.erase(it);
				}
				if (slots.size() >= neededSlots && freedBytes >= neededBytes) { break; }
			}
			if (slots.size() < neededSlots) { AE_UNEXPECTED(AetherError::Asset("Browser model mesh slot capacity exceeded")); }
			if (freedBytes < neededBytes) { AE_UNEXPECTED(AetherError::Asset("Browser model meshes exceed the 64 MiB upload budget")); }
			RetireGrass(std::move(slots));
		}
		AE_TRY_VOID(FlushUploads());
		if (m_pending_uploads.empty()) { m_initial_ready = true; }
		struct Root { Entity entity; std::uint64_t field; std::vector<MeshGen::GrassChunkKey> chunks; };
		std::vector<Root> roots;
		for (auto& [key, resident]: m_grass) { resident.desired.clear(); }
		auto grassView = world.GetRegistry().view<const GrassComponent, const TransformComponent>();
		for (const auto raw: grassView)
		{
			const auto entity = World::FromEntt(raw);
			const auto& grass = grassView.get<const GrassComponent>(raw);
			if (!grass.enabled || world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity)) { continue; }
			if (const auto* renderer = world.TryGet<MeshRendererComponent>(entity); renderer && !renderer->visible) { continue; }
			if (!world.Has<MaterialInstanceComponent>(entity))
			{
				MaterialAsset material;
				material.doubleSided = true;
				material.modulateVertexColor = true;
				world.Emplace<MaterialInstanceComponent>(entity, MaterialInstanceComponent{material});
			}
			if (!world.Has<MeshRendererComponent>(entity))
			{
				world.Emplace<MeshRendererComponent>(entity, MeshRendererComponent{true, false});
			}
			const auto key = MeshGen::GrassMeshKey(grass);
			auto it = m_grass.find(key);
			if (it == m_grass.end())
			{
				AE_TRY(field, MeshGen::GrassField::Load(grass));
				it = m_grass.emplace(key, GrassResident{std::move(*field), {}, {}}).first;
			}
			const auto& worldMatrix = grassView.get<const TransformComponent>(raw).localToWorld;
			const auto determinant = glm::determinant(glm::mat3(worldMatrix));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser grass root has a singular transform"));
			}
			const auto inverse = glm::inverse(worldMatrix);
			const auto local = inverse * packet.cameraWorldPos;
			const glm::mat3 linear(inverse);
			float normSquared = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				float row = 0.0f;
				for (int j = 0; j < 3; ++j) { row += std::abs(glm::dot(linear[i], linear[j])); }
				normSquared = std::max(normSquared, row);
			}
			const float localDistanceScale = std::sqrt(normSquared);
			AE_TRY_VOID(it->second.field.SelectChunks({local.x, local.z}, m_selected_grass, localDistanceScale));
			roots.push_back({entity, key, m_selected_grass});
			auto& desired = it->second.desired;
			desired.insert(desired.end(), m_selected_grass.begin(), m_selected_grass.end());
		}
		std::vector<std::uint32_t> retiring;
		for (auto it = m_grass.begin(); it != m_grass.end();)
		{
			auto& resident = it->second;
			const std::set<MeshGen::GrassChunkKey> wanted(resident.desired.begin(), resident.desired.end());
			for (auto chunk = resident.chunks.begin(); chunk != resident.chunks.end();)
			{
				if (wanted.contains(chunk->first)) { ++chunk; continue; }
				if (chunk->second.slot != UINT32_MAX) { retiring.push_back(chunk->second.slot); }
				chunk = resident.chunks.erase(chunk);
			}
			if (resident.desired.empty()) { it = m_grass.erase(it); }
			else { ++it; }
		}
		RetireGrass(std::move(retiring));
		// Upload only a bounded number per frame; existing meshes remain drawable.
		std::size_t uploaded = 0;
		if (m_pending_uploads.empty())
		{
			for (auto& [key, resident]: m_grass)
			{
				for (const auto chunkKey: resident.desired)
				{
					if (uploaded >= 4) { break; }
					if (resident.chunks.contains(chunkKey)) { continue; }
					++uploaded;
					AE_TRY(chunk, resident.field.GenerateChunk(chunkKey));
					if (chunk->mesh.vertices.empty() || chunk->mesh.indices.empty())
					{
						resident.chunks.emplace(chunkKey, GrassSlot{UINT32_MAX, chunk->origin});
						continue;
					}
					const auto bytes = chunk->mesh.vertices.size() * sizeof(MeshVertex) + chunk->mesh.indices.size() * sizeof(std::uint32_t);
					m_graphics.CollectRetiredMeshes(m_free_grass_slots);
					if (!m_graphics.HasMeshUploadCapacity(bytes))
					{
						if (m_graphics.MeshRetirementPending()) { break; }
						AE_UNEXPECTED(AetherError::Asset("Browser grass desired residency exceeds the 64 MiB mesh upload budget"));
					}
					if (m_free_grass_slots.empty() && m_next_slot >= 1024)
					{
						if (m_graphics.MeshRetirementPending()) { break; }
						AE_UNEXPECTED(AetherError::Asset("Browser grass desired residency exceeds the 1024 mesh slot limit"));
					}
					const auto slot = TakeSlot();
					if (!m_graphics.UploadMesh(slot, chunk->mesh.vertices, chunk->mesh.indices))
					{
						m_free_grass_slots.push_back(slot);
						AE_UNEXPECTED(AetherError::Asset("Browser grass upload failed: " + m_graphics.MeshUploadError()));
					}
					resident.chunks.emplace(chunkKey, GrassSlot{slot, chunk->origin, bytes});
				}
				if (uploaded >= 4) { break; }
			}
		}
		m_streaming_pending = false;
		for (const auto& [key, resident]: m_grass)
		{
			for (const auto chunk: resident.desired)
			{
				if (!resident.chunks.contains(chunk)) { m_streaming_pending = true; break; }
			}
			if (m_streaming_pending) { break; }
		}
		for (const auto& root: roots)
		{
			const auto& resident = m_grass.at(root.field);
			for (const auto chunkKey: root.chunks)
			{
				const auto chunk = resident.chunks.find(chunkKey);
				if (chunk == resident.chunks.end() || chunk->second.slot == UINT32_MAX) { continue; }
				Binding binding{root.entity, chunk->second.slot};
				binding.grass = true;
				binding.origin = chunk->second.origin;
				binding.grassFade = resident.field.DistanceFade(chunkKey.lod);
				m_bindings.push_back(binding);
			}
		}
		std::vector<BlobShadowSource> blobs;
		for (const auto& binding: m_bindings)
		{
			if (!binding.grass && std::any_of(m_pending_uploads.begin(), m_pending_uploads.end(), [&binding](const auto& pending) { return pending.slot == binding.slot; }))
			{
				continue;
			}
			const auto entity = binding.entity;
			if (binding.grass && !world.Get<GrassComponent>(entity).enabled)
			{
				continue;
			}
			const auto* renderer = world.TryGet<MeshRendererComponent>(entity);
			if (!renderer || !renderer->visible || world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			MeshRenderInstance draw;
			draw.world = world.Get<TransformComponent>(entity).localToWorld;
			if (binding.grass)
			{
				draw.world = glm::translate(draw.world, binding.origin);
			}
			draw.mesh = binding.slot;
			draw.castShadows = renderer->castShadows;
			draw.material = world.Get<MaterialInstanceComponent>(entity).asset;
			if (binding.grass)
			{
				const auto& grass = world.Get<GrassComponent>(entity);
				draw.material.grass = true;
				draw.material.grassWindStrength = grass.windStrength;
				draw.material.modulateVertexColor = true;
				draw.material.doubleSided = true;
				draw.material.foliage = false;
				draw.material.alphaBlend = false;
				draw.material.alphaMask = false;
				draw.material.additiveBlend = false;
				draw.material.distanceFade = binding.grassFade;
			}
			const auto uri = draw.material.templateDesc.shaderVfsPath;
			draw.proceduralKind = uri == "shaders://plasma.spv" ? 1 : uri == "shaders://molten.spv" ? 2 : 0;
			if (const auto* effect = world.TryGet<EffectRefComponent>(entity))
			{
				if (effect->name == "plasma")
				{
					draw.proceduralKind = 1;
				}
				else if (effect->name == "molten")
				{
					draw.proceduralKind = 2;
				}
				else
				{
					AE_UNEXPECTED(AetherError::Asset("WEB_EFFECT_UNSUPPORTED: " + effect->name));
				}
			}
			if (draw.proceduralKind == 2)
			{
				draw.effectTint = glm::vec4(1.0f, 0.35f, 0.05f, 1.0f);
			}
			if (const auto* effect = world.TryGet<EffectParamsComponent>(entity))
			{
				draw.effectTint = effect->params.tint;
				draw.effectParams = glm::vec4(effect->params.speed, effect->params.scale, effect->params.intensity, 0.0f);
			}
			AE_TRY(shader, ResolveShader(draw.material.templateDesc.shaderVfsPath));
			draw.customShaderSlot = *shader;
			if (!binding.grass && draw.material.objectLit && !draw.material.alphaBlend)
			{
				const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
				const auto actor = hierarchy && hierarchy->parent.IsValid() ? hierarchy->parent : entity;
				blobs.push_back({actor.id, TransformEffectBoundingSphere(binding.localSphere, draw.world), draw.material.character});
			}
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
				assets::PoseFade fade;
				if (const auto* skin = world.TryGet<SkinnedMeshComponent>(entity))
				{
					if (!binding.model->data.animations.empty())
					{
						clip = skin->clipIndex;
						// Native: the outgoing clip is sampled only while its index is a real clip.
						if (skin->fadeWeight > 0.0f && skin->fadeClipIndex < binding.model->data.animations.size())
						{
							fade = {skin->fadeClipIndex, skin->fadeTime, skin->fadeLooping, skin->fadeWeight};
						}
					}
					time = skin->animTime;
					loop = skin->looping;
				}
				AE_TRY(pose, assets::EvaluateModelPose(binding.model->data, clip, time, loop, &fade));
				draw.skinMatrices = pose->skins[binding.skin];
			}
			packet.render3D.meshes.push_back(std::move(draw));
		}
		packet.hasSceneDraws = packet.hasSceneDraws || !packet.render3D.meshes.empty();
		packet.blobCharacterCount = static_cast<std::uint32_t>(ExtractBlobShadows(blobs, glm::vec3(packet.cameraWorldPos), 64, packet.blobShadows));
		return {};
	}

	const WebModels::Model* WebModels::FindModel(Entity entity) const
	{
		for (const auto& binding: m_bindings)
		{
			if (binding.entity.id == entity.id)
			{
				return binding.model;
			}
		}
		return nullptr;
	}

	std::uint32_t WebModels::Catalog::ClipCount(Entity animator) const
	{
		const auto* model = m_owner.FindModel(animator);
		return model ? static_cast<std::uint32_t>(model->data.animations.size()) : 0u;
	}

	std::string_view WebModels::Catalog::ClipName(Entity animator, std::uint32_t index) const
	{
		const auto* model = m_owner.FindModel(animator);
		return model && index < model->data.animations.size() ? std::string_view(model->data.animations[index].name) : std::string_view{};
	}

	float WebModels::Catalog::ClipDuration(Entity animator, std::uint32_t index) const
	{
		const auto* model = m_owner.FindModel(animator);
		return model && index < model->clipDurations.size() ? model->clipDurations[index] : 0.0f;
	}

	bool WebModels::ClipDurations(World& world, Entity entity, const SkinnedMeshComponent& skin, float& clip, float& fade) const
	{
		for (const auto& binding: m_bindings)
		{
			if (binding.entity.id != entity.id)
			{
				continue;
			}
			if (!binding.model || binding.model->clipDurations.empty())
			{
				return false;
			}
			const auto& durations = binding.model->clipDurations;
			clip = skin.clipIndex < durations.size() ? durations[skin.clipIndex] : 0.0f;
			fade = skin.fadeClipIndex < durations.size() ? durations[skin.fadeClipIndex] : 0.0f;
			return true;
		}
		(void) world;
		return false;
	}


	std::uint32_t WebModels::TakeSlot()
	{
		m_graphics.CollectRetiredMeshes(m_free_grass_slots);
		if (m_free_grass_slots.empty()) { return m_next_slot < 1024 ? m_next_slot++ : m_next_virtual_slot++; }
		const auto slot = m_free_grass_slots.back();
		m_free_grass_slots.pop_back();
		return slot;
	}

	Expected<void> WebModels::FlushUploads()
	{
		if (m_graphics.MeshRetirementPending()) { return {}; }
		m_graphics.CollectRetiredMeshes(m_free_grass_slots);
		const auto virtualCount = std::count_if(m_pending_uploads.begin(), m_pending_uploads.end(), [](const auto& pending) { return pending.slot >= 1024; });
		if (static_cast<std::size_t>(virtualCount) > m_free_grass_slots.size()) { return {}; }
		for (auto& pending: m_pending_uploads)
		{
			if (pending.slot < 1024) { continue; }
			const auto oldSlot = pending.slot;
			pending.slot = TakeSlot();
			for (auto& binding: m_bindings) { if (binding.slot == oldSlot) { binding.slot = pending.slot; } }
			for (auto& [name, slot]: m_primitives) { if (slot == oldSlot) { slot = pending.slot; } }
			for (auto& [name, model]: m_models)
			{
				for (auto& slot: model.slots) { if (slot == oldSlot) { slot = pending.slot; } }
			}
		}
		for (const auto& pending: m_pending_uploads)
		{
			if (pending.model)
			{
				const auto& primitive = m_models.at(pending.source).data.primitives[pending.primitive];
				if (!m_graphics.UploadMesh(pending.slot, primitive.vertices, primitive.indices))
				{
					AE_UNEXPECTED(AetherError::Asset("Browser model upload failed: " + pending.source + ": " + m_graphics.MeshUploadError()));
				}
			}
			else
			{
				const auto mesh = GeneratePrimitiveMesh(*scene::detail::PrimitiveFromName(pending.source));
				if (!mesh || !m_graphics.UploadMesh(pending.slot, mesh->vertices, mesh->indices))
				{
					AE_UNEXPECTED(AetherError::Asset("Browser primitive upload failed: " + pending.source + ": " + m_graphics.MeshUploadError()));
				}
			}
		}
		m_pending_uploads.clear();
		return {};
	}
	void WebModels::RetireGrass(std::vector<std::uint32_t> slots)
	{
		if (slots.empty()) { return; }
		m_graphics.RetireMeshes(std::move(slots));
	}

	void WebModels::ClearEntities()
	{
		m_bindings.clear();
		m_initial_ready = false;
		m_streaming_pending = false;
		std::vector<std::uint32_t> slots;
		std::set<std::uint32_t> pendingSlots;
		for (const auto& pending: m_pending_uploads)
		{
			pendingSlots.insert(pending.slot);
			if (pending.slot < 1024) { m_free_grass_slots.push_back(pending.slot); }
		}
		m_pending_uploads.clear();
		for (const auto& [path, model]: m_models)
		{
			for (const auto slot: model.slots)
			{
				if (!pendingSlots.contains(slot)) { slots.push_back(slot); }
			}
		}
		for (const auto& [name, slot]: m_primitives)
		{
			if (!pendingSlots.contains(slot)) { slots.push_back(slot); }
		}
		m_models.clear();
		m_primitives.clear();
		for (const auto& [key, resident]: m_grass)
		{
			for (const auto& [chunk, data]: resident.chunks)
			{
				if (data.slot != UINT32_MAX) { slots.push_back(data.slot); }
			}
		}
		m_grass.clear();
		RetireGrass(std::move(slots));
	}
} // namespace aether::app::web
