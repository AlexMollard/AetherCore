#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

#include "scene/SceneSerializerDetail.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		void AppendCaptureOrder(World& world, Entity entity, std::vector<Entity>& order, std::unordered_set<std::uint32_t>& seen)
		{
			if (!entity.IsValid() || seen.contains(entity.id) || !world.GetRegistry().valid(World::ToEntt(entity)) || ecs::HasSceneTransientAncestor(world, entity))
			{
				return;
			}

			seen.insert(entity.id);
			order.push_back(entity);
			if (const auto* h = world.TryGet<HierarchyComponent>(entity))
			{
				for (const Entity child: h->children)
				{
					AppendCaptureOrder(world, child, order, seen);
				}
			}
		}

		void AppendEntityRecords(SceneDescription& scene, World& world, const std::vector<Entity>& order, const std::unordered_map<std::uint32_t, int>& indexOf, const MaterialRegistry& materials, const TextureRegistry& textures)
		{
			scene.entities.reserve(scene.entities.size() + order.size());
			for (const Entity e: order)
			{
				EntityRecord rec;
				rec.entityId = e.id;
				if (const auto* nc = world.TryGet<NameComponent>(e))
				{
					rec.name = nc->name;
				}
				ForEachTag(
				        [&](const std::string& tagName, std::uint32_t tagId)
				        {
					        if (TagHas(&world, e.id, tagId))
					        {
						        rec.tags.push_back(tagName);
					        }
				        });
				rec.disabled = world.Has<DisabledComponent>(e);
				if (const auto* sprite = world.TryGet<SpriteRendererComponent>(e))
				{
					rec.sprite = *sprite;
				}
				if (const auto* animator = world.TryGet<SpriteAnimatorComponent>(e))
				{
					rec.spriteAnimator = *animator;
				}
				if (const auto* mr = world.TryGet<MeshRendererComponent>(e))
				{
					rec.meshRenderer = true;
					rec.meshRendererVisible = mr->visible;
					rec.meshRendererCastShadows = mr->castShadows;
				}
				if (const auto* tc = world.TryGet<TransformComponent>(e))
				{
					rec.hasTransform = true;
					DecomposeTRS(tc->localToWorld, rec.position, rec.eulerDeg, rec.scale);
				}
				if (const auto* h = world.TryGet<HierarchyComponent>(e); h != nullptr && h->parent.IsValid())
				{
					const auto it = indexOf.find(h->parent.id);
					rec.parentIndex = it != indexOf.end() ? it->second : -1;
				}
				if (const auto* ms = world.TryGet<MeshSourceComponent>(e))
				{
					rec.mesh = *ms;
				}
				if (const auto* mc = world.TryGet<MaterialComponent>(e))
				{
					MaterialRecord mat;
					bool haveAsset = false;
					if (const auto* inst = world.TryGet<MaterialInstanceComponent>(e))
					{
						mat.asset = inst->asset;
						haveAsset = true;
					}
					else
					{
						haveAsset = materials.TryDescribe(mc->handle, mat.asset);
					}
					if (haveAsset)
					{
						const auto pathOf = [&textures](TextureHandle h, std::string& out)
						{
							if (h.IsValid() && h.index != TextureHandle::kBrokenIndex)
							{
								textures.TryGetPath(h, out);
							}
						};
						pathOf(mat.asset.albedoTex, mat.albedoPath);
						pathOf(mat.asset.normalTex, mat.normalPath);
						pathOf(mat.asset.metallicRoughnessTex, mat.metallicRoughnessPath);
						pathOf(mat.asset.occlusionTex, mat.occlusionPath);
						pathOf(mat.asset.emissiveTex, mat.emissivePath);
						rec.material = std::move(mat);
					}
				}
				if (const auto* smc = world.TryGet<SkinnedMeshComponent>(e))
				{
					rec.skinned = SkinnedRecord{.clipIndex = smc->clipIndex, .animTime = smc->animTime, .playbackSpeed = smc->playbackSpeed, .looping = smc->looping};
				}
				if (const auto* col = world.TryGet<ColliderComponent>(e))
				{
					PhysicsRecord pr{.shapeType = col->shape, .halfExtents = col->halfExtents, .radius = col->radius, .halfHeight = col->halfHeight};
					pr.center = col->center;
					pr.friction = col->friction;
					pr.restitution = col->restitution;
					pr.isSensor = col->isSensor;
					if (const auto* rb = world.TryGet<RigidBodyComponent>(e))
					{
						pr.motionType = rb->motionType;
						pr.mass = rb->mass;
						pr.linearDamping = rb->linearDamping;
						pr.angularDamping = rb->angularDamping;
						pr.gravityFactor = rb->gravityFactor;
						pr.maxLinearVelocity = rb->maxLinearVelocity;
						pr.maxAngularVelocity = rb->maxAngularVelocity;
						pr.continuousCollision = rb->continuousCollision;
						pr.allowSleeping = rb->allowSleeping;
						pr.lockPosition = rb->lockPosition;
						pr.lockRotation = rb->lockRotation;
					}
					else
					{
						pr.motionType = PhysicsMotionType::Static;
					}
					rec.physics = pr;
				}
				if (const auto* j = world.TryGet<JointComponent>(e))
				{
					JointRecord jr;
					jr.type = j->type;
					if (j->target.IsValid())
					{
						const auto it = indexOf.find(j->target.id);
						jr.targetIndex = it != indexOf.end() ? it->second : -1;
					}
					jr.anchor = j->anchor;
					jr.axis = j->axis;
					jr.minLimit = j->minLimit;
					jr.maxLimit = j->maxLimit;
					jr.distance = j->distance;
					jr.collideConnected = j->collideConnected;
					rec.joint = jr;
				}
				if (const auto* rb2d = world.TryGet<RigidBody2DComponent>(e))
				{
					RigidBody2DComponent copy = *rb2d;
					copy.body = {};
					rec.rigidBody2D = std::move(copy);
				}
				if (const auto* col2d = world.TryGet<Collider2DComponent>(e))
				{
					Collider2DComponent copy = *col2d;
					copy.shapes.clear();
					rec.collider2D = std::move(copy);
				}
				if (const auto* j2d = world.TryGet<Joint2DComponent>(e))
				{
					Joint2DComponent copy = *j2d;
					copy.jointId = 0;
					if (copy.target.IsValid())
					{
						const auto it = indexOf.find(copy.target.id);
						rec.joint2DTargetIndex = it != indexOf.end() ? it->second : -1;
					}
					copy.target = {};
					rec.joint2D = std::move(copy);
				}
				if (const auto* c = world.TryGet<ui::UICanvas>(e))
				{
					rec.uiCanvas = UICanvasRecord{static_cast<std::uint8_t>(c->scaleMode), c->referenceResolution, c->sortBias};
				}
				if (const auto* r = world.TryGet<ui::UIRect>(e))
				{
					rec.uiRect = UIRectRecord{r->anchorMin, r->anchorMax, r->offsetMin, r->offsetMax, r->pivot};
				}
				if (const auto* im = world.TryGet<ui::UIImage>(e))
				{
					UIImageRecord ir;
					ir.color = im->color;
					ir.cornerRadius = im->cornerRadius;
					ir.pixelArt = im->pixelArt;
					if (im->texture.IsValid() && im->texture.index != TextureHandle::kBrokenIndex)
					{
						textures.TryGetPath(im->texture, ir.texturePath);
					}
					rec.uiImage = std::move(ir);
				}
				if (const auto* tx = world.TryGet<ui::UIText>(e))
				{
					rec.uiText = UITextRecord{tx->text, tx->fontName, tx->pixelSize, tx->color, static_cast<std::uint8_t>(tx->hAlign), static_cast<std::uint8_t>(tx->vAlign), tx->wrap};
				}
				if (const auto* er = world.TryGet<EffectRefComponent>(e))
				{
					EffectRecord fx;
					fx.name = er->name;
					if (const auto* ep = world.TryGet<EffectParamsComponent>(e))
					{
						fx.params = ep->params;
					}
					rec.effect = std::move(fx);
				}
				// Pure data-only components, captured generically from the reflection
				// registry. Only reflected (authored) fields are stored, so runtime
				// state - baseCaptured/time/base etc. - is naturally left out and
				// resurrected as defaults on apply (what the old per-type "clean" did).
				for (const std::string& typeName: GenericComponentTypeNames())
				{
					const reflect::ComponentType* ct = reflect::FindComponentType(typeName);
					if (ct == nullptr)
					{
						continue;
					}
					const void* comp = ct->tryGetRawConst(world, e);
					if (comp == nullptr)
					{
						continue;
					}
					GenericComponent generic;
					generic.type = typeName;
					generic.fields.reserve(ct->fields.size());
					for (const reflect::FieldDesc& field: ct->fields)
					{
						generic.fields.emplace_back(field.name, field.get(comp));
					}
					rec.reflected.push_back(std::move(generic));
				}
				if (const auto* particles = world.TryGet<ParticleEmitterComponent>(e))
				{
					ParticleEmitterComponent clean = *particles;
					clean.particles.clear();
					clean.spawnAccumulator = 0.0f;
					clean.rngState = 0;
					clean.started = false;
					clean.pendingBurst = 0;
					rec.particles = std::move(clean);
				}
				if (const auto* pl = world.TryGet<PointLightComponent>(e))
				{
					rec.pointLight = *pl;
				}
				if (const auto* sl = world.TryGet<SpotLightComponent>(e))
				{
					rec.spotLight = *sl;
				}
				if (const auto* dn = world.TryGet<DayNightComponent>(e))
				{
					rec.dayNight = *dn;
				}
				if (const auto* tm = world.TryGet<TileMapComponent>(e))
				{
					rec.tileMap = *tm;
				}
				if (const auto* cam = world.TryGet<CameraComponent>(e))
				{
					rec.camera = *cam;
					rec.mainCamera = world.Has<MainCameraComponent>(e);
				}
				if (const auto* orbit = world.TryGet<OrbitCameraComponent>(e))
				{
					rec.orbitCamera = *orbit;
				}
				if (const auto* script = world.TryGet<ScriptComponent>(e); script != nullptr)
				{
					for (const ScriptEntry& entry: script->scripts)
					{
						if (entry.path.empty())
						{
							continue;
						}
						rec.scripts.push_back(ScriptRecord{.type = entry.path, .properties = ScriptPropsToSceneRefs(entry.properties, indexOf)});
					}
				}
				scene.entities.push_back(std::move(rec));
			}

			std::unordered_set<std::string> seenAssetIds;
			const auto addManifest = [&](const AssetSource& src)
			{
				const std::string idHex = ComputeAssetId(src).ToHex();
				if (!seenAssetIds.insert(idHex).second)
				{
					return;
				}
				scene.assetManifest.push_back(AssetManifestEntry{.id = idHex, .type = (src.type == AssetType::Mesh ? "mesh" : "texture"), .path = src.path, .subIndex = src.subIndex, .builtin = src.builtin});
			};
			for (const EntityRecord& rec: scene.entities)
			{
				if (rec.mesh)
				{
					addManifest(rec.mesh->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(rec.mesh->path) : MakeModelMeshSource(rec.mesh->path, static_cast<int>(rec.mesh->primitiveIndex)));
				}
				if (rec.material)
				{
					for (const std::string& p: {rec.material->albedoPath, rec.material->normalPath, rec.material->metallicRoughnessPath, rec.material->occlusionPath, rec.material->emissivePath})
					{
						if (!p.empty())
						{
							addManifest(MakeTextureSource(p));
						}
					}
				}
				if (rec.uiImage && !rec.uiImage->texturePath.empty())
				{
					addManifest(MakeTextureSource(rec.uiImage->texturePath));
				}
			}
		}
	} // namespace

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer)
	{
		SceneDescription scene;
		scene.kind = world.GetSceneKind();
		scene.features = world.GetSceneFeatures();
		auto& reg = world.GetRegistry();

		// Punctual lights are entities now (LightComponents.hpp) and serialize
		if (renderer != nullptr)
		{
			EnvironmentRecord env;
			env.ambient = renderer->GetAmbientLight();
			env.sunDirection = renderer->GetDirectionalLightDirection();
			env.sunIntensity = renderer->GetDirectionalLightIntensity();
			env.sunColor = renderer->GetSunColor();
			env.skyHorizon = renderer->GetSkyHorizonColor();
			env.skyZenith = renderer->GetSkyZenithColor();
			env.skyVoid = renderer->GetSkyVoidColor();
			scene.environment = env;
		}

		// - excluded so boot auto-generation and Play snapshots never duplicate
		std::vector<Entity> order;
		std::unordered_set<std::uint32_t> seen;
		for (const Entity root: world.Roots())
		{
			AppendCaptureOrder(world, root, order, seen);
		}

		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (!e.IsValid() || ecs::HasSceneTransientAncestor(world, e))
			{
				continue;
			}
			const auto* h = world.TryGet<HierarchyComponent>(e);
			if (!seen.contains(e.id) && (!h || !h->parent.IsValid()))
			{
				AppendCaptureOrder(world, e, order, seen);
			}
		}

		std::unordered_map<std::uint32_t, int> indexOf;
		for (std::size_t i = 0; i < order.size(); ++i)
		{
			indexOf[order[i].id] = static_cast<int>(i);
		}

		AppendEntityRecords(scene, world, order, indexOf, materials, textures);
		return scene;
	}

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		SceneDescription desc;
		std::vector<Entity> order;
		for (const Entity root: roots)
		{
			const std::size_t start = order.size();
			order.push_back(root);
			for (std::size_t i = start; i < order.size(); ++i)
			{
				if (const auto* h = world.TryGet<HierarchyComponent>(order[i]))
				{
					order.insert(order.end(), h->children.begin(), h->children.end());
				}
			}
		}
		std::unordered_map<std::uint32_t, int> indexOf;
		for (std::size_t i = 0; i < order.size(); ++i)
		{
			indexOf[order[i].id] = static_cast<int>(i);
		}
		AppendEntityRecords(desc, world, order, indexOf, materials, textures);
		return desc;
	}

	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures)
	{
		SceneDescription prefab = CaptureSubtrees(world, {root}, materials, textures);
		if (const auto* nc = world.TryGet<NameComponent>(root))
		{
			prefab.name = nc->name;
		}
		return prefab;
	}
} // namespace aether::app::scene
