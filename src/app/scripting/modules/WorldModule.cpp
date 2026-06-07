#include "scripting/DasModuleBase.hpp"

#include <cmath>
#include <string>
#include <unordered_map>
#include <array>

#include <glm/gtc/matrix_transform.hpp>

#include "daScript/daScript.h"

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scene/TagSlots.hpp"
#include "assets/AssetManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace
{
	// Compose a TRS matrix from pos/euler(degrees)/scale - YXZ rotation order.
	glm::mat4 ComposeTransform(glm::vec3 pos, glm::vec3 rotEulerDeg, glm::vec3 scale)
	{
		glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
		glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.y), glm::vec3(0, 1, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.x), glm::vec3(1, 0, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.z), glm::vec3(0, 0, 1));
		glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
		return t * r * s;
	}
} // namespace

// ── Binding functions ─────────────────────────────────────────────────────────

namespace
{
	using namespace aether::app::scripting;

	// ── Entity lifecycle ──────────────────────────────────────────────────────

	// entity_create(world) -> uint
	uint32_t das_entity_create(aether::World* w)
	{
		aether::Entity e = w->Create();
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

	// ── TransformComponent field access ───────────────────────────────────────
	// Individual float3 accessors avoid needing das::cast<> for large structs.

	// get_position(world, entity_id) -> float3
	das::float3 das_get_position(aether::World* w, uint32_t id)
	{
		das::float3 r{};
		const auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return r;
		}
		r.x = tc->localToWorld[3][0];
		r.y = tc->localToWorld[3][1];
		r.z = tc->localToWorld[3][2];
		return r;
	}

