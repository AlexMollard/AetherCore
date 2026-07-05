#include "scripting/DasModuleBase.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <array>

#include <glm/gtc/matrix_transform.hpp>

#include "daScript/daScript.h"

#include <glm/gtc/quaternion.hpp>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/TagSlots.hpp"
#include "assets/AssetManager.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialAuthoring.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/SceneSerializer.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/DasHelpers.hpp"
#include "utils/Logger.hpp"

// -- Binding functions ---------------------------------------------------------

namespace
{
	using namespace aether::app::scripting;

	// -- Entity lifecycle ------------------------------------------------------

	// entity_create(world) -> uint
	uint32_t das_entity_create(aether::World* w)
	{
		aether::Entity e = w->Create();
		w->Emplace<aether::NameComponent>(e, aether::NameComponent{.name = "Entity"});
		ActiveContext().sceneEntities.push_back(e);
		return e.id;
	}

	// entity_destroy(world, entity_id)
	void das_entity_destroy(aether::World* w, uint32_t id)
	{
		w->Destroy(aether::Entity{id});
	}

	// entity_valid(entity_id) -> bool
	bool das_entity_valid(uint32_t id)
	{
		return id != 0;
	}

	// set_name(world, entity_id, name)
	void das_set_name(aether::World* w, uint32_t id, const char* name)
	{
		w->EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = das_to_std_string(name)});
	}

	// -- Data-driven behaviors (BehaviorSystem advances these while Playing) ----

	// add_bob(world, entity_id, amplitude, frequency, phase)
	void das_add_bob(aether::World* w, uint32_t id, float amplitude, float frequency, float phase)
	{
		w->EmplaceOrReplace<aether::BobComponent>(aether::Entity{id}, aether::BobComponent{.amplitude = amplitude, .frequency = frequency, .phase = phase});
	}

	// add_spin(world, entity_id, euler_deg_per_sec)
	void das_add_spin(aether::World* w, uint32_t id, das::float3 eulerDegPerSec)
	{
		w->EmplaceOrReplace<aether::SpinComponent>(aether::Entity{id}, aether::SpinComponent{.eulerDegPerSec = to_glm(eulerDegPerSec)});
	}

	// add_orbit(world, entity_id, center, radius, speed_deg, start_angle_deg, yaw_offset_deg, height)
	void das_add_orbit(aether::World* w, uint32_t id, das::float3 center, float radius, float speedDeg, float startAngleDeg, float yawOffsetDeg, float height)
	{
		w->EmplaceOrReplace<aether::OrbitComponent>(aether::Entity{id}, aether::OrbitComponent{.center = to_glm(center), .radius = radius, .angularSpeedDeg = speedDeg, .angleDeg = startAngleDeg, .yawOffsetDeg = yawOffsetDeg, .height = height});
	}

	// add_material_pulse(world, entity_id, emissive_a, emissive_b, frequency)
	void das_add_material_pulse(aether::World* w, uint32_t id, das::float3 emissiveA, das::float3 emissiveB, float frequency)
	{
		w->EmplaceOrReplace<aether::MaterialPulseComponent>(aether::Entity{id}, aether::MaterialPulseComponent{.emissiveA = to_glm(emissiveA), .emissiveB = to_glm(emissiveB), .frequency = frequency});
	}

	// mark_transient(world, entity_id) - exclude the entity (and its subtree)
	// from scene capture; script-owned runtime actors use this.
	void das_mark_transient(aether::World* w, uint32_t id)
	{
		// Empty tag component: entt's empty-type storage returns void from
		// emplace, so the World wrapper (which returns T&) cannot be used.
		auto& reg = w->GetRegistry();
		const auto e = aether::World::ToEntt(aether::Entity{id});
		if (!reg.all_of<aether::SceneTransientComponent>(e))
		{
			reg.emplace<aether::SceneTransientComponent>(e);
		}
	}

	// scene_file_exists(name) -> bool - lets on_attach skip its legacy content
	// builders once the startup scene file has been generated.
	bool das_scene_file_exists(const char* name)
	{
		return aether::app::scene::ReadSceneFile(name ? name : "").has_value();
	}

	// get_name(world, entity_id) -> string  (empty string when unnamed)
	char* das_get_name(aether::World* w, uint32_t id, das::Context* ctx)
	{
		const auto nc = w->TryGet<aether::NameComponent>(aether::Entity{id});
		return nc ? das_string(ctx, nc->name) : nullptr;
	}

	// -- TransformComponent field access ---------------------------------------
	// Individual float3 accessors avoid needing das::cast<> for large structs.

	// get_position(world, entity_id) -> float3
	das::float3 das_get_position(aether::World* w, uint32_t id)
	{
		const auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return {};
		}
		return to_das(glm::vec3(tc->localToWorld[3]));
	}

	// Script movement carries the entity's body - the same true-teleport
	// semantics as the editor's ApplyWorldTransform (Jolt SetPosition/Rotation
	// + prev==curr interp state). Without this, a body added to a
	// script-driven entity (a capsule on the player) simulates away from
	// where the script keeps putting the visuals and the two owners yank the
	// transform back and forth every frame.
	void TeleportBodyToTransform(aether::World& w, aether::Entity e)
	{
		auto* ps = w.TryGet<aether::PhysicsStateComponent>(e);
		const auto* tc = w.TryGet<aether::TransformComponent>(e);
		if (ps == nullptr || tc == nullptr)
		{
			return;
		}
		glm::vec3 pos{}, euler{}, scale{};
		aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
		const glm::quat q = glm::angleAxis(glm::radians(euler.y), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(euler.x), glm::vec3(1, 0, 0)) * glm::angleAxis(glm::radians(euler.z), glm::vec3(0, 0, 1));
		ps->prevPosition = pos;
		ps->currPosition = pos;
		ps->prevRotation = q;
		ps->currRotation = q;
		ps->scale = glm::max(scale, glm::vec3(0.001f));

		const auto* rb = w.TryGet<aether::RigidBodyComponent>(e);
		auto* physics = ActiveContext().physics;
		if (rb != nullptr && physics != nullptr)
		{
			physics->SetPosition(rb->body, pos);
			physics->SetRotation(rb->body, q);
		}
	}

	// set_position(world, entity_id, pos)
	// Only updates the translation column - leaves rotation/scale intact.
	// Children ride along with the delta, keeping their relative offsets.
	void das_set_position(aether::World* w, uint32_t id, das::float3 pos)
	{
		auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return;
		}
		glm::mat4 m = tc->localToWorld;
		m[3] = glm::vec4(to_glm(pos), 1.0f);
		aether::ecs::SetWorldTransform(*w, aether::Entity{id}, m);
		TeleportBodyToTransform(*w, aether::Entity{id});
	}

	// get_scale(world, entity_id) -> float3
	das::float3 das_get_scale(aether::World* w, uint32_t id)
	{
		const auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return {1.0f, 1.0f, 1.0f};
		}
		const auto& m = tc->localToWorld;
		return to_das({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
	}

	// get_euler(world, entity_id) -> float3  (degrees, YXZ order)
	das::float3 das_get_euler(aether::World* w, uint32_t id)
	{
		const auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return {};
		}
		glm::vec3 pos{}, euler{}, scale{};
		aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
		return to_das(euler);
	}

	// set_euler(world, entity_id, euler_deg) - updates only rotation, preserves translation and scale.
	// The subtree follows with the same world-space delta (relative offsets kept).
	void das_set_euler(aether::World* w, uint32_t id, das::float3 euler)
	{
		auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return;
		}

		glm::vec3 pos{}, curEuler{}, scale{};
		aether::DecomposeTRS(tc->localToWorld, pos, curEuler, scale);
		aether::ecs::SetWorldTransform(*w, aether::Entity{id}, aether::ComposeTransform(pos, to_glm(euler), scale));
		TeleportBodyToTransform(*w, aether::Entity{id});
	}

	// set_transform(world, entity_id, pos, euler_deg, scale)
	// Recomposes the full TRS matrix from the three float3 arguments. Spawned
	// mesh children (and anything else parented under the entity) follow with
	// the same world-space delta, so per-primitive local offsets survive.
	void das_set_transform(aether::World* w, uint32_t id, das::float3 pos, das::float3 euler, das::float3 scale)
	{
		const auto xform = aether::ComposeTransform(to_glm(pos), to_glm(euler), to_glm(scale));
		aether::ecs::SetWorldTransform(*w, aether::Entity{id}, xform);
		TeleportBodyToTransform(*w, aether::Entity{id});
	}

	// -- Model loading ---------------------------------------------------------

	// load_model(world, entity_id, path)
	void das_load_model(aether::World* w, uint32_t id, const char* path)
	{
		auto& ctx = ActiveContext();

		if (!ctx.assets)
		{
			AE_WARN(aether::LogCategory::App, "load_model: no asset manager");
			return;
		}

		glm::mat4 xform{1.0f};

		if (const auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id}))
		{
			xform = tc->localToWorld;
		}

		// -------------------------------------------------------------------------
		// Try reuse existing model
		// -------------------------------------------------------------------------

		aether::LoadedModel* modelPtr = nullptr;

		auto it = ctx.loadedModelMap.find(path);

		if (it != ctx.loadedModelMap.end())
		{
			modelPtr = &ctx.loadedModels[it->second];
		}
		else
		{
			auto result = ctx.assets->LoadModel(path);

			if (!result)
			{
				AE_WARN(aether::LogCategory::App, "load_model: failed to load '{}' because of {}", path, result.error());
				return;
			}

			ctx.loadedModels.push_back(std::move(result.value()));

			const size_t index = ctx.loadedModels.size() - 1;

			ctx.loadedModelMap[path] = index;

			modelPtr = &ctx.loadedModels[index];
		}

		// -------------------------------------------------------------------------
		// Spawn instance with parent link
		// -------------------------------------------------------------------------

		// Name the logical entity after the model file stem (e.g. "fox").
		std::string stem = path ? path : "";
		if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos)
		{
			stem = stem.substr(slash + 1);
		}
		if (const auto dot = stem.find_last_of('.'); dot != std::string::npos)
		{
			stem = stem.substr(0, dot);
		}
		if (stem.empty())
		{
			stem = "Model";
		}
		w->EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = stem});

		// Mirror the legacy entityIds.clear() below: reloading a model onto the
		// same logical entity re-links its children instead of accumulating.
		if (const auto* h = w->TryGet<aether::HierarchyComponent>(aether::Entity{id}))
		{
			const std::vector<aether::Entity> stale = h->children;
			for (const aether::Entity s: stale)
			{
				aether::ecs::DetachFromParent(*w, s);
			}
		}

		std::vector<aether::Entity> meshEntities = ctx.assets->SpawnModel(*modelPtr, id);

		for (std::size_t i = 0; i < meshEntities.size(); ++i)
		{
			const aether::Entity meshEntity = meshEntities[i];
			if (auto tc = w->TryGet<aether::TransformComponent>(meshEntity))
			{
				tc->localToWorld = xform * tc->localToWorld;
			}
			// SpawnModel already linked meshEntity under id via ecs::SetParent.
			w->EmplaceOrReplace<aether::NameComponent>(meshEntity, aether::NameComponent{.name = stem + " mesh"});
			// Stable identity for serialization: SpawnModel emits one entity per
			// primitive in order, so i is the LoadedModel primitive index.
			w->EmplaceOrReplace<aether::MeshSourceComponent>(meshEntity, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Model, .path = path ? path : "", .primitiveIndex = static_cast<std::uint32_t>(i)});
			ctx.sceneEntities.push_back(meshEntity);
		}
	}

	// -- Entity iteration ------------------------------------------------------

	// for_each_with_transform(world) <| $(e : uint) { ... }
	void das_for_each_with_transform(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		for (auto enttE: w->View<aether::TransformComponent>())
		{
			const auto id = static_cast<uint32_t>(entt::to_integral(enttE));
			vec4f args[1];
			args[0] = das::cast<uint32_t>::from(id);
			ctx->invoke(block, args, nullptr, at);
		}
	}

	// -- Dynamic Tags ---------------------------------------------------------

	// DAS binding functions - delegate to TagSlots.cpp implementation

	// create_tag(name: string) -> uint
	uint32_t das_create_tag(const char* name)
	{
		std::string tagName(name ? name : "");
		return aether::TagCreate(tagName);
	}

	// get_tag_id(name: string) -> uint
	uint32_t das_get_tag_id(const char* name)
	{
		std::string tagName(name ? name : "");
		return aether::TagGetId(tagName);
	}

	// add_tag(world, entity_id, tag_id)
	void das_add_tag(aether::World* w, uint32_t entityId, uint32_t tagId)
	{
		aether::TagAdd(w, entityId, tagId);
	}

	// has_tag(world, entity_id, tag_id) -> bool
	bool das_has_tag(aether::World* w, uint32_t entityId, uint32_t tagId)
	{
		return aether::TagHas(w, entityId, tagId);
	}

	// remove_tag(world, entity_id, tag_id)
	void das_remove_tag(aether::World* w, uint32_t entityId, uint32_t tagId)
	{
		aether::TagRemove(w, entityId, tagId);
	}

	// for_each_with_tag(world, tag_id) <| $(e : uint) { ... }
	void das_for_each_with_tag(aether::World* w, uint32_t tagId, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		aether::ForEachWithTag(w,
		        tagId,
		        [&](uint32_t id)
		        {
			        vec4f args[1];
			        args[0] = das::cast<uint32_t>::from(id);
			        ctx->invoke(block, args, nullptr, at);
		        });
	}

	// for_each_with_tag_transform(world, tag_id) <| $(e : uint, pos : float3, euler : float3, scale : float3) { ... }
	// Convenience combo: iterate tagged entities that also have a TransformComponent and
	// hand the block decomposed pos/euler(degrees, YXZ)/scale - no per-entity get calls needed.
	void das_for_each_with_tag_transform(aether::World* w, uint32_t tagId, const das::TBlock<void, uint32_t, das::float3, das::float3, das::float3>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		aether::ForEachWithTag(w,
		        tagId,
		        [&](uint32_t id)
		        {
			        const auto tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
			        if (!tc)
			        {
				        return;
			        }
			        glm::vec3 pos{}, euler{}, scale{};
			        aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
			        vec4f args[4];
			        args[0] = das::cast<uint32_t>::from(id);
			        args[1] = das::cast<das::float3>::from(to_das(pos));
			        args[2] = das::cast<das::float3>::from(to_das(euler));
			        args[3] = das::cast<das::float3>::from(to_das(scale));
			        ctx->invoke(block, args, nullptr, at);
		        });
	}

	// -- Primitive mesh caching -----------------------------------------------
	// create_mesh(world, type_string) -> uint
	// Caches a primitive mesh by type ("cube", "sphere", "plane", "quad", "triangle")
	// and returns a handle that can be passed to add_mesh.
	uint32_t das_create_mesh(aether::World* w, const char* type)
	{
		(void) w;
		auto& ctx = ActiveContext();
		if (!ctx.primitives)
		{
			return 0u;
		}

		aether::PrimitiveMesh primType{};
		const char* displayName = "Mesh";
		const std::string_view sv(type ? type : "");
		if (sv == "cube")
		{
			primType = aether::PrimitiveMesh::Cube;
			displayName = "Cube";
		}
		else if (sv == "sphere")
		{
			primType = aether::PrimitiveMesh::Sphere;
			displayName = "Sphere";
		}
		else if (sv == "plane")
		{
			primType = aether::PrimitiveMesh::Plane;
			displayName = "Plane";
		}
		else if (sv == "quad")
		{
			primType = aether::PrimitiveMesh::Quad;
			displayName = "Quad";
		}
		else if (sv == "triangle")
		{
			primType = aether::PrimitiveMesh::Triangle;
			displayName = "Triangle";
		}
		else
		{
			return 0u;
		}

		// Build the default primitive material on first use. Pure authoring data;
		// the registry acquires (and dedups) a GPU slot at add_mesh time.
		if (!ctx.defaultMaterialInitialized)
		{
			ctx.defaultMaterial = {};
			ctx.defaultMaterial.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.f);
			ctx.defaultMaterial.roughnessFactor = 0.6f;
			ctx.defaultMaterial.metallicFactor = 0.0f;
			// Primitive meshes carry meaningful vertex colours (rainbow cubes).
			ctx.defaultMaterial.modulateVertexColor = true;
			// Two-sided by default: primitives include the single-sided plane
			// (used for walls), which must stay visible from both faces - this
			// matches the pre-phase-3 default pipeline (CullMode::None). Authored
			// materials via set_material still default single-sided (back-culled).
			ctx.defaultMaterial.doubleSided = true;
			ctx.defaultMaterialInitialized = true;
		}

		SceneContext::CachedMesh entry;
		entry.mesh = &ctx.primitives->Get(primType);
		entry.materialAsset = ctx.defaultMaterial;
		entry.displayName = displayName;
		entry.kindName = std::string(sv);
		ctx.meshCache.push_back(std::move(entry));
		return static_cast<uint32_t>(ctx.meshCache.size() - 1u);
	}

	// add_mesh(world, entity_id, mesh_handle)
	// Attaches PipelineComponent + MeshComponent + MaterialComponent to an existing
	// entity using a cached mesh handle returned by create_mesh.
	void das_add_mesh(aether::World* w, uint32_t entityId, uint32_t meshHandle)
	{
		auto& ctx = ActiveContext();
		if (meshHandle >= ctx.meshCache.size())
		{
			return;
		}

		const auto& entry = ctx.meshCache[meshHandle];
		if (!entry.mesh || !ctx.assets)
		{
			return;
		}

		const aether::Entity e{entityId};
		w->EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = entry.mesh});
		w->EmplaceOrReplace<aether::MeshSourceComponent>(e, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Primitive, .path = entry.kindName, .primitiveIndex = 0});
		// AssignMaterial resolves the pipeline through PipelineCache and emplaces
		// PipelineComponent, so every add_mesh entity gets a pipeline.
		aether::MaterialSystem::AssignMaterial(*w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), entry.materialAsset);

		// Keep an explicit user rename ("Entity" is the entity_create default).
		const auto* nc = w->TryGet<aether::NameComponent>(e);
		if (!nc || nc->name.empty() || nc->name == "Entity")
		{
			w->EmplaceOrReplace<aether::NameComponent>(e, aether::NameComponent{.name = entry.displayName});
		}
	}

	// -- Material painting: one-shot solid paint --------------------------------
	// Content-addressed and immutable: entities painted identical values share a
	// single GPU material slot. Painted materials are solid (vertex-colour
	// modulation flag stays off).

	// set_material(world, entity_id, color, metallic, roughness)
	void das_set_material(aether::World* w, uint32_t id, das::float3 color, float metallic, float roughness)
	{
		auto& ctx = ActiveContext();
		if (!ctx.assets)
		{
			return;
		}
		aether::MaterialAsset asset;
		asset.baseColorFactor = glm::vec4(to_glm(color), 1.0f);
		asset.metallicFactor = metallic;
		asset.roughnessFactor = roughness;
		// Seed the editable instance so later entity_material_set_* edits compose
		// on this paint instead of restarting from a blank default.
		const aether::Entity e{id};
		w->EmplaceOrReplace<aether::MaterialInstanceComponent>(e, aether::MaterialInstanceComponent{asset});
		aether::MaterialSystem::AssignMaterial(*w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), asset);
	}

	// set_material_color(world, entity_id, color) - paint with the default
	// surface response (dielectric, roughness matching the primitive default).
	void das_set_material_color(aether::World* w, uint32_t id, das::float3 color)
	{
		das_set_material(w, id, color, 0.0f, 0.6f);
	}

	// -- Named / shared authoring materials -------------------------------------
	// Build once, edit fields, bind to many entities. Editing re-binds every
	// bound entity. Sharing is content-honest (identical content dedups to one
	// GPU slot) but each id stays an independently editable authored material.

	// make_material(color, metallic, roughness) -> material_id
	uint32_t das_make_material(das::float3 color, float metallic, float roughness)
	{
		auto& ctx = ActiveContext();
		if (!ctx.assets)
		{
			return aether::MaterialAuthoring::kInvalidId;
		}
		aether::MaterialAsset asset;
		asset.baseColorFactor = glm::vec4(to_glm(color), 1.0f);
		asset.metallicFactor = metallic;
		asset.roughnessFactor = roughness;
		return ctx.assets->GetMaterialAuthoring().Create(asset);
	}

	// bind_material(world, entity_id, material_id)
	void das_bind_material(aether::World* w, uint32_t entityId, uint32_t materialId)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			ctx.assets->GetMaterialAuthoring().Bind(*w, aether::Entity{entityId}, materialId);
		}
	}

	// material_set_color/metallic/roughness/emissive(world, material_id, ...)
	void das_material_set_color(aether::World* w, uint32_t materialId, das::float3 color)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			ctx.assets->GetMaterialAuthoring().SetBaseColor(*w, materialId, to_glm(color));
		}
	}

	void das_material_set_metallic(aether::World* w, uint32_t materialId, float value)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			ctx.assets->GetMaterialAuthoring().SetMetallic(*w, materialId, value);
		}
	}

	void das_material_set_roughness(aether::World* w, uint32_t materialId, float value)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			ctx.assets->GetMaterialAuthoring().SetRoughness(*w, materialId, value);
		}
	}

	void das_material_set_emissive(aether::World* w, uint32_t materialId, das::float3 color)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			ctx.assets->GetMaterialAuthoring().SetEmissive(*w, materialId, to_glm(color));
		}
	}

	// -- Per-entity material instance edits -------------------------------------
	// Mutate this one entity's material a field at a time (copy-on-write). Seeds
	// a default material if the entity has none. A no-op value change is free.

	void das_entity_material_set_color(aether::World* w, uint32_t entityId, das::float3 color)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			aether::MaterialSystem::SetBaseColor(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), to_glm(color));
		}
	}

	void das_entity_material_set_metallic(aether::World* w, uint32_t entityId, float value)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			aether::MaterialSystem::SetMetallic(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
		}
	}

	void das_entity_material_set_roughness(aether::World* w, uint32_t entityId, float value)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			aether::MaterialSystem::SetRoughness(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
		}
	}

	void das_entity_material_set_emissive(aether::World* w, uint32_t entityId, das::float3 color)
	{
		auto& ctx = ActiveContext();
		if (ctx.assets)
		{
			aether::MaterialSystem::SetEmissive(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), to_glm(color));
		}
	}

	// set_material_texture(world, entity_id, path) - set the entity's albedo map
	// from a VFS texture path. A MISSING/failed path resolves to the magenta
	// fallback (the visible "missing texture" marker), so this is the script-side
	// hook for exercising the texture-fallback path.
	void das_set_material_texture(aether::World* w, uint32_t entityId, const char* path)
	{
		auto& ctx = ActiveContext();
		if (!ctx.assets)
		{
			return;
		}
		auto& texReg = ctx.assets->GetTextureRegistry();
		// Acquire a loader ref (missing path -> broken handle -> magenta), hand the
		// handle to the material (whose slot cascade takes its OWN ref), then drop
		// the loader ref. Balanced + dedup-safe for real textures; a pure no-op on
		// refcounts for the broken handle.
		const aether::TextureHandle tex = texReg.Acquire(path);
		aether::MaterialSystem::SetAlbedoTexture(*w, aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), tex);
		texReg.Release(tex);
	}

} // namespace

