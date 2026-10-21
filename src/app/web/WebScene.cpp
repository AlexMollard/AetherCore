#include "WebScene.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
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
#include "scene/SceneDocumentExpansion.hpp"
#include "passes/TonemapDefs.hpp"
#include "scene/LightComponents.hpp"

namespace aether::app::web
{
	std::string WebScene::ProjectPath(std::string_view kind, std::string_view name, std::string_view suffix)
	{
		const std::string key(kind);
		const auto length = EM_ASM_INT({ return lengthBytesUTF8(globalThis.aetherProject ?.paths ?.[UTF8ToString($0)] || (UTF8ToString($0) == 'prefabs' ? 'assets/prefabs' : 'scenes')); }, key.c_str());
		std::string base(length, '\0');
		EM_ASM({ stringToUTF8(globalThis.aetherProject ?.paths ?.[UTF8ToString($0)] || (UTF8ToString($0) == 'prefabs' ? 'assets/prefabs' : 'scenes'), $1, $2); }, key.c_str(), base.data(), base.size() + 1);
		return "project://" + base + "/" + std::string(name) + std::string(suffix);
	}

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

	Expected<TextureResource> WebTextureSink::Load(std::string_view path, TextureColorSpace colorSpace)
	{
		auto bytes = io::FileSystem::ReadFile(path);
		WebTextureHeader header;
		if (!bytes || !ReadWebTextureHeader(*bytes, header) || m_next_slot >= Capacity())
		{
			error = "Missing, invalid or oversized browser texture, or texture capacity exceeded: " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		const auto* pixels = reinterpret_cast<const std::uint8_t*>(bytes->data() + sizeof(header));
		if (!m_graphics.UploadTexture(m_next_slot, header.width, header.height, {pixels, static_cast<size_t>(header.pixelBytes)}, colorManaged && colorSpace == TextureColorSpace::Srgb, colorManaged))
		{
			error = "WebGPU texture upload failed: " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		return TextureResource(m_next_slot++);
	}

	WebScene::WebScene(World& sourceWorld, webgpu::WebGpuDevice& graphics)
	      : world(sourceWorld), m_sink(graphics), m_textures(m_sink), m_model_textures(m_sink), m_models(graphics, m_model_textures, m_sink), m_graphics(graphics)
	{
		m_textures.InitializeDefault(TextureResource(0));
		m_textures.InitializeWhite(TextureResource(1));
		m_model_textures.InitializeDefault(TextureResource(0));
		m_model_textures.InitializeWhite(TextureResource(1));
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
		m_model_textures.ReleaseAll();
	}

	Expected<std::vector<Entity>> WebScene::Apply(const scene::SceneDescription& description)
	{
		// Never silently discard a scene record that belongs to a later browser milestone.
		if (description.version > scene::kSceneFormatVersion || description.entities.size() > 65536 || !description.prefabInstances.empty() || !description.includes.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Unsupported scene version, size or feature in webgpu-2d-v1"));
		}
		for (const auto& record: description.entities)
		{
			if (record.physics || record.effect || record.joint || !record.scriptJoints.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Unsupported 3D entity in browser scene: " + record.name));
			}
			for (const auto& generic: record.reflected)
			{
				if (generic.type != "Rigid Body 2D" && generic.type != "Collider 2D" && generic.type != "Tile Map"
				        && generic.type != "Point Light" && generic.type != "Spot Light" && generic.type != "Sky" && generic.type != "Light 2D Settings"
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
			AE_TRY_VOID(m_models.Apply(world, entity, record));
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
		for (const auto& light: description.lights)
		{
			if (light.isSpot)
			{
				const auto entity = world.Create();
				world.Emplace<NameComponent>(entity, NameComponent{.name = "Spot Light"});
				const float directionLength = glm::dot(light.direction, light.direction);
				if (!std::isfinite(directionLength) || directionLength < 1e-8f)
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid browser legacy spot-light direction"));
				}
				world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ecs::LightAimMatrix(light.position, light.direction)});
				world.Emplace<SpotLightComponent>(entity, SpotLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius,
				        .innerAngleRad = light.innerAngleRad, .outerAngleRad = light.outerAngleRad, .castsShadow = light.castsShadow});
			}
			else
			{
				ecs::CreatePointLightEntity(world, light.position, PointLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .castsShadow = light.castsShadow});
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
		AE_TRY(text, io::FileSystem::ReadFileText(ProjectPath("scenes", name, ".scene.toml")));
		auto description = scene::ParseToml(*text);
		if (!description)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser scene TOML"));
		}
		const auto expanded = scene::ExpandSceneDocument(*description,
		        [](std::string_view path, scene::SceneDocumentKind kind) -> std::expected<scene::SceneDescription, std::string>
		        {
			        const auto resolved = kind == scene::SceneDocumentKind::Scene ? WebScene::ProjectPath("scenes", path, ".scene.toml") : WebScene::ProjectPath("prefabs", path, ".prefab.toml");
			        const auto text = io::FileSystem::ReadFileText(resolved);
			        if (!text) { return std::unexpected("Missing browser scene dependency: " + resolved); }
			        const auto parsed = scene::ParseToml(*text);
			        if (!parsed) { return std::unexpected("Invalid browser scene dependency: " + resolved); }
			        return *parsed;
		        });
		if (!expanded) { AE_UNEXPECTED(AetherError::Asset(expanded.error())); }
		*description = *expanded;
		const auto roots = world.Roots();
		for (const auto root: roots)
		{
			world.Destroy(root);
		}
		world.SetSceneKind(description->kind);
		world.SetSceneFeatures(description->features);
		m_tile_system.InvalidateAll();
		m_models.ClearEntities();
		m_environment = description->environment;
		m_light_time = 0.0f;
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
		AE_TRY(text, io::FileSystem::ReadFileText(ProjectPath("prefabs", name, ".prefab.toml")));
		auto description = scene::ParseToml(*text);
		if (!description)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser prefab TOML"));
		}
		const auto expanded = scene::ExpandSceneDocument(*description,
		        [](std::string_view path, scene::SceneDocumentKind kind) -> std::expected<scene::SceneDescription, std::string>
		        {
			        const auto resolved = kind == scene::SceneDocumentKind::Scene ? WebScene::ProjectPath("scenes", path, ".scene.toml") : WebScene::ProjectPath("prefabs", path, ".prefab.toml");
			        const auto text = io::FileSystem::ReadFileText(resolved);
			        if (!text)
			        {
				        return std::unexpected("Missing browser prefab dependency: " + resolved);
			        }
			        const auto parsed = scene::ParseToml(*text);
			        if (!parsed)
			        {
				        return std::unexpected("Invalid browser prefab dependency: " + resolved);
			        }
			        return *parsed;
		        });
		if (!expanded)
		{
			AE_UNEXPECTED(AetherError::Asset(expanded.error()));
		}
		*description = *expanded;
		for (const auto& record: description->entities)
		{
			if (!record.scripts.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Scripted browser prefabs are not supported yet"));
			}
		}
		const auto root = std::find_if(description->entities.begin(), description->entities.end(), [](const auto& record) { return record.parentIndex < 0; });
		if (root == description->entities.end())
		{
			AE_UNEXPECTED(AetherError::Asset("Browser prefab has no authored root"));
		}
		const auto rootIndex = static_cast<std::size_t>(std::distance(description->entities.begin(), root));
		AE_TRY(created, Apply(*description));
		if (created->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Empty browser prefab"));
		}
		world.SetSceneFeatures(world.GetSceneFeatures() | description->features);
		return (*created)[rootIndex];
	}

