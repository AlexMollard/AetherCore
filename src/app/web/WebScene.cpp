#include "WebScene.hpp"

#include <algorithm>
#include <emscripten.h>
#include "utils/Utf8.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "io/FileSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "animation/SpriteAnimationSystem.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/TransformUtils.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "WebAssetFormat.hpp"

namespace aether::app::web
{
	namespace
	{
		bool ValidName(std::string_view name)
		{
			return !name.empty() && name.size() <= 128 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
		}
	} // namespace

	std::string WebTextureSink::ResolvePath(std::string_view path) const
	{
		const auto dot = path.rfind('.');
		return dot == std::string_view::npos ? std::string(path) : std::string(path.substr(0, dot)) + ".texture";
	}

	Expected<TextureResource> WebTextureSink::Load(std::string_view path, TextureColorSpace)
	{
		auto bytes = io::FileSystem::ReadFile(path);
		WebTextureHeader header;
		if (!bytes || !ReadWebTextureHeader(*bytes, header) || m_next_slot >= Capacity())
		{
			error = "Missing, invalid or oversized browser texture, or texture capacity exceeded: " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		const auto* pixels = reinterpret_cast<const std::uint8_t*>(bytes->data() + sizeof(header));
		if (!m_graphics.UploadTexture(m_next_slot, header.width, header.height, {pixels, static_cast<size_t>(header.pixelBytes)}))
		{
			error = "WebGPU texture upload failed: " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		return TextureResource(m_next_slot++);
	}

	WebScene::WebScene(World& sourceWorld, webgpu::WebGpuDevice& graphics)
	      : world(sourceWorld), m_sink(graphics), m_textures(m_sink), m_graphics(graphics)
	{
		m_textures.InitializeDefault(TextureResource(0));
		m_textures.InitializeWhite(TextureResource(1));
		m_sprite_system.Initialize(m_textures, m_sprites);
		m_tile_system.Initialize(m_textures, m_sprites, m_tiles);
		auto physics = std::make_unique<Physics2DSystem>();
		physics->SetTileAssets(&m_tiles);
		world.RegisterSystem(std::move(physics));
		world.RegisterSystem(std::make_unique<SpriteAnimationSystem>(m_sprites));
	}

	WebScene::~WebScene()
	{
		world.UnregisterSystem("SpriteAnimationSystem");
		world.UnregisterSystem("Physics2DSystem");
		m_tile_system.Shutdown();
		m_sprite_system.Shutdown();
		m_textures.ReleaseAll();
	}

	Expected<std::vector<Entity>> WebScene::Apply(const scene::SceneDescription& description)
	{
		// This profile supports portable components only. Never silently discard a 3D record.
		if (description.version > scene::kSceneFormatVersion || description.entities.size() > 65536 || !description.prefabInstances.empty() || !description.includes.empty() || !description.lights.empty() || description.environment)
		{
			AE_UNEXPECTED(AetherError::Asset("Unsupported scene version, size or feature in webgpu-2d-v1"));
		}
		for (const auto& record: description.entities)
		{
			if (record.mesh || record.meshRenderer || record.material || record.skinned || record.physics || record.effect || record.joint || !record.scriptJoints.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Unsupported 3D entity in browser scene: " + record.name));
			}
			for (const auto& generic: record.reflected)
			{
				if (generic.type != "Rigid Body 2D" && generic.type != "Collider 2D" && generic.type != "Tile Map"
				        && (generic.type != "UI Canvas" && generic.type != "UI Rect" && generic.type != "UI Text" && generic.type != "UI Image" && generic.type != "UI Selectable" && generic.type != "UI Slider" && generic.type != "UI Toggle"
				                && generic.type != "UI Button" && generic.type != "UI Progress Bar"))
				{
					AE_UNEXPECTED(AetherError::Asset("Unsupported browser component: " + generic.type));
				}
			}
		}
		std::vector<unsigned char> parents(description.entities.size());
		for (size_t index = 0; index < parents.size(); ++index)
		{
			if (parents[index] == 2)
			{
				continue;
			}
			int current = static_cast<int>(index);
			while (current >= 0)
			{
				if (static_cast<size_t>(current) >= parents.size())
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid browser scene parent index"));
				}
				if (parents[current] != 0)
				{
					break;
				}
				parents[current] = 1;
				current = description.entities[current].parentIndex;
			}
			if (current >= 0 && parents[current] == 1)
			{
				AE_UNEXPECTED(AetherError::Asset("Cyclic browser scene hierarchy"));
			}
			current = static_cast<int>(index);
			while (current >= 0 && parents[current] == 1)
			{
				parents[current] = 2;
				current = description.entities[current].parentIndex;
			}
		}
		std::vector<Entity> created;
		for (const auto& record: description.entities)
		{
			const auto entity = world.Create();
			created.push_back(entity);
			world.Emplace<NameComponent>(entity, NameComponent{.name = record.name});
			if (record.nodeId)
			{
				world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{record.nodeId});
			}
			for (const auto& tag: record.tags)
			{
				const auto id = TagCreate(tag);
				if (id == UINT32_MAX)
				{
					AE_UNEXPECTED(AetherError::Asset("Browser tag capacity exceeded"));
				}
				TagAdd(&world, entity.id, id);
			}
			world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform(record.position, record.eulerDeg, record.scale)});
			if (record.disabled)
			{
				world.Emplace<DisabledComponent>(entity);
			}
			if (record.sprite)
			{
				if (!record.sprite->atlasPath.empty())
				{
					AE_TRY(atlas, m_sprites.LoadAtlas(record.sprite->atlasPath));
					if (!(*atlas)->Find(record.sprite->spriteId))
					{
						AE_UNEXPECTED(AetherError::Asset("Browser sprite id is missing from its atlas"));
					}
				}
				world.Emplace<SpriteRendererComponent>(entity, *record.sprite);
			}
			if (record.spriteAnimator)
			{
				AE_TRY(animation, m_sprites.LoadAnimation(record.spriteAnimator->animationPath));
				AE_TRY(atlas, m_sprites.LoadAtlas((*animation)->atlasPath));
				if ((*animation)->frames.empty())
				{
					AE_UNEXPECTED(AetherError::Asset("Browser sprite animation has no frames"));
				}
				for (const auto& frame: (*animation)->frames)
				{
					if (!(*atlas)->Find(frame.spriteId))
					{
						AE_UNEXPECTED(AetherError::Asset("Browser animation frame references a missing sprite"));
					}
				}
				world.Emplace<SpriteAnimatorComponent>(entity, *record.spriteAnimator);
			}
			if (record.camera)
			{
				if (record.camera->projection != CameraProjection::Orthographic)
				{
					AE_UNEXPECTED(AetherError::Asset("webgpu-2d-v1 requires orthographic cameras"));
				}
				world.Emplace<CameraComponent>(entity, *record.camera);
				if (record.mainCamera)
				{
					world.Emplace<MainCameraComponent>(entity);
				}
			}
			for (const auto& generic: record.reflected)
			{
				const auto* type = reflect::FindComponentType(generic.type);
				if (!type || !type->emplaceDefault)
				{
					AE_UNEXPECTED(AetherError::Asset("Unknown browser component: " + generic.type));
				}
				auto* component = type->emplaceDefault(world, entity);
				for (const auto& [name, value]: generic.fields)
				{
					if (const auto* field = type->FindField(name))
					{
						field->set(component, value);
					}
				}
				if (generic.type == "Tile Map")
				{
					const auto& tileComponent = world.Get<TileMapComponent>(entity);
					AE_TRY(map, m_tiles.LoadTileMap(tileComponent.tilemapPath));
					AE_TRY(set, m_tiles.LoadTileSet((*map)->tileSetPath));
					for (const auto& tile: (*set)->tiles)
					{
						AE_TRY(atlas, m_sprites.LoadAtlas(tile.atlasPath));
						if (!(*atlas)->Find(tile.spriteId))
						{
							AE_UNEXPECTED(AetherError::Asset("Browser tile references a missing sprite"));
						}
					}
				}
				if (type->postSet)
				{
					type->postSet(world, entity);
				}
			}
			if (record.uiImage)
			{
				auto& image = world.EmplaceOrReplace<ui::UIImage>(entity);
				image.color = record.uiImage->color;
				image.texturePath = record.uiImage->texturePath;
				image.pixelArt = record.uiImage->pixelArt;
			}
		}
		for (size_t i = 0; i < created.size(); ++i)
		{
			const auto& record = description.entities[i];
			if (record.parentIndex >= 0)
			{
				if (static_cast<size_t>(record.parentIndex) >= created.size())
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid browser scene parent index"));
				}
				ecs::SetParent(world, created[i], created[record.parentIndex]);
			}
			if (record.joint2D)
			{
				auto joint = *record.joint2D;
				if (record.joint2DTargetIndex >= 0 && static_cast<size_t>(record.joint2DTargetIndex) < created.size())
				{
					joint.target = created[record.joint2DTargetIndex];
				}
				world.Emplace<Joint2DComponent>(created[i], joint);
			}
		}
		return created;
	}

	Expected<std::vector<WebScriptRecord>> WebScene::Load(std::string_view name)
	{
		if (!ValidName(name))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid scene name"));
		}
		AE_TRY(text, io::FileSystem::ReadFileText("project://scenes/" + std::string(name) + ".scene.toml"));
		auto description = scene::ParseToml(*text);
		if (!description)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser scene TOML"));
		}
		const auto roots = world.Roots();
		for (const auto root: roots)
		{
			world.Destroy(root);
		}
		world.SetSceneKind(description->kind);
		world.SetSceneFeatures(description->features);
		m_tile_system.InvalidateAll();
		AE_TRY(created, Apply(*description));
		std::vector<WebScriptRecord> scripts;
		for (size_t i = 0; i < created->size(); ++i)
		{
			for (const auto& script: description->entities[i].scripts)
			{
				auto resolved = script;
				resolved.properties = scene::detail::ScriptPropsFromSceneRefs(script.properties, *created);
				for (const auto& [property, node]: script.nodeRefs)
				{
					const auto found = std::find_if(description->entities.begin(), description->entities.end(), [node](const auto& record) { return record.nodeId == node; });
					if (found == description->entities.end())
					{
						AE_UNEXPECTED(AetherError::Asset("Unresolved browser script node reference"));
					}
					resolved.properties[property].i64 = (*created)[std::distance(description->entities.begin(), found)].id;
				}
				scripts.push_back({(*created)[i], std::move(resolved)});
			}
		}
		static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem"))->FlushPendingOnly(world);
		return scripts;
	}

	Expected<Entity> WebScene::Instantiate(std::string_view name)
	{
		if (!ValidName(name))
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid prefab name"));
		}
		AE_TRY(text, io::FileSystem::ReadFileText("project://assets/prefabs/" + std::string(name) + ".prefab.toml"));
		auto description = scene::ParseToml(*text);
		if (!description)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser prefab TOML"));
		}
		for (const auto& record: description->entities)
		{
			if (!record.scripts.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Scripted browser prefabs are not supported yet"));
			}
		}
		AE_TRY(created, Apply(*description));
		if (created->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Empty browser prefab"));
		}
		return created->front();
	}

	Expected<void> WebScene::Extract(RenderFramePacket& packet, unsigned width, unsigned height, float elapsed)
	{
		packet.hasCameraData = true;
		packet.view = glm::mat4(1);
		packet.proj = glm::mat4(1);
		for (const auto entity: world.View<CameraComponent, TransformComponent, MainCameraComponent>())
		{
			const auto& camera = world.GetRegistry().get<CameraComponent>(entity);
			const auto& transform = world.GetRegistry().get<TransformComponent>(entity);
			const float halfHeight = camera.orthographicHeight * 0.5f;
			const float halfWidth = halfHeight * float(width) / std::max(height, 1u);
			packet.proj = glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, camera.nearPlane, camera.farPlane);
			packet.view = glm::inverse(transform.localToWorld);
			break;
		}
		packet.proj[1][1] *= -1;
		m_sprite_system.Extract(world, packet.render2D);
		const auto beforeTiles = packet.render2D.sprites.size();
		m_tile_system.Extract(world, {}, elapsed, packet.render2D);
		EM_ASM({ aetherRuntime.tileSprites = $0; }, packet.render2D.sprites.size() - beforeTiles);
		ui::ResolveCanvases(world, {width, height});
		AE_TRY_VOID(ExtractUi(packet, width, height));
		Finalize2DFrame(packet.render2D);
		if (!m_sink.error.empty())
		{
			AE_UNEXPECTED(AetherError::Asset(m_sink.error));
		}
		return {};
	}
} // namespace aether::app::web
