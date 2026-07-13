#include "editor/ComponentCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <glm/glm.hpp>

#include "assets/AssetManager.hpp"
#include "debug/Icons.hpp"
#include "material/MaterialSystem.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "ui/UiComponents.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	namespace
	{
		// Transform-derived seeds (mirrors the Inspector palette's smarter defaults).
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
				return glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
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
			if (!w.Has<MaterialComponent>(e))
			{
				MaterialAsset asset{};
				asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				asset.roughnessFactor = 0.6f;
				asset.doubleSided = true;
				MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
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

		// Simple entry for a plain, default-constructed (or fixed-value) component.
		template<typename T>
		ComponentCatalogEntry Simple(std::string name, std::string category, std::string icon, T value = T{})
		{
			return ComponentCatalogEntry{
			        std::move(name), std::move(category), std::move(icon),
			        [](const World& w, Entity e) { return w.Has<T>(e); },
			        [value](World& w, Entity e, ServiceContainer&)
			        {
				        if constexpr (std::is_empty_v<T>)
				        {
					        (void) value; // tag component: emplace takes no value
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

			// ── Core ────────────────────────────────────────────────────────────
			// Transform is auto-included from the reflection registry below (its
			// default is just identity, so no hand-written entry is needed).
			c.push_back(Simple<NameComponent>("Name", "Core", ICON_FA_PEN, NameComponent{.name = "Entity"}));
			c.push_back(Simple<HierarchyComponent>("Hierarchy", "Core", ICON_FA_SITEMAP));

			// ── Rendering ───────────────────────────────────────────────────────
			const auto meshEntry = [](std::string name, std::string icon, PrimitiveMesh kind, const char* path)
			{
				return ComponentCatalogEntry{std::move(name), "Rendering", std::move(icon),
				        [](const World& w, Entity e) { return w.Has<MeshComponent>(e); },
				        [kind, path](World& w, Entity e, ServiceContainer& s) { AddMeshPrimitive(w, e, s, kind, path); },
				        [](World& w, Entity e) { RemoveMeshBundle(w, e); }};
			};
			c.push_back(meshEntry("Cube", ICON_FA_CUBE, PrimitiveMesh::Cube, "cube"));
			c.push_back(meshEntry("Sphere", ICON_FA_CIRCLE, PrimitiveMesh::Sphere, "sphere"));
			c.push_back(meshEntry("Plane", ICON_FA_IMAGE, PrimitiveMesh::Plane, "plane"));
			c.push_back(meshEntry("Quad", ICON_FA_IMAGE, PrimitiveMesh::Quad, "quad"));
			c.push_back(meshEntry("Triangle", ICON_FA_PLAY, PrimitiveMesh::Triangle, "triangle"));

			c.push_back(ComponentCatalogEntry{"Sprite", "Rendering", ICON_FA_IMAGE,
			        [](const World& w, Entity e) { return w.Has<SpriteRendererComponent>(e); },
			        [](World& w, Entity e, ServiceContainer& s)
			        {
				        auto* prims = s.TryGet<PrimitiveMeshes>();
				        auto* assets = s.TryGet<AssetManager>();
				        if (prims == nullptr || assets == nullptr) { return; }
				        EnsureTransform(w, e);
				        w.EmplaceOrReplace<MeshComponent>(e, MeshComponent{.mesh = &prims->Get(PrimitiveMesh::Quad)});
				        w.EmplaceOrReplace<MeshSourceComponent>(e, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = "quad", .primitiveIndex = 0});
				        MaterialAsset asset{};
				        asset.baseColorFactor = glm::vec4(1.0f);
				        asset.roughnessFactor = 1.0f;
				        asset.metallicFactor = 0.0f;
				        asset.doubleSided = true;
				        asset.alphaBlend = true;
				        MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
				        w.EmplaceOrReplace<SpriteRendererComponent>(e);
			        },
			        [](World& w, Entity e) { RemoveMeshBundle(w, e); }});

			c.push_back(ComponentCatalogEntry{"Material", "Rendering", ICON_FA_PALETTE,
			        [](const World& w, Entity e) { return w.Has<MaterialComponent>(e); },
			        [](World& w, Entity e, ServiceContainer& s)
			        {
				        auto* assets = s.TryGet<AssetManager>();
				        if (assets == nullptr) { return; }
				        MaterialAsset asset{};
				        asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				        asset.roughnessFactor = 0.6f;
				        MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			        },
			        [](World& w, Entity e) { w.Remove<MaterialComponent>(e); }});

			c.push_back(ComponentCatalogEntry{"Point Light", "Rendering", ICON_FA_LIGHTBULB,
			        [](const World& w, Entity e) { return w.Has<PointLightComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { EnsureTransform(w, e); w.EmplaceOrReplace<PointLightComponent>(e); },
			        [](World& w, Entity e) { w.Remove<PointLightComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Spot Light", "Rendering", ICON_FA_LIGHTBULB,
			        [](const World& w, Entity e) { return w.Has<SpotLightComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { EnsureTransform(w, e); w.EmplaceOrReplace<SpotLightComponent>(e); },
			        [](World& w, Entity e) { w.Remove<SpotLightComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Camera", "Rendering", ICON_FA_VIDEO,
			        [](const World& w, Entity e) { return w.Has<CameraComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { EnsureTransform(w, e); w.EmplaceOrReplace<CameraComponent>(e); },
			        [](World& w, Entity e) { w.Remove<CameraComponent>(e); }});
			// Reference-only (addable=false): UI text is authored as a UI ENTITY
			// (Create > UI > Text / ui::CreateTextEntity), never slapped onto an
			// arbitrary entity as a loose component. This entry exists purely so a
			// script's UiTextRef field can drop-validate against real UI text
			// entities through the `has` predicate - the single, Unity-style model.
			c.push_back(ComponentCatalogEntry{"UI Text", "Rendering", ICON_FA_PEN,
			        [](const World& w, Entity e) { return w.Has<ui::UIText>(e); },
			        nullptr,
			        nullptr,
			        /*addable=*/false});

			// ── Behaviors ───────────────────────────────────────────────────────
			c.push_back(Simple<BobComponent>("Bob", "Behaviors", ICON_FA_WAVE_SQUARE, BobComponent{.amplitude = 1.5f, .frequency = 0.8f}));
			c.push_back(Simple<SpinComponent>("Spin", "Behaviors", ICON_FA_ROTATE, SpinComponent{.eulerDegPerSec = {0.0f, 40.0f, 0.0f}}));
			c.push_back(ComponentCatalogEntry{"Orbit", "Behaviors", ICON_FA_CIRCLE_NOTCH,
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

			// ── Physics ─────────────────────────────────────────────────────────
			c.push_back(Simple<RigidBodyComponent>("Rigid Body", "Physics", ICON_FA_WEIGHT_HANGING, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic}));
			const auto colliderEntry = [](std::string name, PhysicsShapeType shape)
			{
				return ComponentCatalogEntry{std::move(name), "Physics", ICON_FA_WEIGHT_HANGING,
				        [](const World& w, Entity e) { return w.Has<ColliderComponent>(e); },
				        [shape](World& w, Entity e, ServiceContainer&) { AddCollider(w, e, shape); },
				        [](World& w, Entity e) { w.Remove<ColliderComponent>(e); }};
			};
			c.push_back(colliderEntry("Box Collider", PhysicsShapeType::Box));
			c.push_back(colliderEntry("Sphere Collider", PhysicsShapeType::Sphere));
			c.push_back(colliderEntry("Capsule Collider", PhysicsShapeType::Capsule));
			c.push_back(colliderEntry("Cylinder Collider", PhysicsShapeType::Cylinder));
			c.push_back(ComponentCatalogEntry{"Trigger Volume", "Physics", ICON_FA_WEIGHT_HANGING,
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
			        [](World& w, Entity e) { w.Remove<ColliderComponent>(e); }});
			c.push_back(ComponentCatalogEntry{"Joint", "Physics", ICON_FA_LINK,
			        [](const World& w, Entity e) { return w.Has<JointComponent>(e); },
			        [](World& w, Entity e, ServiceContainer&) { w.EmplaceOrReplace<JointComponent>(e, JointComponent{.anchor = EntityPosition(w, e)}); },
			        [](World& w, Entity e) { w.Remove<JointComponent>(e); }});
			c.push_back(Simple<CollisionEventsComponent>("Collision Events", "Physics", ICON_FA_BOLT));

			// ── Editor ──────────────────────────────────────────────────────────
			c.push_back(Simple<SceneTransientComponent>("Scene Transient", "Editor", ICON_FA_GHOST));

			// Auto-include any addable reflected component (scene/reflection/) not
			// hand-written above, so a new component needs only its AE_COMPONENT
			// declaration to appear in the palette + MCP - its has/add/remove come
			// from the registry. Hand-written entries win by name (they carry custom
			// add behaviour / seeds / bundles). The &rt captures are stable: the
			// registry is a program-lifetime static.
			for (const reflect::ComponentType& rt: reflect::ComponentTypes())
			{
				if (!rt.addable) { continue; }
				if (std::any_of(c.begin(), c.end(), [&](const ComponentCatalogEntry& e) { return e.name == rt.name; })) { continue; }
				c.push_back(ComponentCatalogEntry{rt.name, rt.category, rt.icon,
				        [&rt](const World& w, Entity e) { return rt.has(w, e); },
				        [&rt](World& w, Entity e, ServiceContainer&) { rt.emplaceDefault(w, e); },
				        [&rt](World& w, Entity e) { rt.remove(w, e); }});
			}

			return c;
		}
	} // namespace

	const std::vector<ComponentCatalogEntry>& ComponentCatalog()
	{
		static const std::vector<ComponentCatalogEntry> catalog = Build();
		return catalog;
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
