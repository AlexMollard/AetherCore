#include "editor/ComponentCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <glm/glm.hpp>

#include "assets/AssetManager.hpp"
#include "debug/Icons.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scene/TransformUtils.hpp"
#include "ui/UiComponents.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	namespace
	{
		glm::vec3 EntityPosition(const World& w, Entity e)
		{
			if (const auto* t = w.TryGet<TransformComponent>(e))
			{
				return glm::vec3(t->localToWorld[3]);
			}
			return glm::vec3(0.0f);
		}

		glm::vec3 EntityScale(const World& w, Entity e)
		{
			if (const auto* t = w.TryGet<TransformComponent>(e))
			{
				const glm::mat4& m = t->localToWorld;
				return aether::ExtractScale(m);
			}
			return glm::vec3(1.0f);
		}

		void EnsureTransform(World& w, Entity e)
		{
			if (!w.Has<TransformComponent>(e))
			{
				w.Emplace<TransformComponent>(e);
			}
		}

		// Default material for a freshly-added primitive that has none of its own yet:
		// a prototype grid (Kenney CC0 "Prototype Textures", see resources/CREDITS.md)
		// instead of a flat colour - the standard greybox affordance, so a cube or
		// sphere's scale and orientation read at a glance instead of everything looking
		// like the same grey blob. "engine://" so it resolves from data/engine.pak in a
		// published build the same as a loose dev tree (see EngineAssetsPak).
		constexpr const char* kDefaultPrimitiveTexturePath = "engine://textures/prototype/grid_light.texture";

		void AddMeshPrimitive(World& w, Entity e, ServiceContainer& s, PrimitiveMesh kind, const char* pathName)
		{
			auto* prims = s.TryGet<PrimitiveMeshes>();
			auto* assets = s.TryGet<AssetManager>();
			if (prims == nullptr || assets == nullptr)
			{
				return;
			}
			EnsureTransform(w, e);
			w.EmplaceOrReplace<MeshComponent>(e, MeshComponent{.mesh = &prims->Get(kind)});
			w.EmplaceOrReplace<MeshSourceComponent>(e, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = pathName, .primitiveIndex = 0});
			// Guarded exactly like the flat colour it replaces: an entity that already
			// carries a MaterialComponent (scene-authored, or a second "Add" over a prop
			// someone already textured) never reaches this branch, so this only restyles
			// entities with no material of their own.
			if (!w.Has<MaterialComponent>(e))
			{
				TextureRegistry& textures = assets->GetTextureRegistry();
				const TextureHandle grid = textures.Acquire(kDefaultPrimitiveTexturePath, TextureColorSpace::Srgb);
				MaterialAsset asset{};
				asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				asset.roughnessFactor = 0.6f;
				asset.doubleSided = true;
				asset.albedoTex = grid;
				MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
				if (grid.IsValid())
				{
					textures.Release(grid);
				}
			}
			w.EmplaceOrReplace<MeshRendererComponent>(e);
		}

		void RemoveMeshBundle(World& w, Entity e)
		{
			w.Remove<MeshRendererComponent>(e);
			w.Remove<SpriteRendererComponent>(e);
			w.Remove<MeshComponent>(e);
			w.Remove<MeshSourceComponent>(e);
		}

		void AddCollider(World& w, Entity e, PhysicsShapeType shape)
		{
			const glm::vec3 scale = EntityScale(w, e);
			ColliderComponent c{};
			c.shape = shape;
			c.halfExtents = scale * 0.5f;
			c.radius = shape == PhysicsShapeType::Sphere ? std::max({scale.x, scale.y, scale.z}) * 0.5f : scale.x * 0.5f;
			c.halfHeight = scale.y * 0.5f;
			w.Emplace<ColliderComponent>(e, c);
		}

		template<typename T>
		ComponentCatalogEntry Simple(std::string name, std::string category, std::string icon, const T& value = T{})
		{
			return ComponentCatalogEntry{std::move(name),
			        std::move(category),
			        std::move(icon),
			        [](const World& w, Entity e) { return w.Has<T>(e); },
			        [value](World& w, Entity e, ServiceContainer&)
			        {
				        if constexpr (std::is_empty_v<T>)
				        {
					        (void) value;
					        w.EmplaceOrReplace<T>(e);
				        }
				        else
				        {
					        w.EmplaceOrReplace<T>(e, value);
				        }
			        },
			        [](World& w, Entity e) { w.Remove<T>(e); }};
		}

		std::vector<ComponentCatalogEntry> Build()
		{
			std::vector<ComponentCatalogEntry> c;

			c.push_back(Simple<NameComponent>("Name", "Core", ICON_FA_TAG, NameComponent{.name = "Entity"}));
			c.push_back(Simple<HierarchyComponent>("Hierarchy", "Core", ICON_FA_SITEMAP));

			const auto meshEntry = [](std::string name, std::string icon, PrimitiveMesh kind, const char* path)
			{
				return ComponentCatalogEntry{std::move(name),
				        "Rendering",
				        std::move(icon),
				        [](const World& w, Entity e) { return w.Has<MeshComponent>(e); },
				        [kind, path](World& w, Entity e, ServiceContainer& s) { AddMeshPrimitive(w, e, s, kind, path); },
				        [](World& w, Entity e) { RemoveMeshBundle(w, e); }};
			};
			c.push_back(meshEntry("Cube", ICON_FA_CUBE, PrimitiveMesh::Cube, "cube"));
			c.push_back(meshEntry("Sphere", ICON_FA_CIRCLE, PrimitiveMesh::Sphere, "sphere"));
			c.push_back(meshEntry("Plane", ICON_FA_IMAGE, PrimitiveMesh::Plane, "plane"));
			c.push_back(meshEntry("Quad", ICON_FA_IMAGE, PrimitiveMesh::Quad, "quad"));
			c.push_back(meshEntry("Triangle", ICON_FA_PLAY, PrimitiveMesh::Triangle, "triangle"));

			c.push_back(ComponentCatalogEntry{"Sprite Renderer",
			        "Rendering",
			        ICON_FA_IMAGE,
			        [](const World& w, Entity e) { return w.Has<SpriteRendererComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        EnsureTransform(w, e);
				        w.EmplaceOrReplace<SpriteRendererComponent>(e);
			        },
			        [](World& w, Entity e) { w.Remove<SpriteRendererComponent>(e); }});

			c.push_back(ComponentCatalogEntry{"Sprite Animator",
			        "Animation",
			        ICON_FA_FILM,
			        [](const World& w, Entity e) { return w.Has<SpriteAnimatorComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        EnsureTransform(w, e);
				        if (!w.Has<SpriteRendererComponent>(e))
				        {
					        w.Emplace<SpriteRendererComponent>(e);
				        }
				        w.EmplaceOrReplace<SpriteAnimatorComponent>(e);
			        },
			        [](World& w, Entity e) { w.Remove<SpriteAnimatorComponent>(e); }});

			c.push_back(ComponentCatalogEntry{"Material",
			        "Rendering",
			        ICON_FA_PALETTE,
			        [](const World& w, Entity e) { return w.Has<MaterialComponent>(e); },
			        [](World& w, Entity e, ServiceContainer& s)
			        {
				        auto* assets = s.TryGet<AssetManager>();
				        if (assets == nullptr)
				        {
					        return;
				        }
				        MaterialAsset asset{};
				        asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				        asset.roughnessFactor = 0.6f;
				        MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			        },
			        [](World& w, Entity e) { w.Remove<MaterialComponent>(e); }});

			c.push_back(ComponentCatalogEntry{"Sky",
			        "Rendering",
			        ICON_FA_CLOUD,
			        [](const World& w, Entity e) { return w.Has<SkyComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { w.EmplaceOrReplace<SkyComponent>(e); },
			        [](World& w, Entity e) { w.Remove<SkyComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Point Light",
			        "Rendering",
			        ICON_FA_LIGHTBULB,
			        [](const World& w, Entity e) { return w.Has<PointLightComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        EnsureTransform(w, e);
				        w.EmplaceOrReplace<PointLightComponent>(e);
			        },
			        [](World& w, Entity e) { w.Remove<PointLightComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Spot Light",
			        "Rendering",
			        ICON_FA_LIGHTBULB,
			        [](const World& w, Entity e) { return w.Has<SpotLightComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        EnsureTransform(w, e);
				        w.EmplaceOrReplace<SpotLightComponent>(e);
			        },
			        [](World& w, Entity e) { w.Remove<SpotLightComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Camera",
			        "Rendering",
			        ICON_FA_VIDEO,
			        [](const World& w, Entity e) { return w.Has<CameraComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        EnsureTransform(w, e);
				        w.EmplaceOrReplace<CameraComponent>(e);
			        },
			        [](World& w, Entity e) { w.Remove<CameraComponent>(e); }});

			c.push_back(Simple<BobComponent>("Bob", "Behaviors", ICON_FA_WAVE_SQUARE, BobComponent{.amplitude = 1.5f, .frequency = 0.8f}));
			c.push_back(Simple<SpinComponent>("Spin", "Behaviors", ICON_FA_ROTATE, SpinComponent{.eulerDegPerSec = {0.0f, 40.0f, 0.0f}}));
			c.push_back(ComponentCatalogEntry{"Orbit",
			        "Behaviors",
			        ICON_FA_CIRCLE_NOTCH,
			        [](const World& w, Entity e) { return w.Has<OrbitComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        const glm::vec3 pos = EntityPosition(w, e);
				        const float radius = std::max(std::sqrt(pos.x * pos.x + pos.z * pos.z), 3.0f);
				        w.EmplaceOrReplace<OrbitComponent>(e, OrbitComponent{.center = {0.0f, 0.0f, 0.0f}, .radius = radius, .angularSpeedDeg = 30.0f, .angleDeg = glm::degrees(std::atan2(pos.z, pos.x)), .yawOffsetDeg = 0.0f, .height = pos.y});
			        },
			        [](World& w, Entity e) { w.Remove<OrbitComponent>(e); }});
			c.push_back(Simple<MaterialPulseComponent>("Material Pulse", "Behaviors", ICON_FA_HEART_PULSE, MaterialPulseComponent{.emissiveA = {0.0f, 0.0f, 0.05f}, .emissiveB = {0.9f, 0.2f, 0.05f}, .frequency = 2.0f}));
			c.push_back(Simple<ScalePulseComponent>("Scale Pulse", "Behaviors", ICON_FA_EXPAND, ScalePulseComponent{.amplitude = 0.2f, .frequency = 2.0f}));
			c.push_back(Simple<LookAtComponent>("Look At", "Behaviors", ICON_FA_EYE, LookAtComponent{.target = {0.0f, 0.0f, 0.0f}}));

			// 3D physics entries: Physics3D-gated and exclusive with the 2D set.
			const std::vector<std::string> kConflicts2D{"Rigid Body 2D", "Collider 2D", "Joint 2D"};
			const auto physics3D = [&kConflicts2D](ComponentCatalogEntry entry)
			{
				entry.requiredFeatures = SceneFeatureFlags::Physics3D;
				entry.conflictsWith = kConflicts2D;
				return entry;
			};
			c.push_back(physics3D(Simple<RigidBodyComponent>("Rigid Body", "Physics", ICON_FA_WEIGHT_HANGING, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic})));
			const auto colliderEntry = [](std::string name, PhysicsShapeType shape)
			{
				return ComponentCatalogEntry{std::move(name),
				        "Physics",
				        ICON_FA_DRAW_POLYGON,
				        [](const World& w, Entity e) { return w.Has<ColliderComponent>(e); },
				        [shape](World& w, Entity e, ServiceContainer&) { AddCollider(w, e, shape); },
				        [](World& w, Entity e) { w.Remove<ColliderComponent>(e); }};
			};
			c.push_back(physics3D(colliderEntry("Box Collider", PhysicsShapeType::Box)));
			c.push_back(physics3D(colliderEntry("Sphere Collider", PhysicsShapeType::Sphere)));
			c.push_back(physics3D(colliderEntry("Capsule Collider", PhysicsShapeType::Capsule)));
			c.push_back(physics3D(colliderEntry("Cylinder Collider", PhysicsShapeType::Cylinder)));
			c.push_back(physics3D(ComponentCatalogEntry{"Trigger Volume",
			        "Physics",
			        ICON_FA_WEIGHT_HANGING,
			        [](const World& w, Entity e) { return w.Has<ColliderComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&)
			        {
				        ColliderComponent col{};
				        col.shape = PhysicsShapeType::Box;
				        col.halfExtents = EntityScale(w, e) * 0.5f;
				        col.isSensor = true;
				        col.layer = PhysicsLayer::Sensor;
				        w.Emplace<ColliderComponent>(e, col);
			        },
			        [](World& w, Entity e) { w.Remove<ColliderComponent>(e); }}));
			c.push_back(physics3D(ComponentCatalogEntry{"Joint",
			        "Physics",
			        ICON_FA_LINK,
			        [](const World& w, Entity e) { return w.Has<JointComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { w.EmplaceOrReplace<JointComponent>(e, JointComponent{.anchor = EntityPosition(w, e)}); },
			        [](World& w, Entity e) { w.Remove<JointComponent>(e); }}));
			c.push_back(Simple<CollisionEventsComponent>("Collision Events", "Physics", ICON_FA_BOLT));

			c.push_back(Simple<SceneTransientComponent>("Scene Transient", "Editor", ICON_FA_GHOST));

			// registry is a program-lifetime static.
			for (const reflect::ComponentType& rt: reflect::ComponentTypes())
			{
				if (!rt.addable)
				{
					continue;
				}
				// A component with a bespoke hand-authored entry above (flagged at its
				// declaration, not matched by DisplayName) is skipped so it is not listed
				// twice - a rename can no longer silently duplicate the palette entry.
				if (rt.hasHandAuthoredCatalogEntry)
				{
					continue;
				}
				ComponentCatalogEntry entry{
				        rt.name, rt.category, rt.icon, [&rt](const World& w, Entity e) { return rt.has(w, e); },
				        [&rt](World& w, Entity e, ServiceContainer&)
				        {
					        rt.emplaceDefault(w, e);
					        if (rt.postSet) // compose companions / rebuild backing objects on a bare add, as set/apply do
					        {
						        rt.postSet(w, e);
					        }
				        },
				        [&rt](World& w, Entity e) { rt.remove(w, e); }};
				entry.requiredFeatures = rt.requiredFeatures;
				entry.conflictsWith = rt.conflictsWith;
				c.push_back(std::move(entry));
			}

			return c;
		}
	} // namespace

	const std::vector<ComponentCatalogEntry>& ComponentCatalog()
	{
		static const std::vector<ComponentCatalogEntry> catalog = Build();
		return catalog;
	}

	std::string ComponentAddBlockReason(const World& world, Entity entity, const ComponentCatalogEntry& entry)
	{
		// Unity-style: every component is available in every scene. The one
		// hard rule is per-entity - an entity never simulates in two physics
		// domains at once (conflictsWith).
		for (const std::string& conflictName: entry.conflictsWith)
		{
			const ComponentCatalogEntry* conflict = FindComponent(conflictName);
			if (conflict != nullptr && conflict->has && conflict->has(world, entity))
			{
				return "conflicts with the entity's " + conflictName + " component";
			}
		}
		return {};
	}

	bool ComponentVisibleInMenu(const World& world, Entity entity, const ComponentCatalogEntry& entry)
	{
		if (!entry.addable)
		{
			return false;
		}
		for (const std::string& conflictName: entry.conflictsWith)
		{
			const ComponentCatalogEntry* conflict = FindComponent(conflictName);
			if (conflict != nullptr && conflict->has && conflict->has(world, entity))
			{
				return false;
			}
		}
		return true;
	}

	void EnableComponentFeatures(World& world, const ComponentCatalogEntry& entry)
	{
		if (entry.requiredFeatures != SceneFeatureFlags::None && !HasAllSceneFeatures(world.GetSceneFeatures(), entry.requiredFeatures))
		{
			world.SetSceneFeatures(world.GetSceneFeatures() | entry.requiredFeatures);
		}
	}

	const ComponentCatalogEntry* FindComponent(std::string_view name)
	{
		for (const ComponentCatalogEntry& e: ComponentCatalog())
		{
			if (e.name == name)
			{
				return &e;
			}
		}
		return nullptr;
	}
} // namespace aether::editor
