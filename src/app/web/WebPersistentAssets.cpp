#include "WebScene.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/GrassComponent.hpp"
#include "physics/PhysicsComponents.hpp"
#include "ui/UiComponents.hpp"
#include <unordered_set>

namespace aether::app::web
{
	namespace
	{
		bool AssetField(std::string_view type, std::string_view name)
		{
			return (type == "Tile Map" && name == "tilemap") || (type == "Particle Emitter" && name == "texture") || (type == "Audio Source" && name == "clipPath") || (type == "UI Text" && name == "font")
			       || (type == "Grass" && (name == "surface_mesh" || name == "exclusion_mesh"));
		}
	} // namespace

	Expected<std::uint64_t> WebScene::MeasurePersistentAssets(Entity root) const
	{
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid persistent streaming actor"));
		}
		std::uint64_t bytes = 4096;
		std::size_t count = 0;
		for (const auto raw: world.GetRegistry().storage<entt::entity>())
		{
			if (!world.GetRegistry().valid(raw))
			{
				continue;
			}
			const auto entity = World::FromEntt(raw);
			auto ancestor = entity;
			std::size_t depth = 0;
			while (ancestor.IsValid() && ancestor != root)
			{
				if (++depth > 65536 || !world.GetRegistry().valid(World::ToEntt(ancestor)))
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid persistent streaming actor hierarchy"));
				}
				const auto* hierarchy = world.TryGet<HierarchyComponent>(ancestor);
				ancestor = hierarchy ? hierarchy->parent : Entity{};
			}
			if (ancestor != root)
			{
				continue;
			}
			if (++count > 65536)
			{
				AE_UNEXPECTED(AetherError::Asset("Persistent streaming actor is too large"));
			}
			std::uint64_t paths = 0;
			if (const auto* mesh = world.TryGet<MeshSourceComponent>(entity))
			{
				paths += mesh->path.size() + mesh->primitiveKey.size();
			}
			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(entity))
			{
				paths += sprite->texturePath.size() + sprite->atlasPath.size();
			}
			if (const auto* animation = world.TryGet<SpriteAnimatorComponent>(entity))
			{
				paths += animation->animationPath.size();
			}
			if (const auto* collider = world.TryGet<ColliderComponent>(entity))
			{
				paths += collider->meshSource.size();
			}
			if (const auto* image = world.TryGet<ui::UIImage>(entity))
			{
				paths += image->texturePath.size();
			}
			if (world.Has<MeshSourceComponent>(entity) || world.Has<GrassComponent>(entity))
			{
				AE_TRY(materialBytes, m_models.MeasureSnapshotMaterialPaths(world, entity));
				paths += *materialBytes;
			}
			std::size_t fields = 0;
			for (const auto& type: reflect::ComponentTypes())
			{
				if (!type.tryGetRawConst)
				{
					continue;
				}
				const auto* component = type.tryGetRawConst(world, entity);
				if (!component)
				{
					continue;
				}
				for (const auto& field: type.fields)
				{
					if (!AssetField(type.name, field.name))
					{
						continue;
					}
					std::size_t strings = 0, rows = 0;
					if (!field.measureStorage || !field.measureStorage(component, strings, rows) || rows)
					{
						AE_UNEXPECTED(AetherError::Asset("Cannot measure persistent streaming asset field"));
					}
					paths += strings + type.name.size() + field.name.size();
					++fields;
				}
			}
			// Includes the temporary entity records, hierarchy worklist, TOML escaping and
			// string growth, plus the resulting document retained for change detection.
			constexpr std::uint64_t maximum = 256ull * 1024 * 1024;
			if (paths > maximum / 32 || fields > maximum / 1024)
			{
				AE_UNEXPECTED(AetherError::Asset("Persistent streaming assets are too large"));
			}
			const auto cost = 16384ull + 32 * paths + 1024 * fields;
			if (cost > maximum - bytes)
			{
				AE_UNEXPECTED(AetherError::Asset("Persistent streaming assets are too large"));
			}
			bytes += cost;
		}
		return bytes;
	}

	Expected<std::string> WebScene::CapturePersistentAssets(Entity root)
	{
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid persistent streaming actor"));
		}
		scene::SceneDescription document;
		document.kind = world.GetSceneKind();
		document.features = world.GetSceneFeatures();
		std::vector<Entity> queue{root};
		std::unordered_set<std::uint32_t> seen;
		for (std::size_t index = 0; index < queue.size(); ++index)
		{
			const auto entity = queue[index];
			if (!world.GetRegistry().valid(World::ToEntt(entity)) || !seen.insert(entity.id).second)
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid persistent streaming actor hierarchy"));
			}
			if (queue.size() > 65536)
			{
				AE_UNEXPECTED(AetherError::Asset("Persistent streaming actor is too large"));
			}
			scene::EntityRecord record;
			if (const auto* mesh = world.TryGet<MeshSourceComponent>(entity))
			{
				record.mesh = *mesh;
			}
			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(entity))
			{
				record.sprite = *sprite;
			}
			if (const auto* animation = world.TryGet<SpriteAnimatorComponent>(entity))
			{
				record.spriteAnimator = *animation;
			}
			if (const auto* collider = world.TryGet<ColliderComponent>(entity); collider && !collider->meshSource.empty())
			{
				scene::PhysicsRecord physics;
				physics.meshSource = collider->meshSource;
				record.physics = std::move(physics);
			}
			if (const auto* image = world.TryGet<ui::UIImage>(entity))
			{
				record.uiImage = scene::UIImageRecord{.color = image->color, .cornerRadius = image->cornerRadius, .pixelArt = image->pixelArt, .texturePath = image->texturePath};
			}
			if (record.mesh || world.Has<GrassComponent>(entity))
			{
				AE_TRY_VOID(m_models.CaptureSnapshotMaterial(world, entity, record));
			}
			for (const auto& type: reflect::ComponentTypes())
			{
				if (!type.tryGetRawConst)
				{
					continue;
				}
				const auto* component = type.tryGetRawConst(world, entity);
				if (!component)
				{
					continue;
				}
				scene::GenericComponent generic;
				generic.type = type.name;
				for (const auto& field: type.fields)
				{
					if (AssetField(type.name, field.name))
					{
						generic.fields.emplace_back(field.name, field.get(component));
					}
				}
				if (!generic.fields.empty())
				{
					record.reflected.push_back(std::move(generic));
				}
			}
			document.entities.push_back(std::move(record));
			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			{
				queue.insert(queue.end(), hierarchy->children.begin(), hierarchy->children.end());
			}
		}
		return scene::WriteToml(document);
	}
} // namespace aether::app::web