	// set_position(world, entity_id, pos)
	// Only updates the translation column - leaves rotation/scale intact.
	void das_set_position(aether::World* w, uint32_t id, das::float3 pos)
	{
		auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return;
		}
		tc->localToWorld[3] = glm::vec4(pos.x, pos.y, pos.z, 1.0f);
	}

	// get_scale(world, entity_id) -> float3
	das::float3 das_get_scale(aether::World* w, uint32_t id)
	{
		das::float3 r{};
		r.x = r.y = r.z = 1.0f;
		const auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return r;
		}
		const auto& m = tc->localToWorld;
		r.x = glm::length(glm::vec3(m[0]));
		r.y = glm::length(glm::vec3(m[1]));
		r.z = glm::length(glm::vec3(m[2]));
		return r;
	}

	// get_euler(world, entity_id) -> float3  (degrees, YXZ order)
	das::float3 das_get_euler(aether::World* w, uint32_t id)
	{
		das::float3 r{};
		const auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return r;
		}
		const auto& mat = tc->localToWorld;

		float sx = glm::length(glm::vec3(mat[0]));
		float sy = glm::length(glm::vec3(mat[1]));
		float sz = glm::length(glm::vec3(mat[2]));
		glm::vec3 c2 = sz > 1e-6f ? glm::vec3(mat[2]) / sz : glm::vec3(0, 0, 1);
		float sinX = glm::clamp(-c2.y, -1.0f, 1.0f);
		float rotXRad = std::asin(sinX);
		float rotYRad = std::atan2(c2.x, c2.z);
		float rotZRad = 0.0f;
		float cosX = std::cos(rotXRad);
		if (std::abs(cosX) > 1e-4f)
		{
			glm::vec3 c0 = sx > 1e-6f ? glm::vec3(mat[0]) / sx : glm::vec3(1, 0, 0);
			glm::vec3 c1 = sy > 1e-6f ? glm::vec3(mat[1]) / sy : glm::vec3(0, 1, 0);
			rotZRad = std::atan2(c0.y, c1.y);
		}

		r.x = glm::degrees(rotXRad);
		r.y = glm::degrees(rotYRad);
		r.z = glm::degrees(rotZRad);
		return r;
	}

	// set_euler(world, entity_id, euler_deg) - updates only rotation, preserves translation and scale
	void das_set_euler(aether::World* w, uint32_t id, das::float3 euler)
	{
		auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id});
		if (!tc)
		{
			return;
		}

		glm::vec3 pos = glm::vec3(tc->localToWorld[3]);
		float sx = glm::length(glm::vec3(tc->localToWorld[0]));
		float sy = glm::length(glm::vec3(tc->localToWorld[1]));
		float sz = glm::length(glm::vec3(tc->localToWorld[2]));
		glm::vec3 scale = {sx, sy, sz};

		tc->localToWorld = ComposeTransform(pos, {euler.x, euler.y, euler.z}, scale);

		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (sec)
		{
			for (const auto eid : sec->entityIds)
			{
				if (auto* stc = w->TryGet<aether::TransformComponent>(aether::Entity{eid}))
				{
					stc->localToWorld = ComposeTransform(pos, {euler.x, euler.y, euler.z}, scale);
				}
			}
		}
	}

	// set_transform(world, entity_id, pos, euler_deg, scale)
	// Recomposes the full TRS matrix from the three float3 arguments.
	void das_set_transform(aether::World* w, uint32_t id, das::float3 pos, das::float3 euler, das::float3 scale)
	{
		const auto xform = ComposeTransform({pos.x, pos.y, pos.z}, {euler.x, euler.y, euler.z}, {scale.x, scale.y, scale.z});

		// Update script entity transform.
		if (auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id}))
		{
			tc->localToWorld = xform;
		}

		// Propagate to spawned mesh entities.
		const auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (sec)
		{
			for (const auto eid: sec->entityIds)
			{
				if (auto* stc = w->TryGet<aether::TransformComponent>(aether::Entity{eid}))
				{
					stc->localToWorld = xform;
				}
			}
		}
	}

	// ── Model loading ─────────────────────────────────────────────────────────

	// load_model(world, entity_id, path)
	void das_load_model(aether::World* w, uint32_t id, const char* path)
	{
		auto& ctx = ActiveContext();

		if (!ctx.defaultPipeline)
		{
			AE_WARN(aether::LogCategory::App, "load_model: no default pipeline set");
			return;
		}

		glm::mat4 xform{1.0f};

		if (const auto* tc = w->TryGet<aether::TransformComponent>(aether::Entity{id}))
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

		std::vector<aether::Entity> meshEntities = ctx.assets->SpawnModel(*modelPtr, *ctx.defaultPipeline, id);

		for (aether::Entity meshEntity: meshEntities)
		{
			if (auto* tc = w->TryGet<aether::TransformComponent>(meshEntity))
			{
				tc->localToWorld = xform * tc->localToWorld;
			}
			ctx.sceneEntities.push_back(meshEntity);
		}

		// Store spawned entity IDs for script-level propagation.
		auto* sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		if (!sec)
		{
			w->Emplace<aether::SpawnedEntitiesComponent>(aether::Entity{id});
			sec = w->TryGet<aether::SpawnedEntitiesComponent>(aether::Entity{id});
		}
		sec->entityIds.clear();
		for (const aether::Entity me: meshEntities)
		{
			sec->entityIds.push_back(me.id);
		}
	}

	// ── Entity iteration ──────────────────────────────────────────────────────

	// for_each_with_transform(world) <| $(e : uint) { ... }
	void das_for_each_with_transform(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
	{
		for (auto enttE: w->View<aether::TransformComponent>())
		{
			const uint32_t id = static_cast<uint32_t>(entt::to_integral(enttE));
			vec4f args[1];
			args[0] = das::cast<uint32_t>::from(id);
			ctx->invoke(block, args, nullptr, at);
		}
	}

	// ── Dynamic Tags ─────────────────────────────────────────────────────────

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

	// ── Primitive mesh caching ───────────────────────────────────────────────
	// create_mesh(world, type_string) -> uint
	// Caches a primitive mesh by type ("cube", "sphere", "plane", "quad", "triangle")
	// and returns a handle that can be passed to add_mesh.
	uint32_t das_create_mesh(aether::World* w, const char* type)
	{
		(void) w;
		auto& ctx = ActiveContext();
		if (!ctx.primitives || !ctx.defaultPipeline)
		{
			return 0u;
		}

		aether::PrimitiveMesh primType{};
		const std::string_view sv(type ? type : "");
		if (sv == "cube")
		{
			primType = aether::PrimitiveMesh::Cube;
		}
		else if (sv == "sphere")
		{
			primType = aether::PrimitiveMesh::Sphere;
		}
		else if (sv == "plane")
		{
			primType = aether::PrimitiveMesh::Plane;
		}
		else if (sv == "quad")
		{
			primType = aether::PrimitiveMesh::Quad;
		}
		else if (sv == "triangle")
		{
			primType = aether::PrimitiveMesh::Triangle;
		}
		else
		{
			return 0u;
		}

		// Register default material on first use.
		if (!ctx.defaultMaterialRegistered && ctx.assets)
		{
			ctx.defaultMaterial = {};
			ctx.defaultMaterial.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.f);
			ctx.defaultMaterial.roughnessFactor = 0.6f;
			ctx.defaultMaterial.metallicFactor = 0.0f;
			ctx.assets->RegisterMaterial(ctx.defaultMaterial);
			ctx.defaultMaterialRegistered = true;
		}

		SceneContext::CachedMesh entry;
		entry.mesh = &ctx.primitives->Get(primType);
		entry.pipeline = ctx.defaultPipeline;
		entry.material = ctx.defaultMaterial;
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
		if (!entry.mesh || !entry.pipeline)
		{
			return;
		}

		const aether::Entity e{entityId};
		w->EmplaceOrReplace<aether::PipelineComponent>(e, aether::PipelineComponent{.pipeline = entry.pipeline});
		w->EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = entry.mesh});
		w->EmplaceOrReplace<aether::MaterialComponent>(e, aether::MaterialComponent{.material = entry.material});
	}

} // namespace

// ── Module ────────────────────────────────────────────────────────────────────

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
			addAnnotation(das::make_smart<das::DummyTypeAnnotation>("World", "::aether::World", sizeof(void*), alignof(void*)));

			// Entity lifecycle
			Bind<das_entity_create>(lib, "entity_create", SE::modifyExternal);
			Bind<das_entity_destroy>(lib, "entity_destroy", SE::modifyExternal);
			Bind<das_entity_valid>(lib, "entity_valid", SE::none);

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

			// Entity iteration
			Bind<das_for_each_with_transform>(lib, "for_each_with_transform", SE::modifyExternal);

			// Dynamic Tags
			Bind<das_create_tag>(lib, "create_tag", SE::modifyExternal);
			Bind<das_get_tag_id>(lib, "get_tag_id", SE::accessExternal);
			Bind<das_add_tag>(lib, "add_tag", SE::modifyExternal);
			Bind<das_has_tag>(lib, "has_tag", SE::accessExternal);
			Bind<das_remove_tag>(lib, "remove_tag", SE::modifyExternal);
			Bind<das_for_each_with_tag>(lib, "for_each_with_tag", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

// WorldModule does not use AETHER_DAS_MODULE - ScriptingSubsystem registers it
// explicitly as the first module so all other modules can resolve World* types.
REGISTER_MODULE_IN_NAMESPACE(WorldModule, aether::app::scripting)
