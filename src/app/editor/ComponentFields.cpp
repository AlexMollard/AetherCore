#include "editor/ComponentFields.hpp"

#include <algorithm>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "assets/AssetManager.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/PipelineCache.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

// Legacy field registry. Component reflection (scene/reflection/) is the successor
// and covers most components via one declaration each; this now holds only the
// components not yet migrated because they need bespoke access - currently just
// Material (copy-on-write via MaterialInstanceComponent + MaterialSystem). The MCP
// checks reflection first and falls back here.

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
