#include "WebScene.hpp"
#include "scene/SceneStreamingReferences.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <numbers>
#include <emscripten.h>
#include "utils/Utf8.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "io/FileSystem.hpp"
#include "platform/web/WebFileSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "physics/PhysicsComponents.hpp"
#include "animation/SpriteAnimationSystem.hpp"
#include "animation/AnimationSystem.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include <functional>
#include "scene/SceneSerializerDetail.hpp"
#include "scene/TransformUtils.hpp"
#include "ui/UiImageSystem.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "WebAssetFormat.hpp"
#include "scene/WorldStreaming.hpp"
#include "scene/SceneDocumentExpansion.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "passes/TonemapDefs.hpp"
#include "rendering/SceneEffectExtraction.hpp"
#include "rendering/FrameConstants.hpp"
#include "scene/LightComponents.hpp"
#include "utils/EngineSettings.hpp"
#include "particles/ParticleSystem.hpp"
#include "particles/ParticleValidation.hpp"

namespace aether::app::web
{
	std::string WebScene::ProjectPath(std::string_view kind, std::string_view name, std::string_view suffix)
	{
		if (name.starts_with("project://") || name.starts_with("uid://"))
		{
			const std::string path = io::FileSystem::ResolveAssetPath(std::string(name));
			if (!path.starts_with("project://") || !path.ends_with(suffix) ||
			    path.size() <= std::string_view("project://").size() + suffix.size()) return {};
			return path;
		}
		if (name.empty() || name.size() > 128 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos) return {};
		const std::string key(kind);
		const auto length = EM_ASM_INT({ return lengthBytesUTF8(globalThis.aetherProject ?.paths ?.[UTF8ToString($0)] || (UTF8ToString($0) == 'prefabs' ? 'assets/prefabs' : 'scenes')); }, key.c_str());
		std::string base(length, '\0');
		EM_ASM({ stringToUTF8(globalThis.aetherProject ?.paths ?.[UTF8ToString($0)] || (UTF8ToString($0) == 'prefabs' ? 'assets/prefabs' : 'scenes'), $1, $2); }, key.c_str(), base.data(), base.size() + 1);
		return "project://" + base + "/" + std::string(name) + std::string(suffix);
	}

	namespace
	{
		struct StreamingModelScope
		{
			WebModels& models;
			explicit StreamingModelScope(WebModels& value) : models(value) { models.SetStreamingActivation(true); }
			~StreamingModelScope() { models.SetStreamingActivation(false); }
		};

		Expected<scene::SceneDescription> PreparedSceneRecord(std::span<const std::byte> bytes, std::size_t& offset)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t))
				AE_UNEXPECTED(AetherError::Asset("Truncated prepared browser scene record"));
			std::uint32_t length;
			std::memcpy(&length, bytes.data() + offset, sizeof(length));
			offset += sizeof(length);
			if (length > bytes.size() - offset)
				AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene record length"));
			auto result = scene::ReadSceneBinaryChecked(bytes.data() + offset, length);
			offset += length;
			return result;
		}


		// Same per-entity physics-domain rule as native SceneSerializerApply: one entity never
		// simulates 2D and 3D physics at once; the scene kind breaks the tie.
		struct PhysicsDomain
		{
			bool apply2D = false;
			bool apply3D = false;
		};

		PhysicsDomain ResolvePhysicsDomain(const scene::EntityRecord& record, SceneKind kind)
		{
			PhysicsDomain domain{record.joint2D.has_value(), record.physics.has_value() || record.joint.has_value()};
			for (const auto& generic: record.reflected)
			{
				const auto* type = reflect::FindComponentType(generic.type);
				if (type != nullptr && (static_cast<std::uint32_t>(type->requiredFeatures) & static_cast<std::uint32_t>(SceneFeatureFlags::Physics2D)) != 0)
				{
					domain.apply2D = true;
				}
			}
			if (domain.apply2D && domain.apply3D)
			{
				(kind == SceneKind::Scene2D ? domain.apply3D : domain.apply2D) = false;
			}
			return domain;
		}

		Expected<void> ExtractPostSettings(RenderFramePacket& packet)
		{
			auto& settings = packet.postEffects;
			// Real project manifests always carry a graphics object. Internal reference
			// fixtures without one intentionally retain the neutral comparison settings.
			const bool authored = EM_ASM_INT({ return globalThis.aetherProject?.graphics != null ? 1 : 0; });
			const EngineSettings::Graphics defaults;
			const auto readFloat = [](const char* name, float fallback) -> Expected<float>
			{
				const double value = EM_ASM_DOUBLE({ const value = globalThis.aetherProject?.graphics?.[UTF8ToString($0)]; return value == null ? $1 : (typeof value == 'number' ? value : NaN); }, name, fallback);
				if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid browser graphics number: " + std::string(name)));
				}
				return static_cast<float>(value);
			};
			const auto readBool = [](const char* name, bool fallback) -> Expected<bool>
			{
				const int value = EM_ASM_INT({ const value = globalThis.aetherProject?.graphics?.[UTF8ToString($0)]; return value == null ? $1 : (typeof value == 'boolean' ? (value ? 1 : 0) : -1); }, name, fallback);
				if (value < 0)
				{
					AE_UNEXPECTED(AetherError::Asset("Invalid browser graphics boolean: " + std::string(name)));
				}
				return value != 0;
			};
