#include "WorldModule.hpp"
#include "DasHelpers.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "daScript/daScript.h"

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"

namespace
{
	using namespace aether;
	using namespace aether::app::scripting;

	glm::mat4 ComposeTransform(glm::vec3 pos, glm::vec3 rotEulerDeg, glm::vec3 scale)
	{
		glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
		glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.y), glm::vec3(0, 1, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.x), glm::vec3(1, 0, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.z), glm::vec3(0, 0, 1));
		glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
		return t * r * s;
	}

	// spawn() -> uint  (entity id; 0 = invalid)
	uint32_t das_spawn()
	{
		auto& ctx = ActiveContext();
		auto handle = ctx.world->Spawn();
		aether::Entity e = handle.entity();
		handle.Add<aether::TransformComponent>();
		ctx.sceneEntities.push_back(e);
		ctx.pendingEntities[e.id] = { glm::mat4(1.0f), {} };
		return e.id;
	}

	// set_transform(entity_id, pos, rot_euler_deg, scale)
	void das_set_transform(uint32_t entity_id, das::float3 pos, das::float3 rot, das::float3 scale)
	{
		auto& ctx = ActiveContext();
		aether::Entity entity{ entity_id };
		const glm::mat4 xform = ComposeTransform({ pos.x, pos.y, pos.z }, { rot.x, rot.y, rot.z }, { scale.x, scale.y, scale.z });

		auto it = ctx.pendingEntities.find(entity_id);
		if (it != ctx.pendingEntities.end())
		{
			it->second.transform = xform;
			if (auto* tc = ctx.world->TryGet<aether::TransformComponent>(entity))
			{
				tc->localToWorld = xform;
			}
			for (auto meshEntity: it->second.meshEntities)
			{
				if (auto* tc = ctx.world->TryGet<aether::TransformComponent>(meshEntity))
				{
					tc->localToWorld = xform;
				}
			}
		}
	}

	// load_model(entity_id, path)
	void das_load_model(uint32_t entity_id, const char* path)
	{
		auto& ctx = ActiveContext();
		if (!ctx.defaultPipeline)
		{
			WARN(LogCategory::App, "das load_model: no default pipeline set");
			return;
		}
		auto it = ctx.pendingEntities.find(entity_id);
		glm::mat4 xform = (it != ctx.pendingEntities.end()) ? it->second.transform : glm::mat4(1.0f);

		auto result = ctx.assets->LoadModel(path);
		if (!result)
		{
			WARN(LogCategory::App, "das load_model: failed to load '{}'", path);
			return;
		}

		ctx.loadedModels.push_back(std::move(result.value()));
		aether::LoadedModel& model = ctx.loadedModels.back();
		std::vector<aether::Entity> meshEntities = ctx.assets->SpawnModel(model, *ctx.defaultPipeline);

		for (aether::Entity meshEntity: meshEntities)
		{
			if (auto* tc = ctx.world->TryGet<aether::TransformComponent>(meshEntity))
			{
				tc->localToWorld = xform * tc->localToWorld;
			}
			ctx.sceneEntities.push_back(meshEntity);
		}
		if (it != ctx.pendingEntities.end())
		{
			it->second.meshEntities = meshEntities;
		}
	}

	// set_animator(entity_id, clip)
	void das_set_animator(uint32_t /*entity_id*/, const char* /*clip*/)
	{
	}

	// destroy(entity_id)
	void das_destroy(uint32_t entity_id)
	{
		auto& ctx = ActiveContext();
		auto it = ctx.pendingEntities.find(entity_id);
		if (it != ctx.pendingEntities.end())
		{
			for (aether::Entity meshEntity: it->second.meshEntities)
			{
				ctx.world->Destroy(meshEntity);
			}
			ctx.pendingEntities.erase(it);
		}
		ctx.world->Destroy(aether::Entity{ entity_id });
	}
} // namespace

namespace aether::app::scripting
{
	struct WorldModule : das::Module
	{
		WorldModule()
		      : das::Module("world")
		{
			das::ModuleLibrary lib(this);

			addExtern<DAS_BIND_FUN(das_spawn)>(*this, lib, "spawn", das::SideEffects::modifyExternal, "das_spawn");
			addExtern<DAS_BIND_FUN(das_set_transform)>(*this, lib, "set_transform", das::SideEffects::modifyExternal, "das_set_transform");
			addExtern<DAS_BIND_FUN(das_load_model)>(*this, lib, "load_model", das::SideEffects::modifyExternal, "das_load_model");
			addExtern<DAS_BIND_FUN(das_set_animator)>(*this, lib, "set_animator", das::SideEffects::modifyExternal, "das_set_animator");
			addExtern<DAS_BIND_FUN(das_destroy)>(*this, lib, "destroy", das::SideEffects::modifyExternal, "das_destroy");

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

REGISTER_MODULE_IN_NAMESPACE(WorldModule, aether::app::scripting);

void RegisterWorldModule()
{
	NEED_MODULE(WorldModule);
}