// -- Module --------------------------------------------------------------------

namespace aether::app::scripting
{
	struct WorldModule : DasModuleBase
	{
		WorldModule()
		      : DasModuleBase("world")
		{
			das::ModuleLibrary lib(this);

			// Register World as an opaque reference type so scripts can declare
			// def on_attach(world : World) and pass it to ECS functions.
			addAnnotation(das::make_smart<das::DummyTypeAnnotation>("World", "::aether::World", sizeof(void*), sizeof(void*)));

			// Entity lifecycle
			Bind<das_entity_create>(lib, "entity_create", SE::modifyExternal);
			Bind<das_entity_destroy>(lib, "entity_destroy", SE::modifyExternal);
			Bind<das_entity_valid>(lib, "entity_valid", SE::none);
			Bind<das_set_name>(lib, "set_name", SE::modifyExternal);
			Bind<das_get_name>(lib, "get_name", SE::accessExternal);
			Bind<das_add_bob>(lib, "add_bob", SE::modifyExternal);
			Bind<das_add_spin>(lib, "add_spin", SE::modifyExternal);
			Bind<das_add_orbit>(lib, "add_orbit", SE::modifyExternal);
			Bind<das_add_material_pulse>(lib, "add_material_pulse", SE::modifyExternal);
			Bind<das_mark_transient>(lib, "mark_transient", SE::modifyExternal);
			Bind<das_scene_file_exists>(lib, "scene_file_exists", SE::accessExternal);

			// TransformComponent - structural ops (add/has/remove)
			BIND_COMPONENT("transform", aether::TransformComponent)

			// TransformComponent - field access
			Bind<das_get_position>(lib, "get_position", SE::accessExternal);
			Bind<das_set_position>(lib, "set_position", SE::modifyExternal);
			Bind<das_get_scale>(lib, "get_scale", SE::accessExternal);
			Bind<das_get_euler>(lib, "get_euler", SE::accessExternal);
			Bind<das_set_euler>(lib, "set_euler", SE::modifyExternal);
			Bind<das_set_transform>(lib, "set_transform", SE::modifyExternal);

			// Rendering components - structural ops (spawned internally by load_model)
			BIND_COMPONENT("mesh", aether::MeshComponent)
			BIND_COMPONENT("material", aether::MaterialComponent)
			BIND_COMPONENT("pipeline", aether::PipelineComponent)
			BIND_COMPONENT("skinned_mesh", aether::SkinnedMeshComponent)

			// Model loading convenience
			Bind<das_load_model>(lib, "load_model", SE::modifyExternal);

			// Primitive mesh cache
			Bind<das_create_mesh>(lib, "create_mesh", SE::modifyExternal);
			Bind<das_add_mesh>(lib, "add_mesh", SE::modifyExternal);

			// Material painting (one-shot solid paint)
			Bind<das_set_material>(lib, "set_material", SE::modifyExternal);
			Bind<das_set_material_color>(lib, "set_material_color", SE::modifyExternal);

			// Named / shared authoring materials
			Bind<das_make_material>(lib, "make_material", SE::modifyExternal);
			Bind<das_bind_material>(lib, "bind_material", SE::modifyExternal);
			Bind<das_material_set_color>(lib, "material_set_color", SE::modifyExternal);
			Bind<das_material_set_metallic>(lib, "material_set_metallic", SE::modifyExternal);
			Bind<das_material_set_roughness>(lib, "material_set_roughness", SE::modifyExternal);
			Bind<das_material_set_emissive>(lib, "material_set_emissive", SE::modifyExternal);

			// Per-entity material instance edits
			Bind<das_entity_material_set_color>(lib, "entity_material_set_color", SE::modifyExternal);
			Bind<das_entity_material_set_metallic>(lib, "entity_material_set_metallic", SE::modifyExternal);
			Bind<das_entity_material_set_roughness>(lib, "entity_material_set_roughness", SE::modifyExternal);
			Bind<das_entity_material_set_emissive>(lib, "entity_material_set_emissive", SE::modifyExternal);
			Bind<das_set_material_texture>(lib, "set_material_texture", SE::modifyExternal);

			// Entity iteration
			Bind<das_for_each_with_transform>(lib, "for_each_with_transform", SE::modifyExternal);

			// Dynamic Tags
			Bind<das_create_tag>(lib, "create_tag", SE::modifyExternal);
			Bind<das_get_tag_id>(lib, "get_tag_id", SE::accessExternal);
			Bind<das_add_tag>(lib, "add_tag", SE::modifyExternal);
			Bind<das_has_tag>(lib, "has_tag", SE::accessExternal);
			Bind<das_remove_tag>(lib, "remove_tag", SE::modifyExternal);
			Bind<das_for_each_with_tag>(lib, "for_each_with_tag", SE::accessExternal);
			Bind<das_for_each_with_tag_transform>(lib, "for_each_with_tag_transform", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

// WorldModule does not use AETHER_DAS_MODULE - ScriptingSubsystem registers it
// explicitly as the first module so all other modules can resolve World* types.
REGISTER_MODULE_IN_NAMESPACE(WorldModule, aether::app::scripting)