	void WebScene::Tick(float dt)
	{
		m_light_time += dt;
		for (const auto raw: world.View<SkinnedMeshComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (!world.Has<DisabledComponent>(entity) && !ecs::HasDisabledAncestor(world, entity))
			{
				auto& skin = world.Get<SkinnedMeshComponent>(entity);
				skin.animTime += dt * skin.playbackSpeed;
			}
		}
	}

	Expected<void> WebScene::ExtractLighting(RenderFramePacket& packet)
	{
		// RenderFramePacket's neutral construction defaults are not the native renderer's
		// startup environment. Match Renderer for authored lights without an environment block.
		packet.sunDirectionIntensity = glm::vec4(std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, 3.0f);
		packet.sunColor = glm::vec4(1.0f, 0.96f, 0.90f, 1.0f);
		packet.ambientColor = glm::vec4(0.03f, 0.04f, 0.06f, 1.0f);
		packet.skyHorizonColor = glm::vec4(0.34f, 0.52f, 0.82f, 1.0f);
		packet.skyZenithColor = glm::vec4(0.08f, 0.19f, 0.45f, 1.0f);
		packet.skyVoidColor = glm::vec4(0.001f, 0.002f, 0.005f, packet.skyVoidColor.w);
		if (m_environment)
		{
			const auto& env = *m_environment;
			if (env.cloudCoverage != 0.0f)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser clouds require the post-effects milestone"));
			}
			const float length = glm::dot(env.sunDirection, env.sunDirection);
			const glm::vec3 direction = length < 1e-8f ? glm::vec3(std::numbers::egamma_v<float>) : glm::normalize(env.sunDirection);
			packet.sunDirectionIntensity = glm::vec4(direction, std::max(env.sunIntensity, 0.0f));
			packet.ambientColor = glm::vec4(glm::max(env.ambient, glm::vec3(0.0f)), 1.0f);
			packet.sunColor = glm::vec4(glm::max(env.sunColor, glm::vec3(0.0f)), 1.0f);
			packet.skyHorizonColor = glm::vec4(glm::max(env.skyHorizon, glm::vec3(0.0f)), 1.0f);
			packet.skyZenithColor = glm::vec4(glm::max(env.skyZenith, glm::vec3(0.0f)), 1.0f);
			packet.skyVoidColor = glm::vec4(glm::max(env.skyVoid, glm::vec3(0.0f)), packet.skyVoidColor.w);
			packet.objectAmbient = glm::vec4(env.objectAmbient, 1.0f);
			packet.objectLight0Direction = glm::vec4(env.objectLight0Direction, 0.0f);
			packet.objectLight0Color = glm::vec4(env.objectLight0Color, 0.0f);
			packet.objectLight1Direction = glm::vec4(env.objectLight1Direction, 0.0f);
			packet.objectLight1Color = glm::vec4(env.objectLight1Color, 0.0f);
			packet.renderLightingEnabled = true;
		}
		for (const auto raw: world.View<SkyComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			const auto& sky = world.Get<SkyComponent>(entity);
			if (sky.fogDensity != 0.0f || sky.cloudCoverage != 0.0f || sky.physicalSky || sky.volumetricFog)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser fog, clouds and physical atmosphere require the post-effects milestone"));
			}
			packet.skyHorizonColor = glm::vec4(glm::max(sky.horizonColor, glm::vec3(0.0f)), 1.0f);
			packet.skyZenithColor = glm::vec4(glm::max(sky.zenithColor, glm::vec3(0.0f)), 1.0f);
			packet.skyVoidColor = glm::vec4(glm::max(sky.groundColor, glm::vec3(0.0f)), packet.skyVoidColor.w);
			packet.ambientColor = glm::vec4(glm::max(sky.ambientColor, glm::vec3(0.0f)), 1.0f);
			packet.renderLightingEnabled = true;
			break;
		}
		packet.light2DAmbient = packet.ambientColor;
		for (const auto raw: world.View<Light2DSettingsComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			const auto& settings = world.Get<Light2DSettingsComponent>(entity);
			if (settings.shaftStrength != 0.0f)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser 2D light shafts require the post-effects milestone"));
			}
			packet.light2DAmbient = glm::vec4(settings.ambientColor * settings.ambientIntensity, 1.0f);
			packet.light2DShadowParams = glm::vec4(settings.shadowStrength, settings.shadowSoftness, 0.0f, settings.shaftNoise);
			packet.light2DHasSettings = true;
			packet.renderLightingEnabled = true;
			break;
		}
		// Native LightSystem orders by entity before submission, including its per-entity flicker seed.
		const auto flicker = [this](float amount, float speed, std::uint32_t seed)
		{
			if (amount <= 0.0f)
			{
				return 1.0f;
			}
			const float phase = static_cast<float>(seed & 0xffffu) * 0.001f;
			const float time = m_light_time * speed;
			const float noise = std::clamp(0.5f + 0.35f * std::sin(time * 0.7f + phase * 6.3f) + 0.25f * std::sin(time * 1.73f + phase * 11.1f + 1.1f), 0.0f, 1.0f);
			return 1.0f - amount * (1.0f - noise);
		};
		std::vector<entt::entity> lights;
		for (const auto raw: world.View<PointLightComponent, TransformComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (!world.Has<DisabledComponent>(entity) && !ecs::HasDisabledAncestor(world, entity))
			{
				lights.push_back(raw);
			}
		}
		std::ranges::sort(lights);
		for (const auto raw: lights)
		{
			const auto entity = World::FromEntt(raw);
			const auto& light = world.Get<PointLightComponent>(entity);
			packet.pointLights.push_back(Renderer::PointLight{.position = glm::vec3(world.Get<TransformComponent>(entity).localToWorld[3]),
			        .radius = std::max(light.radius, 0.01f), .color = glm::max(light.color, glm::vec3(0.0f)),
			        .intensity = std::max(light.intensity * flicker(light.flicker, light.flickerSpeed, static_cast<std::uint32_t>(raw)), 0.0f),
			        .sourceRadius = light.sourceRadius, .castsShadow = light.castsShadow});
		}
		lights.clear();
		for (const auto raw: world.View<SpotLightComponent, TransformComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (!world.Has<DisabledComponent>(entity) && !ecs::HasDisabledAncestor(world, entity))
			{
				lights.push_back(raw);
			}
		}
		std::ranges::sort(lights);
		for (const auto raw: lights)
		{
			const auto entity = World::FromEntt(raw);
			const auto& light = world.Get<SpotLightComponent>(entity);
			const auto forward = -glm::vec3(world.Get<TransformComponent>(entity).localToWorld[2]);
			const float innerAngle = glm::clamp(light.innerAngleRad, 0.01f, 1.54f);
			packet.spotLights.push_back(Renderer::SpotLight{.position = glm::vec3(world.Get<TransformComponent>(entity).localToWorld[3]),
			        .radius = std::max(light.radius, 0.01f), .direction = glm::dot(forward, forward) > 1e-8f ? glm::normalize(forward) : glm::vec3(0.0f, -1.0f, 0.0f),
			        .innerAngleRad = innerAngle, .color = glm::max(light.color, glm::vec3(0.0f)),
			        .intensity = std::max(light.intensity * flicker(light.flicker, light.flickerSpeed, static_cast<std::uint32_t>(raw)), 0.0f),
			        .outerAngleRad = glm::clamp(light.outerAngleRad, innerAngle + 0.01f, 1.55f), .sourceRadius = light.sourceRadius, .castsShadow = light.castsShadow});
		}
		packet.renderLightingEnabled |= !packet.pointLights.empty() || !packet.spotLights.empty();
		packet.directionalShadowEnabled = packet.renderLightingEnabled && packet.sunDirectionIntensity.w > 0.001f;
		return {};
	}

	Expected<void> WebScene::Extract(RenderFramePacket& packet, unsigned width, unsigned height, float elapsed)
	{
		// These are the project's native graphics keys, carried through the export manifest.
		packet.tonemapExposure = static_cast<float>(EM_ASM_DOUBLE({ return globalThis.aetherProject?.graphics?.exposure ?? 1; }));
		const int tonemapLength = EM_ASM_INT({ return lengthBytesUTF8(globalThis.aetherProject?.graphics?.tonemap || 'ACES Filmic'); });
		if (tonemapLength < 1 || tonemapLength > 128)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser tonemap setting"));
		}
		std::string tonemap(tonemapLength, '\0');
		EM_ASM({ stringToUTF8(globalThis.aetherProject?.graphics?.tonemap || 'ACES Filmic', $0, $1); }, tonemap.data(), tonemap.size() + 1);
		packet.tonemapMode = static_cast<std::uint32_t>(TonemapModeFromName(tonemap));
		packet.elapsedTime = elapsed;
		packet.sceneFeatures = world.GetSceneFeatures();
		packet.renderExtent = {width, height};
		packet.uiExtent = {width, height};
		packet.hasCameraData = true;
		packet.view = glm::mat4(1);
		packet.proj = glm::mat4(1);
		for (const auto entity: world.View<CameraComponent, TransformComponent, MainCameraComponent>())
		{
			const auto& camera = world.GetRegistry().get<CameraComponent>(entity);
			const auto& transform = world.GetRegistry().get<TransformComponent>(entity);
			if (world.Has<DisabledComponent>(World::FromEntt(entity)) || ecs::HasDisabledAncestor(world, World::FromEntt(entity)))
			{
				continue;
			}
			const float halfHeight = camera.orthographicHeight * 0.5f;
			const float halfWidth = halfHeight * float(width) / std::max(height, 1u);
			if (camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane || camera.fovDegrees <= 0 || camera.fovDegrees >= 180 || camera.orthographicHeight <= 0)
			{
				AE_UNEXPECTED(AetherError::Asset("Invalid browser camera projection"));
			}
			packet.proj = camera.projection == CameraProjection::Orthographic
			        ? glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, camera.nearPlane, camera.farPlane)
			        : glm::perspectiveRH_ZO(glm::radians(camera.fovDegrees), float(width) / std::max(height, 1u), camera.nearPlane, camera.farPlane);
			packet.view = glm::inverse(transform.localToWorld);
			packet.cameraWorldPos = glm::vec4(glm::vec3(transform.localToWorld[3]), 1.0f);
			packet.cameraNearPlane = camera.nearPlane;
			if (camera.depthOfField)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser depth of field requires the post-effects milestone"));
			}
			packet.backgroundMode = static_cast<std::uint32_t>(camera.background);
			packet.backgroundAngleRadians = glm::radians(camera.gradientAngleDegrees);
			packet.skyVoidColor.w = camera.background == CameraBackground::SkyGradient ? 1.0f : 0.0f;
			if (camera.background == CameraBackground::SolidColour)
			{
				packet.backgroundStopCount = 1;
				packet.backgroundStops[0] = glm::vec4(camera.clearColor, 0.0f);
				packet.renderLightingEnabled = true;
			}
			else if (camera.background == CameraBackground::Gradient)
			{
				if (camera.gradientStops.size() < 2 || camera.gradientStops.size() > RenderFramePacket::kMaxBackgroundStops)
				{
					AE_UNEXPECTED(AetherError::Asset("Browser gradient requires between two and eight stops"));
				}
				packet.backgroundStopCount = static_cast<std::uint32_t>(camera.gradientStops.size());
				for (std::uint32_t i = 0; i < packet.backgroundStopCount; ++i)
				{
					packet.backgroundStops[i] = glm::vec4(camera.gradientStops[i].colour, camera.gradientStops[i].position);
				}
				packet.renderLightingEnabled = true;
			}
			break;
		}
		packet.proj[1][1] *= -1;
		AE_TRY_VOID(ExtractLighting(packet));
		m_sprite_system.Extract(world, packet.render2D);
		AE_TRY_VOID(m_models.Extract(world, packet));
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
