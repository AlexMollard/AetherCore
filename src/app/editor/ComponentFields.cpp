#include "editor/ComponentFields.hpp"

#include <algorithm>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "assets/AssetManager.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/PipelineCache.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	namespace
	{
		using nlohmann::json;

		glm::vec3 JVec3(const json& j, glm::vec3 fb)
		{
			if (j.is_array() && j.size() >= 3) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }
			return fb;
		}
		glm::vec4 JVec4(const json& j, glm::vec4 fb)
		{
			if (j.is_array() && j.size() >= 4) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()}; }
			return fb;
		}
		json Arr3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
		json Arr4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

		// --- Point Light -----------------------------------------------------------
		bool ReadPointLight(const World& w, Entity e, ServiceContainer&, json& out)
		{
			const auto* pl = w.TryGet<PointLightComponent>(e);
			if (pl == nullptr) { return false; }
			out["color"] = Arr3(pl->color);
			out["intensity"] = pl->intensity;
			out["radius"] = pl->radius;
			out["castsShadow"] = pl->castsShadow;
			return true;
		}
		std::vector<std::string> WritePointLight(World& w, Entity e, const json& v, ServiceContainer&)
		{
			std::vector<std::string> applied;
			auto* pl = w.TryGet<PointLightComponent>(e);
			if (pl == nullptr) { return applied; }
			if (v.contains("color")) { pl->color = JVec3(v["color"], pl->color); applied.emplace_back("color"); }
			if (v.contains("intensity")) { pl->intensity = v["intensity"].get<float>(); applied.emplace_back("intensity"); }
			if (v.contains("radius")) { pl->radius = v["radius"].get<float>(); applied.emplace_back("radius"); }
			if (v.contains("castsShadow")) { pl->castsShadow = v["castsShadow"].get<bool>(); applied.emplace_back("castsShadow"); }
			return applied;
		}

		// --- Spot Light ------------------------------------------------------------
		bool ReadSpotLight(const World& w, Entity e, ServiceContainer&, json& out)
		{
			const auto* sl = w.TryGet<SpotLightComponent>(e);
			if (sl == nullptr) { return false; }
			out["color"] = Arr3(sl->color);
			out["intensity"] = sl->intensity;
			out["radius"] = sl->radius;
			out["innerAngleDeg"] = glm::degrees(sl->innerAngleRad);
			out["outerAngleDeg"] = glm::degrees(sl->outerAngleRad);
			out["castsShadow"] = sl->castsShadow;
			return true;
		}
		std::vector<std::string> WriteSpotLight(World& w, Entity e, const json& v, ServiceContainer&)
		{
			std::vector<std::string> applied;
			auto* sl = w.TryGet<SpotLightComponent>(e);
			if (sl == nullptr) { return applied; }
			if (v.contains("color")) { sl->color = JVec3(v["color"], sl->color); applied.emplace_back("color"); }
			if (v.contains("intensity")) { sl->intensity = v["intensity"].get<float>(); applied.emplace_back("intensity"); }
			if (v.contains("radius")) { sl->radius = v["radius"].get<float>(); applied.emplace_back("radius"); }
			if (v.contains("innerAngleDeg")) { sl->innerAngleRad = glm::radians(v["innerAngleDeg"].get<float>()); applied.emplace_back("innerAngleDeg"); }
			if (v.contains("outerAngleDeg")) { sl->outerAngleRad = glm::radians(v["outerAngleDeg"].get<float>()); applied.emplace_back("outerAngleDeg"); }
			sl->outerAngleRad = std::max(sl->outerAngleRad, sl->innerAngleRad); // keep the cone valid
			if (v.contains("castsShadow")) { sl->castsShadow = v["castsShadow"].get<bool>(); applied.emplace_back("castsShadow"); }
			return applied;
		}

		// --- Skinned Mesh ----------------------------------------------------------
		bool ReadSkinned(const World& w, Entity e, ServiceContainer&, json& out)
		{
			const auto* smc = w.TryGet<SkinnedMeshComponent>(e);
			if (smc == nullptr) { return false; }
			out["clip"] = smc->clipIndex;
			out["speed"] = smc->playbackSpeed;
			out["time"] = smc->animTime;
			out["looping"] = smc->looping;
			return true;
		}
		std::vector<std::string> WriteSkinned(World& w, Entity e, const json& v, ServiceContainer&)
		{
			std::vector<std::string> applied;
			auto* smc = w.TryGet<SkinnedMeshComponent>(e);
			if (smc == nullptr) { return applied; }
			if (v.contains("clip")) { smc->clipIndex = static_cast<std::uint32_t>(std::max(0, v["clip"].get<int>())); smc->animTime = 0.0f; applied.emplace_back("clip"); }
			if (v.contains("speed")) { smc->playbackSpeed = v["speed"].get<float>(); applied.emplace_back("speed"); }
			if (v.contains("time")) { smc->animTime = v["time"].get<float>(); applied.emplace_back("time"); }
			if (v.contains("looping")) { smc->looping = v["looping"].get<bool>(); applied.emplace_back("looping"); }
			return applied;
		}

		// --- Camera ----------------------------------------------------------------
		bool ReadCamera(const World& w, Entity e, ServiceContainer&, json& out)
		{
			const auto* cam = w.TryGet<CameraComponent>(e);
			if (cam == nullptr) { return false; }
			out["fov"] = cam->fovDegrees;
			out["near"] = cam->nearPlane;
			out["far"] = cam->farPlane;
			out["main"] = w.Has<MainCameraComponent>(e);
			return true;
		}
		std::vector<std::string> WriteCamera(World& w, Entity e, const json& v, ServiceContainer&)
		{
			std::vector<std::string> applied;
			auto* cam = w.TryGet<CameraComponent>(e);
			if (cam == nullptr) { return applied; }
			if (v.contains("fov")) { cam->fovDegrees = v["fov"].get<float>(); applied.emplace_back("fov"); }
			if (v.contains("near")) { cam->nearPlane = v["near"].get<float>(); applied.emplace_back("near"); }
			if (v.contains("far")) { cam->farPlane = v["far"].get<float>(); applied.emplace_back("far"); }
			if (v.contains("main"))
			{
				const bool main = v["main"].get<bool>();
				if (main && !w.Has<MainCameraComponent>(e)) { w.Emplace<MainCameraComponent>(e, MainCameraComponent{}); }
				else if (!main && w.Has<MainCameraComponent>(e)) { w.Remove<MainCameraComponent>(e); }
				applied.emplace_back("main");
			}
			return applied;
		}

		// --- Material (copy-on-write via MaterialInstanceComponent) ----------------
		// Mirrors DrawMaterial: edits land on a per-entity instance seeded from the
		// registry material, then MaterialSystem::AssignMaterial re-commits it.
		bool DescribeMaterial(const World& w, Entity e, ServiceContainer& services, MaterialAsset& out)
		{
			if (const auto* inst = w.TryGet<MaterialInstanceComponent>(e)) { out = inst->asset; return true; }
			const auto* mc = w.TryGet<MaterialComponent>(e);
			auto* assets = services.TryGet<AssetManager>();
			if (mc == nullptr || assets == nullptr) { return false; }
			return assets->GetMaterialRegistry().TryDescribe(mc->handle, out);
		}
		bool ReadMaterial(const World& w, Entity e, ServiceContainer& services, json& out)
		{
			if (w.TryGet<MaterialComponent>(e) == nullptr) { return false; }
			MaterialAsset a{};
			if (!DescribeMaterial(w, e, services, a)) { return false; }
			out["baseColor"] = Arr4(a.baseColorFactor);
			out["metallic"] = a.metallicFactor;
			out["roughness"] = a.roughnessFactor;
			out["occlusion"] = a.occlusionStrength;
			out["emissive"] = Arr3(a.emissiveFactor);
			out["doubleSided"] = a.doubleSided;
			out["alphaBlend"] = a.alphaBlend;
			out["alphaMask"] = a.alphaMask;
			out["alphaCutoff"] = a.alphaCutoff;
			out["receiveShadows"] = a.receiveShadows;
			return true;
		}
		std::vector<std::string> WriteMaterial(World& w, Entity e, const json& v, ServiceContainer& services)
		{
			std::vector<std::string> applied;
			auto* mc = w.TryGet<MaterialComponent>(e);
			auto* assets = services.TryGet<AssetManager>();
			if (mc == nullptr || assets == nullptr) { return applied; }

			// Seed a live instance from the current material so untouched fields
			// (textures, other factors) are preserved.
			auto* inst = w.TryGet<MaterialInstanceComponent>(e);
			if (inst == nullptr)
			{
				MaterialAsset seed{};
				if (!assets->GetMaterialRegistry().TryDescribe(mc->handle, seed)) { return applied; }
				inst = &w.Emplace<MaterialInstanceComponent>(e, MaterialInstanceComponent{seed});
			}
			MaterialAsset& a = inst->asset;
			if (v.contains("baseColor")) { a.baseColorFactor = JVec4(v["baseColor"], a.baseColorFactor); applied.emplace_back("baseColor"); }
			if (v.contains("metallic")) { a.metallicFactor = v["metallic"].get<float>(); applied.emplace_back("metallic"); }
			if (v.contains("roughness")) { a.roughnessFactor = v["roughness"].get<float>(); applied.emplace_back("roughness"); }
			if (v.contains("occlusion")) { a.occlusionStrength = v["occlusion"].get<float>(); applied.emplace_back("occlusion"); }
			if (v.contains("emissive")) { a.emissiveFactor = JVec3(v["emissive"], a.emissiveFactor); applied.emplace_back("emissive"); }
			if (v.contains("doubleSided")) { a.doubleSided = v["doubleSided"].get<bool>(); applied.emplace_back("doubleSided"); }
			if (v.contains("alphaBlend")) { a.alphaBlend = v["alphaBlend"].get<bool>(); applied.emplace_back("alphaBlend"); }
			if (v.contains("alphaMask")) { a.alphaMask = v["alphaMask"].get<bool>(); applied.emplace_back("alphaMask"); }
			if (v.contains("alphaCutoff")) { a.alphaCutoff = v["alphaCutoff"].get<float>(); applied.emplace_back("alphaCutoff"); }
			if (v.contains("receiveShadows")) { a.receiveShadows = v["receiveShadows"].get<bool>(); applied.emplace_back("receiveShadows"); }

			if (!applied.empty())
			{
				MaterialSystem::AssignMaterial(w, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), a);
			}
			return applied;
		}

		const std::vector<ComponentFieldSet>& BuildSets()
		{
			static const std::vector<ComponentFieldSet> sets = {
			        {"Point Light", "color:vec3, intensity:float, radius:float, castsShadow:bool", ReadPointLight, WritePointLight},
			        {"Spot Light", "color:vec3, intensity:float, radius:float, innerAngleDeg:float, outerAngleDeg:float, castsShadow:bool", ReadSpotLight, WriteSpotLight},
			        {"Skinned Mesh", "clip:int, speed:float, time:float, looping:bool", ReadSkinned, WriteSkinned},
			        {"Camera", "fov:float, near:float, far:float, main:bool", ReadCamera, WriteCamera},
			        {"Material", "baseColor:vec4, metallic:float, roughness:float, occlusion:float, emissive:vec3, doubleSided:bool, alphaBlend:bool, alphaMask:bool, alphaCutoff:float, receiveShadows:bool", ReadMaterial, WriteMaterial},
			};
			return sets;
		}
	} // namespace

	const std::vector<ComponentFieldSet>& ComponentFieldSets()
	{
		return BuildSets();
	}

	const ComponentFieldSet* FindComponentFields(std::string_view name)
	{
		const auto& sets = BuildSets();
		const auto it = std::find_if(sets.begin(), sets.end(), [&](const ComponentFieldSet& s) { return s.name == name; });
		return it != sets.end() ? &*it : nullptr;
	}
} // namespace aether::editor