#define AE_WEB_POST_FIELD(name, reader) \
		{ \
			AE_TRY(value, reader(#name, authored ? defaults.name : settings.name)); \
			settings.name = *value; \
		}
			AE_WEB_POST_FIELD(fxaa, readBool)
			AE_WEB_POST_FIELD(gtao, readBool)
			AE_WEB_POST_FIELD(autoExposure, readBool)
			AE_WEB_POST_FIELD(reflections, readBool)
			AE_WEB_POST_FIELD(volumetrics, readBool)
			AE_WEB_POST_FIELD(gradeContrast, readFloat)
			AE_WEB_POST_FIELD(gradeSaturation, readFloat)
			AE_WEB_POST_FIELD(gradeTemperature, readFloat)
			AE_WEB_POST_FIELD(gradeTint, readFloat)
			AE_WEB_POST_FIELD(vignetteIntensity, readFloat)
			AE_WEB_POST_FIELD(vignetteRoundness, readFloat)
			AE_WEB_POST_FIELD(chromaticAberration, readFloat)
			AE_WEB_POST_FIELD(sharpness, readFloat)
			AE_WEB_POST_FIELD(filmGrain, readFloat)
			AE_WEB_POST_FIELD(motionBlur, readFloat)
			AE_WEB_POST_FIELD(motionBlurMaxRadius, readFloat)
			AE_WEB_POST_FIELD(gtaoRadius, readFloat)
			AE_WEB_POST_FIELD(gtaoStrength, readFloat)
			AE_WEB_POST_FIELD(specularFilter, readFloat)
			AE_WEB_POST_FIELD(foliageSkyLook, readFloat)
			AE_WEB_POST_FIELD(bloomStrength, readFloat)
			AE_WEB_POST_FIELD(bloomRadius, readFloat)
			AE_WEB_POST_FIELD(exposureKey, readFloat)
			AE_WEB_POST_FIELD(exposureSpeed, readFloat)
			AE_WEB_POST_FIELD(reflectionMaxRoughness, readFloat)
			AE_WEB_POST_FIELD(reflectionIntensity, readFloat)
			AE_WEB_POST_FIELD(renderScale, readFloat)
#undef AE_WEB_POST_FIELD
			AE_TRY(contact, readBool("contactShadows", authored ? defaults.contactShadows : packet.contactShadows));
			AE_TRY(split, readFloat("shadowSplitLambda", authored ? defaults.shadowSplitLambda : packet.shadowSplitLambda));
			AE_TRY(anisotropy, readFloat("anisotropy", static_cast<float>(authored ? defaults.anisotropy : packet.anisotropy)));
			AE_TRY(lodBias, readFloat("mipLodBias", authored ? defaults.mipLodBias : packet.mipLodBias));
			packet.anisotropy = static_cast<int>(std::clamp(*anisotropy, 1.0f, 16.0f));
			packet.mipLodBias = ClampTextureLodBias(*lodBias);
			packet.contactShadows = *contact;
			packet.shadowSplitLambda = *split;
			return {};
		}
	} // namespace

	std::string WebTextureSink::ResolvePath(std::string_view path) const
	{
		const auto dot = path.rfind('.');
		return dot == std::string_view::npos ? std::string(path) : std::string(path.substr(0, dot)) + ".texture";
	}

	Expected<TextureResource> WebTextureSink::Load(std::string_view path, TextureColorSpace colorSpace)
	{
		error.clear();
		m_graphics.CollectRetiredTextures(m_free_slots);
		const auto resolved = io::FileSystem::ResolveAssetPath(path);
		auto bytes = aether::web::FindWebAsset(resolved);
		const bool streamed = !bytes.empty();
		std::vector<std::byte> bootstrap;
		if (bytes.empty())
		{
			AE_TRY(loaded, io::FileSystem::ReadFile(path));
			bootstrap = std::move(*loaded);
			bytes = bootstrap;
		}
		WebTextureHeader header;
		if (!ReadWebTextureHeader(bytes, header) || (m_free_slots.empty() && m_next_slot >= Capacity()))
		{
			error = "Missing, invalid or oversized browser texture, or texture capacity exceeded: " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		const auto slot = m_free_slots.empty() ? m_next_slot : m_free_slots.back();
		const auto* pixels = reinterpret_cast<const std::uint8_t*>(bytes.data() + sizeof(header));
		const auto mipData = streamed && colorManaged ? aether::web::FindWebAsset(resolved + ".mips") : std::span<const std::byte>{};
		if (streamed && colorManaged && mipData.empty())
			AE_UNEXPECTED(AetherError::Asset("Streaming texture mip data was not prepared by the worker: " + resolved));
		const bool uploaded = mipData.empty()
		        ? m_graphics.UploadTexture(slot, header.width, header.height, {pixels, static_cast<size_t>(header.pixelBytes)}, colorManaged && colorSpace == TextureColorSpace::Srgb, colorManaged, streamed ? m_budget : nullptr)
		        : m_graphics.UploadPreparedTexture(slot, mipData, colorSpace == TextureColorSpace::Srgb, m_budget);
		if (!uploaded)
		{
			error = m_graphics.TextureUploadError().empty() ? "WebGPU texture upload failed: " + std::string(path) : m_graphics.TextureUploadError() + ": " + std::string(path);
			AE_UNEXPECTED(AetherError::Asset(error));
		}
		if (m_free_slots.empty()) ++m_next_slot;
		else m_free_slots.pop_back();
		auto lifetime = std::shared_ptr<void>(new std::uint32_t(slot), [state = m_retirement](void* value)
		{
			const auto retired = *static_cast<std::uint32_t*>(value);
			delete static_cast<std::uint32_t*>(value);
			if (state->graphics) state->graphics->RetireTextures({retired});
		});
		return TextureResource(slot, std::move(lifetime));
	}

	void WebTextureSink::Retire(std::vector<std::uint32_t> slots)
	{
		m_graphics.RetireTextures(std::move(slots));
	}

	WebScene::WebScene(World& sourceWorld, webgpu::WebGpuDevice& graphics)
	      : world(sourceWorld), m_sink(graphics), m_textures(m_sink), m_model_textures(m_sink), m_models(graphics, m_model_textures, m_sink), m_graphics(graphics)
	{
		m_textures.InitializeDefault(TextureResource(0));
		m_textures.InitializeWhite(TextureResource(1));
		m_model_textures.InitializeDefault(TextureResource(0));
		m_model_textures.InitializeWhite(TextureResource(1));
		UiImageSystem::ConnectLifecycle(world, m_model_textures);
		m_sprite_system.Initialize(m_textures, m_sprites);
		m_tile_system.Initialize(m_textures, m_sprites, m_tiles);
		// Native Application order: Animation, SpriteAnimation, Physics, Physics2D, (scripts),
		// Particles. Scripts run after every world simulation system and before particles so a
		// burst a script queues emits the same tick, and collision events produced by this tick's
		// physics step are visible to the same tick's scripts.
		auto animation = std::make_unique<AnimationSystem>();
		m_animation = animation.get();
		m_animation->SetClipDurationSource([this](World& source, Entity entity, const SkinnedMeshComponent& skin, float& clip, float& fade) { return m_models.ClipDurations(source, entity, skin, clip, fade); });
		world.RegisterSystem(std::move(animation));
		world.RegisterSystem(std::make_unique<SpriteAnimationSystem>(m_sprites));
		auto physics3D = std::make_unique<PhysicsSystem>();
		m_physics = physics3D.get();
		world.RegisterSystem(std::move(physics3D));
		auto physics = std::make_unique<Physics2DSystem>();
		physics->SetTileAssets(&m_tiles);
		world.RegisterSystem(std::move(physics));
		auto script = std::make_unique<WebScriptSystem>();
		m_script_system = script.get();
		world.RegisterSystem(std::move(script));
		auto particles = std::make_unique<ParticleSystem>(m_textures);
		particles->SetRetireCallback([this](Entity entity) { m_retired_particles.push_back(entity); });
		world.RegisterSystem(std::move(particles));
	}

	WebScene::~WebScene()
	{
		UiImageSystem::DisconnectLifecycle(world);
		world.UnregisterSystem("ParticleSystem");
		world.UnregisterSystem("WebScriptSystem");
		world.UnregisterSystem("Physics2DSystem");
		world.UnregisterSystem("PhysicsSystem");
		world.UnregisterSystem("SpriteAnimationSystem");
		world.UnregisterSystem("AnimationSystem");
		m_tile_system.Shutdown();
		m_sprite_system.Shutdown();
		m_models.ClearEntities();
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
			if (record.effect && record.effect->name != "plasma" && record.effect->name != "molten")
			{
				AE_UNEXPECTED(AetherError::Asset("Unsupported browser entity effect: " + record.effect->name));
			}
			for (const auto& generic: record.reflected)
			{
				if (generic.type != "Rigid Body 2D" && generic.type != "Collider 2D" && generic.type != "Tile Map" && generic.type != "Character Controller"
				        && generic.type != "Particle Emitter" && generic.type != "Audio Source"
				        && generic.type != "Darkness Volume" && generic.type != "Grass" && generic.type != "Grass Bender"
				        && generic.type != "Network Identity" && generic.type != "Network Transform" && generic.type != "Net Player"
				        && generic.type != "Point Light" && generic.type != "Spot Light" && generic.type != "Sky" && generic.type != "Light 2D Settings"
				        && (generic.type != "UI Canvas" && generic.type != "UI Rect" && generic.type != "UI Text" && generic.type != "UI Image" && generic.type != "UI Selectable" && generic.type != "UI Slider" && generic.type != "UI Toggle"
				                && generic.type != "UI Button" && generic.type != "UI Progress Bar" && generic.type != "UI Text Box"
				                && generic.type != "UI Mask" && generic.type != "UI Effect" && generic.type != "UI Material"))
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
		struct Rollback
		{
			World& world;
			std::vector<Entity>& entities;
			WebModels& models;
			bool committed = false;
			~Rollback()
			{
				if (!committed)
				{
					for (const auto entity: entities)
					{
						if (world.GetRegistry().valid(World::ToEntt(entity)))
							world.Destroy(entity);
					}
					models.ReleaseUnused(world);
				}
			}
		} rollback{world, created, m_models};
		for (const auto& record: description.entities)
		{
			const auto entity = world.Create();
			created.push_back(entity);
			world.Emplace<NameComponent>(entity, NameComponent{.name = record.name});
			if (record.nodeId)
			{
				world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{record.nodeId, record.sourceNodeId});
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
			if (record.effect)
			{
				world.Emplace<EffectRefComponent>(entity, EffectRefComponent{.name = record.effect->name});
				world.Emplace<EffectParamsComponent>(entity, EffectParamsComponent{.params = record.effect->params});
			}
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
			const auto domain = ResolvePhysicsDomain(record, description.kind);
			for (const auto& generic: record.reflected)
			{
				const auto* type = reflect::FindComponentType(generic.type);
				if (!type || !type->emplaceDefault)
				{
					AE_UNEXPECTED(AetherError::Asset("Unknown browser component: " + generic.type));
				}
				// The entity's 2D physics lost the 2D/3D tie-break: never emplace both domains.
				if (!domain.apply2D && (static_cast<std::uint32_t>(type->requiredFeatures) & static_cast<std::uint32_t>(SceneFeatureFlags::Physics2D)) != 0)
				{
					continue;
				}
				if (generic.type == "Character Controller")
				{
					world.SetSceneFeatures(world.GetSceneFeatures() | SceneFeatureFlags::Physics3D);
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
				image.cornerRadius = record.uiImage->cornerRadius;
				image.texturePath = record.uiImage->texturePath;
				image.textureDirty = true;
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
			if (record.joint2D && ResolvePhysicsDomain(record, description.kind).apply2D)
			{
				auto joint = *record.joint2D;
				if (record.joint2DTargetIndex >= 0 && static_cast<size_t>(record.joint2DTargetIndex) < created.size())
				{
					joint.target = created[record.joint2DTargetIndex];
				}
				world.Emplace<Joint2DComponent>(created[i], joint);
			}
		}
		AE_TRY_VOID(ApplyPhysics3D(description, created));
		for (const auto& light: description.lights)
		{
			if (light.isSpot)
			{
				const auto entity = world.Create();
				created.push_back(entity);
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
				created.push_back(ecs::CreatePointLightEntity(world, light.position, PointLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .castsShadow = light.castsShadow}));
			}
		}
		rollback.committed = true;
		return created;
	}

	// The same component values native's PhysicsSerde.cpp applies (Collider+RigidBody from one
	// PhysicsRecord, Joint, Script Joints), in entity order so body creation order, and with it
	// Jolt body ids and solver order, matches a native load. Runs after every entity exists
	// because joints name their targets by scene index.
	Expected<void> WebScene::ApplyPhysics3D(const scene::SceneDescription& description, const std::vector<Entity>& created)
	{
		for (std::size_t i = 0; i < created.size(); ++i)
		{
			const auto& record = description.entities[i];
			if (!ResolvePhysicsDomain(record, description.kind).apply3D)
			{
				continue;
			}
			const Entity entity = created[i];
			if (record.physics)
			{
				const auto& phys = *record.physics;
				world.Emplace<ColliderComponent>(entity,
				        ColliderComponent{
				                .shape = phys.shapeType,
				                .halfExtents = phys.halfExtents,
				                .radius = phys.radius,
				                .halfHeight = phys.halfHeight,
				                .center = phys.center,
				                .friction = phys.friction,
				                .restitution = phys.restitution,
				                .isSensor = phys.isSensor,
				                .layer = phys.isSensor ? PhysicsLayer::Sensor : PhysicsLayer::Moving,
				                .meshSource = phys.meshSource,
				        });
				world.Emplace<RigidBodyComponent>(entity,
				        RigidBodyComponent{
				                .motionType = phys.motionType,
				                .mass = phys.mass,
				                .linearDamping = phys.linearDamping,
				                .angularDamping = phys.angularDamping,
				                .gravityFactor = phys.gravityFactor,
				                .maxLinearVelocity = phys.maxLinearVelocity,
				                .maxAngularVelocity = phys.maxAngularVelocity,
				                .continuousCollision = phys.continuousCollision,
				                .allowSleeping = phys.allowSleeping,
				                .lockPosition = phys.lockPosition,
				                .lockRotation = phys.lockRotation,
				                .startActive = phys.startActive,
				        });
			}
			if (record.joint)
			{
				const auto& jr = *record.joint;
				Entity target{};
				if (jr.targetIndex >= 0 && jr.targetIndex < static_cast<int>(created.size()))
				{
					target = created[static_cast<std::size_t>(jr.targetIndex)];
				}
				world.Emplace<JointComponent>(entity,
				        JointComponent{
				                .type = jr.type,
				                .target = target,
				                .anchor = jr.anchor,
				                .axis = jr.axis,
				                .minLimit = jr.minLimit,
				                .maxLimit = jr.maxLimit,
				                .distance = jr.distance,
				                .collideConnected = jr.collideConnected,
				        });
			}
			for (const auto& sjr: record.scriptJoints)
			{
				JointEntry entry;
				if (sjr.targetIndex >= 0 && sjr.targetIndex < static_cast<int>(created.size()))
				{
					entry.target = created[static_cast<std::size_t>(sjr.targetIndex)];
				}
				entry.type = sjr.type;
				entry.anchor = sjr.anchor;
				entry.axis = sjr.axis;
				entry.minLimit = sjr.minLimit;
				entry.maxLimit = sjr.maxLimit;
				entry.distance = sjr.distance;
				entry.swingLimit = sjr.swingLimit;
				entry.collideConnected = sjr.collideConnected;
				m_physics->AddScriptJoint(world, entity, entry);
			}
			world.SetSceneFeatures(world.GetSceneFeatures() | SceneFeatureFlags::Physics3D);
		}
		return {};
	}

	Expected<std::vector<WebScriptRecord>> WebScene::Load(std::string_view name)
	{
		const std::string path = ProjectPath("scenes", name, ".scene.toml");
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid scene name or reference"));
		}
		AE_TRY(text, io::FileSystem::ReadFileText(path));
		auto description = scene::ParseToml(*text);
		if (!description)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser scene TOML"));
		}
		auto regions = description->streamingRegions;
		// Headerless prefab defaults contribute feature metadata during expansion, but
		// the primary authored scene alone chooses its default lighting policy.
		const bool defaultLighting = description->kind == SceneKind::Scene3D && HasSceneFeature(description->features, SceneFeatureFlags::Lighting3D);
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
		if (!regions.empty())
		{
			if (auto ids = scene::EnsureStreamingSceneNodeIds(*description); !ids) AE_UNEXPECTED(AetherError::Asset(ids.error()));
		}
		++m_scene_generation;
		// Detach persistent subtrees before destroying their nonpersistent parents.
		auto& registry = world.GetRegistry();
		std::vector<Entity> doomed;
		std::vector<Entity> spared;
		for (const auto handle: registry.storage<entt::entity>())
		{
			if (!registry.valid(handle)) continue;
			const auto entity = World::FromEntt(handle);
			if (ecs::HasDontDestroyOnLoadAncestor(world, entity)) spared.push_back(entity);
			else doomed.push_back(entity);
		}
		if (registry.ctx().contains<scene::StreamingFieldReferenceState>())
		{
			std::vector<Entity> detachedNodes;
			for (const auto entity: doomed)
			{
				const auto* node = world.TryGet<SceneNodeComponent>(entity);
				if (node && node->id) detachedNodes.push_back(entity);
			}
			if (!spared.empty())
			{
				if (auto result = scene::PrepareStreamingFieldReferencesForDetach(world, detachedNodes); !result)
					return std::unexpected(result.error());
			}
			scene::ForgetStreamingFieldReferences(world, doomed);
		}
		for (const auto entity: spared)
		{
			const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
			if (hierarchy && hierarchy->parent.IsValid() && !ecs::HasDontDestroyOnLoadAncestor(world, hierarchy->parent))
				ecs::DetachFromParent(world, entity);
			if (world.Has<scene::StreamingRegionOwner>(entity)) world.Remove<scene::StreamingRegionOwner>(entity);
		}
		for (const auto entity: doomed)
		{
			if (registry.valid(World::ToEntt(entity)))
			{
				world.Destroy(entity);
			}
		}
		world.SetSceneKind(description->kind);
		world.SetSceneFeatures(description->features);
		m_tile_system.InvalidateAll();
		if (spared.empty()) m_models.ClearEntities();
		else m_models.ReleaseUnused(world);
		m_retired_particles.clear();
		m_environment = description->environment;
		m_default_lighting = defaultLighting;
		m_light_time = 0.0f;
		AE_TRY(created, Apply(*description));
		if (!regions.empty())
		{
			scene::StreamingNodeIndex index(world);
			for (std::size_t i = 0; i < description->entities.size(); ++i)
			{
				for (const auto& generic: description->entities[i].reflected)
				{
					const auto* type = reflect::FindComponentType(generic.type);
					auto* component = type && type->tryGetRaw ? type->tryGetRaw(world, (*created)[i]) : nullptr;
					if (!component) continue;
					for (const auto& [name, value]: generic.fields)
					{
						const auto* field = type->FindField(name);
						if (!field || (field->type != reflect::FieldType::EntityRef && field->type != reflect::FieldType::List)) continue;
						if (auto result = scene::ApplyStreamingFieldReference(world, (*created)[i], *type, *field, component, value, index); !result) return std::unexpected(result.error());
					}
				}
			}
		}
		if (registry.ctx().contains<scene::StreamingFieldReferenceState>())
		{
			if (auto result = scene::ResolveStreamingFieldReferences(world); !result)
				return std::unexpected(result.error());
		}
		AE_TRY(scripts, ResolveScripts(*description, *created));
		static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem"))->FlushPendingOnly(world);
		m_physics->FlushPendingOnly(world);
		m_regions = std::move(regions);
		return std::move(*scripts);
	}

	Expected<WebRegionActivation> WebScene::BeginPreparedRegion(std::span<const std::byte> prepared, std::uint64_t restoreGateNode)
	{
		if (std::endian::native != std::endian::little || prepared.size() < 16 ||
		        std::memcmp(prepared.data(), "AESC", 4) != 0 || prepared.size() > UINT64_MAX / 8)
			AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene header"));
		std::uint32_t version, count, reserved;
		std::memcpy(&version, prepared.data() + 4, 4);
		std::memcpy(&count, prepared.data() + 8, 4);
		std::memcpy(&reserved, prepared.data() + 12, 4);
		if (version != 1 || !count || count > 65537 || reserved)
			AE_UNEXPECTED(AetherError::Asset("Unsupported prepared browser scene"));
		const auto charge = static_cast<std::uint64_t>(prepared.size()) * 8;
		if (!m_streaming_budget || !m_streaming_budget->TryReserve(streaming::MemoryClass::CpuPending, charge))
			AE_UNEXPECTED(AetherError::Asset("Browser scene activation exceeds CPU budget"));
		struct Reservation
		{
			std::shared_ptr<streaming::BudgetLedger> budget;
			std::uint64_t bytes;
			~Reservation() { if (bytes) budget->Release(streaming::MemoryClass::CpuPending, bytes); }
		} reservation{m_streaming_budget, charge};
		std::size_t offset = 16;
		AE_TRY(metadata, PreparedSceneRecord(prepared, offset));
		if (!metadata->entities.empty() || !metadata->streamingRegions.empty() || metadata->kind != world.GetSceneKind())
			AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene metadata"));
		WebRegionActivation activation;
		activation.description = std::move(*metadata);
		activation.description.entities.reserve(count - 1);
		activation.region.entities.reserve(count - 1 + activation.description.lights.size());
		activation.prepared = prepared;
		activation.preparedOffset = offset;
		activation.preparedEntities = count - 1;
		activation.budget = m_streaming_budget;
		activation.documentCharge = charge;
		activation.restoreGateNode = restoreGateNode;
		reservation.bytes = 0;
		return activation;
	}

	Expected<WebRegionActivation> WebScene::BeginRegion(std::string_view sceneReference)
	{
		const auto path = ProjectPath("scenes", sceneReference, ".scene.toml");
		if (path.empty())
			AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming scene reference"));
		const auto prepared = aether::web::FindWebAsset(path + ".prepared-scene");
		if (!prepared.empty()) return BeginPreparedRegion(prepared, 0);
		if (!aether::web::FindWebAsset(path).empty())
			AE_UNEXPECTED(AetherError::Asset("Streamed browser scene was not prepared by the worker"));
		AE_TRY(text, io::FileSystem::ReadFileText(path));
		auto description = scene::ParseToml(*text);
		if (!description || !description->streamingRegions.empty())
			AE_UNEXPECTED(AetherError::Asset("Invalid or nested browser streaming scene"));
		if (description->kind != world.GetSceneKind())
			AE_UNEXPECTED(AetherError::Asset("Browser streaming scene kind does not match the world"));
		auto expanded = scene::ExpandSceneDocument(*description,
		        [](std::string_view reference, scene::SceneDocumentKind kind) -> std::expected<scene::SceneDescription, std::string>
		        {
			        const auto resolved = kind == scene::SceneDocumentKind::Scene
			                ? WebScene::ProjectPath("scenes", reference, ".scene.toml")
			                : WebScene::ProjectPath("prefabs", reference, ".prefab.toml");
			        const auto text = io::FileSystem::ReadFileText(resolved);
			        if (!text) return std::unexpected("Missing browser streaming dependency: " + resolved);
			        auto parsed = scene::ParseToml(*text);
			        if (!parsed || !parsed->streamingRegions.empty())
				        return std::unexpected("Invalid or nested browser streaming dependency: " + resolved);
			        return std::move(*parsed);
		        }, {.preserveExternalNodeReferences = true});
		if (!expanded) AE_UNEXPECTED(AetherError::Asset(expanded.error()));
		if (auto ids = scene::EnsureStreamingSceneNodeIds(*expanded); !ids) AE_UNEXPECTED(AetherError::Asset(ids.error()));
		if (expanded->entities.size() > 65536 || !expanded->streamingRegions.empty())
			AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming region size or nesting"));
		std::vector<unsigned char> marks(expanded->entities.size());
		for (std::size_t i = 0; i < marks.size(); ++i)
		{
			if (marks[i] == 2) continue;
			int current = static_cast<int>(i);
			while (current >= 0 && static_cast<std::size_t>(current) < marks.size() && marks[current] == 0)
			{
				marks[current] = 1;
				current = expanded->entities[current].parentIndex;
			}
			if (current >= 0 && (static_cast<std::size_t>(current) >= marks.size() || marks[current] == 1))
				AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming hierarchy"));
			current = static_cast<int>(i);
			while (current >= 0 && marks[current] == 1)
			{
				marks[current] = 2;
				current = expanded->entities[current].parentIndex;
			}
		}
		WebRegionActivation activation;
		activation.description = *expanded;
		activation.region.entities.reserve(expanded->entities.size() + expanded->lights.size());
		return activation;
	}

	Expected<bool> WebScene::StepRegion(WebRegionActivation& activation, std::uint32_t entityBudget)
	{
		if (!entityBudget) AE_UNEXPECTED(AetherError::Asset("Invalid streaming activation budget"));
		auto& description = activation.description;
		auto& entities = activation.region.entities;
		const auto authoredCount = activation.prepared.empty() ? description.entities.size() : static_cast<std::size_t>(activation.preparedEntities);
		if (!activation.region.activationGate.IsValid())
		{
			const auto gate = world.Create();
			world.Emplace<NameComponent>(gate, NameComponent{.name = "Streaming Region"});
			world.Emplace<TransformComponent>(gate);
			world.Emplace<DisabledComponent>(gate);
			if (activation.restoreGateNode)
			{
				world.Emplace<SceneNodeComponent>(gate, SceneNodeComponent{activation.restoreGateNode});
				activation.nodes.emplace(activation.restoreGateNode, gate);
			}
			activation.region.activationGate = gate;
			--entityBudget;
		}
		while (entityBudget && activation.phase < 3)
		{
			if (activation.phase == 0)
			{
				if (activation.cursor == authoredCount + description.lights.size())
				{
					if (!activation.prepared.empty())
					{
						if (activation.preparedOffset != activation.prepared.size() || description.entities.size() != authoredCount)
							AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene record count"));
						std::vector<unsigned char> marks(authoredCount);
						for (std::size_t i = 0; i < authoredCount; ++i)
						{
							if (marks[i] == 2) continue;
							int current = static_cast<int>(i);
							while (current >= 0 && static_cast<std::size_t>(current) < authoredCount && marks[current] == 0)
							{
								marks[current] = 1;
								current = description.entities[current].parentIndex;
							}
							if (current >= 0 && (static_cast<std::size_t>(current) >= authoredCount || marks[current] == 1))
								AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene hierarchy"));
							current = static_cast<int>(i);
							while (current >= 0 && marks[current] == 1) { marks[current] = 2; current = description.entities[current].parentIndex; }
						}
						if (activation.restoreGateNode)
						{
							if (!activation.restoredGateSeen) AE_UNEXPECTED(AetherError::Asset("Missing restored streaming gate"));
							for (const auto& record: description.entities)
								if (record.parentIndex < 0 && record.nodeId != activation.restoreGateNode)
									AE_UNEXPECTED(AetherError::Asset("Restored streaming entity lies outside its gate"));
						}
					}
					activation.cursor = 0;
					activation.fieldNodes = std::make_shared<scene::StreamingNodeIndex>(world);
					activation.phase = 1;
					continue;
				}
				scene::SceneDescription slice;
				slice.kind = description.kind;
				slice.features = description.features;
				if (activation.cursor < authoredCount)
				{
					if (!activation.prepared.empty())
					{
						AE_TRY(document, PreparedSceneRecord(activation.prepared, activation.preparedOffset));
						if (document->entities.size() != 1 || !document->streamingRegions.empty())
							AE_UNEXPECTED(AetherError::Asset("Invalid prepared browser scene entity"));
						description.entities.push_back(std::move(document->entities.front()));
					}
					if (!activation.restoreGateNode)
					{
						auto& authored = description.entities[activation.cursor];
						authored.nodeId = scene::StreamingNodeId(activation.region.id, authored.sourceNodeId);
					}
					if (activation.restoreGateNode && description.entities[activation.cursor].nodeId == activation.restoreGateNode)
					{
						const auto& gateRecord = description.entities[activation.cursor];
						if (activation.restoredGateSeen || gateRecord.parentIndex >= 0 || !gateRecord.scripts.empty())
							AE_UNEXPECTED(AetherError::Asset("Invalid restored streaming gate"));
						activation.restoredGateSeen = true;
						world.Destroy(activation.region.activationGate);
						activation.nodes.erase(activation.restoreGateNode);
						activation.region.activationGate = {};
					}
					auto record = description.entities[activation.cursor];
					if (activation.restoreGateNode)
					{
						if (!record.nodeId) AE_UNEXPECTED(AetherError::Asset("Restored streaming entity has no stable node"));
						const std::function<void(reflect::FieldValue&)> clear = [&](reflect::FieldValue& value)
						{
							if (value.type == reflect::FieldType::EntityRef) value.entity = 0;
							for (auto& row: value.list) for (auto& child: row) clear(child);
						};
						for (auto& component: record.reflected) for (auto& [name, value]: component.fields) clear(value);
					}
					record.parentIndex = -1;
					record.joint2D.reset();
					record.joint.reset();
					record.scriptJoints.clear();
					record.disabled = true;
					slice.entities.push_back(std::move(record));
				}
				else slice.lights.push_back(description.lights[activation.cursor - authoredCount]);
				StreamingModelScope scope(m_models);
				AE_TRY(created, Apply(slice));
				if (created->size() != 1) AE_UNEXPECTED(AetherError::Asset("Invalid streaming entity activation"));
				if (!world.Has<DisabledComponent>((*created)[0])) world.Emplace<DisabledComponent>((*created)[0]);
				if (activation.cursor >= authoredCount) ecs::SetParent(world, (*created)[0], activation.region.activationGate);
				entities.push_back((*created)[0]);
				if (activation.restoreGateNode && activation.cursor < authoredCount &&
				        description.entities[activation.cursor].nodeId == activation.restoreGateNode)
					activation.region.activationGate = (*created)[0];
				if (activation.restoreGateNode && activation.cursor < authoredCount)
				{
					const auto node = description.entities[activation.cursor].nodeId;
					if (!activation.nodes.emplace(node, (*created)[0]).second)
						AE_UNEXPECTED(AetherError::Asset("Duplicate restored streaming node"));
				}
				++activation.cursor;
				--entityBudget;
				continue;
			}
			if (activation.phase == 1)
			{
				if (activation.cursor == authoredCount)
				{
					activation.cursor = 0;
					activation.phase = 2;
					continue;
				}
				const auto i = activation.cursor++;
				const auto& record = description.entities[i];
				const auto entity = entities[i];
				if (entity != activation.region.activationGate)
				{
					if (record.parentIndex >= 0) ecs::SetParent(world, entity, entities[record.parentIndex]);
					else ecs::SetParent(world, entity, activation.region.activationGate);
				}
				{
					const auto& referenceIndex = *activation.fieldNodes;
					for (const auto& generic: record.reflected)
					{
						const auto* type = reflect::FindComponentType(generic.type);
						auto* component = type && type->tryGetRaw ? type->tryGetRaw(world, entity) : nullptr;
						if (!component) continue;
						for (const auto& [name, saved]: generic.fields)
						{
							if (const auto* field = type->FindField(name))
							{
								if (auto result = scene::ApplyStreamingFieldReference(world, entity, *type, *field, component, saved, referenceIndex); !result)
									return std::unexpected(result.error());
							}
						}
						if (type->postSet) type->postSet(world, entity);
					}
				}
				if (record.joint2D && ResolvePhysicsDomain(record, description.kind).apply2D)
				{
					auto joint = *record.joint2D;
					if (record.joint2DTargetIndex >= 0 && static_cast<std::size_t>(record.joint2DTargetIndex) < authoredCount)
						joint.target = entities[record.joint2DTargetIndex];
					world.Emplace<Joint2DComponent>(entity, joint);
				}
				if (ResolvePhysicsDomain(record, description.kind).apply3D)
				{
					if (record.joint)
					{
						const auto& joint = *record.joint;
						Entity target{};
						if (joint.targetIndex >= 0 && static_cast<std::size_t>(joint.targetIndex) < authoredCount) target = entities[joint.targetIndex];
						world.Emplace<JointComponent>(entity, JointComponent{.type = joint.type, .target = target, .anchor = joint.anchor,
						        .axis = joint.axis, .minLimit = joint.minLimit, .maxLimit = joint.maxLimit, .distance = joint.distance, .collideConnected = joint.collideConnected});
					}
					for (const auto& joint: record.scriptJoints)
					{
						JointEntry entry;
						if (joint.targetIndex >= 0 && static_cast<std::size_t>(joint.targetIndex) < authoredCount) entry.target = entities[joint.targetIndex];
						entry.type = joint.type; entry.anchor = joint.anchor; entry.axis = joint.axis; entry.minLimit = joint.minLimit;
						entry.maxLimit = joint.maxLimit; entry.distance = joint.distance; entry.swingLimit = joint.swingLimit; entry.collideConnected = joint.collideConnected;
						m_physics->AddScriptJoint(world, entity, entry);
					}
				}
				AE_TRY(scripts, ResolveScripts(description, entities, i, i + 1));
				activation.region.scripts.insert(activation.region.scripts.end(), std::make_move_iterator(scripts->begin()), std::make_move_iterator(scripts->end()));
				--entityBudget;
				continue;
			}
			if (activation.cursor == entities.size())
			{
				world.SetSceneFeatures(world.GetSceneFeatures() | description.features);
				static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem"))->FlushPendingOnly(world);
				m_physics->FlushPendingOnly(world);
				if (!activation.restoreGateNode) entities.push_back(activation.region.activationGate);
				activation.phase = 3;
				break;
			}
			const auto i = activation.cursor++;
			if (entities[i] != activation.region.activationGate && (i >= authoredCount || !description.entities[i].disabled)) world.Remove<DisabledComponent>(entities[i]);
			--entityBudget;
		}
		if (activation.phase == 3 && activation.documentCharge)
		{
			activation.description = {};
			activation.budget->Release(streaming::MemoryClass::CpuPending, activation.documentCharge);
			activation.documentCharge = 0;
		}
		return activation.phase == 3;
	}

	void WebScene::CancelRegion(WebRegionActivation& activation)
	{
		if (activation.region.activationGate.IsValid() && world.GetRegistry().valid(World::ToEntt(activation.region.activationGate)))
			world.Destroy(activation.region.activationGate);
		DetachRegion(activation.region);
		activation.description = {};
		if (activation.documentCharge)
		{
			activation.budget->Release(streaming::MemoryClass::CpuPending, activation.documentCharge);
			activation.documentCharge = 0;
		}
	}


	void WebScene::DetachRegion(WebRegion& region)
	{
		for (const auto entity: region.entities)
		{
			if (world.GetRegistry().valid(World::ToEntt(entity)))
				world.Destroy(entity);
		}
		m_models.ReleaseUnused(world);
		region.entities.clear();
		region.scripts.clear();
	}


	// Resolves each script's entity / authored-node references to live entities, as a scene load does.
	Expected<std::vector<WebScriptRecord>> WebScene::ResolveScripts(const scene::SceneDescription& description, const std::vector<Entity>& created, std::size_t begin, std::size_t end)
	{
		std::vector<WebScriptRecord> scripts;
		for (size_t i = begin; i < std::min(end, description.entities.size()); ++i)
		{
			if (description.entities[i].scripts.empty())
			{
				continue;
			}
			ScriptComponent component;
			component.scripts.reserve(description.entities[i].scripts.size());
			for (const auto& script: description.entities[i].scripts)
			{
				auto resolved = script;
				resolved.properties = scene::detail::ScriptPropsFromSceneRefs(script.properties, created);
				resolved.properties.erase("Self");
				for (const auto& [property, node]: script.nodeRefs)
				{
					if (property == "Self")
					{
						continue;
					}
					const auto found = std::find_if(description.entities.begin(), description.entities.end(), [node](const auto& record) { return record.nodeId == node || record.sourceNodeId == node; });
					if (found != description.entities.end())
					{
						const auto index = std::distance(description.entities.begin(), found);
						resolved.properties[property].i64 = created[index].id;
						resolved.nodeRefs[property] = found->nodeId;
					}
					else
					{
						AE_TRY(target, scene::ResolveSceneNodeReference(world, node));
						resolved.properties[property].i64 = target->id;
						if (target->IsValid()) resolved.nodeRefs[property] = world.Get<SceneNodeComponent>(*target).id;
					}
				}
				const auto scriptIndex = static_cast<std::uint32_t>(component.scripts.size());
				component.scripts.push_back(ScriptEntry{.path = resolved.type, .attached = false, .properties = resolved.properties});
				scripts.push_back({created[i], std::move(resolved), scriptIndex});
			}
			world.Emplace<ScriptComponent>(created[i], std::move(component));
		}
		return scripts;
	}

	Expected<WebInstantiated> WebScene::Instantiate(std::string_view name, glm::vec3 position)
	{
		const std::string path = ProjectPath("prefabs", name, ".prefab.toml");
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid prefab name or reference"));
		}
		AE_TRY(text, io::FileSystem::ReadFileText(path));
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
		// Native InstantiatePrefab: the authored root is re-placed at the requested translation
		// (identity rotation/scale), the whole subtree moving with it. A prefab never redefines the
		// scene kind - Apply does not touch it.
		const Entity rootEntity = (*created)[rootIndex];
		ecs::SetWorldTransform(world, rootEntity, glm::translate(glm::mat4(1.0f), position));
		// 2D bodies built while the subtree still sat at the prefab's base pose are rebuilt from the
		// final pose (SceneSerializerApply's RebuildSubtreeBodies2D); 3D bodies are created by the
		// next PhysicsSystem flush, which already sees the final pose.
		if (auto* physics2D = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
		{
			const std::function<void(Entity)> rebuild = [&](Entity entity)
			{
				if (world.Has<RigidBody2DComponent>(entity))
				{
					physics2D->RebuildBody(world, entity);
				}
				if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
				{
					const auto children = hierarchy->children;
					for (const Entity child: children)
					{
						rebuild(child);
					}
				}
			};
			rebuild(rootEntity);
		}
		AE_TRY(scripts, ResolveScripts(*description, *created));
		return WebInstantiated{rootEntity, std::move(*created), std::move(*scripts)};
	}

	std::vector<Entity> WebScene::TakeRetiredParticles()
	{
		std::vector<Entity> retired;
		retired.swap(m_retired_particles);
		return retired;
	}

	Expected<void> WebScene::ValidateEffects()
	{
		std::uint64_t particleCapacity = 0;
		std::uint64_t spawnWork = 0;
		for (const auto raw: world.View<ParticleEmitterComponent>())
		{
			const auto entity = World::FromEntt(raw);
			const auto& emitter = world.Get<ParticleEmitterComponent>(entity);
			if (const auto* transform = world.TryGet<TransformComponent>(entity))
			{
				for (unsigned column = 0; column < 4; ++column)
				{
					for (unsigned row = 0; row < 4; ++row)
					{
						if (!std::isfinite(transform->localToWorld[column][row]))
						{
							AE_UNEXPECTED(AetherError::Asset("Non-finite browser particle transform"));
						}
					}
				}
			}
			const auto valid = ValidateBrowserParticleEmitter(emitter);
			if (!valid)
			{
				AE_UNEXPECTED(AetherError::Asset(valid.error()));
			}
			particleCapacity += emitter.maxParticles;
			spawnWork += emitter.pendingBurst + (emitter.emitOnStart && !emitter.started ? static_cast<std::uint64_t>(emitter.burstCount) : 0);
			spawnWork += static_cast<std::uint64_t>(std::ceil(emitter.spawnAccumulator + emitter.rate / 60.0f));
			if (particleCapacity > kWebParticleCapacity || spawnWork > kWebParticleCapacity)
			{
				AE_UNEXPECTED(AetherError::Asset("Browser aggregate particle capacity or spawn-work budget exceeded (65536)"));
			}
		}
		return {};
	}

	void WebScene::Tick(float dt)
	{
		// Skin clocks advance in the shared AnimationSystem, not here.
		m_light_time += dt;
	}

	Expected<void> WebScene::ExtractLighting(RenderFramePacket& packet)
	{
		// Authored 3D scenes use native startup lighting even without environment/light records.
		// Explicit mesh-only fixtures retain the legacy unlit path; 2D scene defaults do not opt in.
		packet.renderLightingEnabled |= m_default_lighting;
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
			packet.shadingParams.y = env.cloudCoverage;
			packet.shadingParams.z = env.cloudSpeed;
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
			packet.fogParams = glm::vec4(sky.fogDensity, sky.fogHeightFalloff, sky.fogSunScatter, sky.fogMaxOpacity);
			packet.skyParams = glm::vec4(sky.physicalSky ? 1.0f : 0.0f, sky.turbidity, sky.volumetricFog ? sky.volumetricIntensity : 0.0f, sky.volumetricAnisotropy);
			packet.shadingParams.y = sky.cloudCoverage;
			packet.shadingParams.z = sky.cloudSpeed;
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
		std::vector<DarknessVolumeSource> darkness;
		for (const auto raw: world.View<DarknessVolumeComponent, TransformComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (!world.Has<DisabledComponent>(entity) && !ecs::HasDisabledAncestor(world, entity))
			{
				darkness.push_back({world.Get<TransformComponent>(entity).localToWorld, world.Get<DarknessVolumeComponent>(entity).fadeDepth});
			}
		}
		ExtractDarknessVolumes(darkness, glm::vec3(packet.cameraWorldPos), kMaxDarknessVolumes, packet.darknessVolumes);
		return {};
	}

	Expected<void> WebScene::Extract(RenderFramePacket& packet, unsigned width, unsigned height, float elapsed)
	{
		AE_TRY_VOID(ValidateEffects());
		AE_TRY_VOID(ExtractPostSettings(packet));
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
			packet.dofParams = ExtractDepthOfFieldParams(camera.focusDistance, camera.focalLengthMm, camera.aperture, camera.depthOfField, camera.projection == CameraProjection::Perspective);
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
		packet.shadingParams.x = packet.postEffects.specularFilter;
		packet.shadingParams.w = packet.postEffects.foliageSkyLook;
		m_sprite_system.Extract(world, packet.render2D);
		AE_TRY_VOID(m_models.Extract(world, packet));
		const auto beforeTiles = packet.render2D.sprites.size();
		m_tile_system.Extract(world, {}, elapsed, packet.render2D);
		EM_ASM({ aetherRuntime.tileSprites = $0; }, packet.render2D.sprites.size() - beforeTiles);
		auto* particles = static_cast<ParticleSystem*>(world.FindSystem("ParticleSystem"));
		particles->SetBillboardTarget(&packet.render2D.billboards);
		particles->Extract(world, packet.render2D);
		particles->SetBillboardTarget(nullptr);
		packet.renderSceneGeneration = m_scene_generation;
		// Display-space particles still need opaque depth and the final composite target.
		if (!packet.render2D.billboards.empty())
		{
			packet.renderLightingEnabled = true;
		}
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
