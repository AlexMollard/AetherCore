#include "scripting/interop/InteropCommon.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "assets/AssetManager.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialAuthoring.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

// Model loading, primitive meshes, and materials exported to C#: model/mesh
// caches live on the SceneContext, materials go through MaterialSystem
// (per-entity) and MaterialAuthoring (shared).

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// ── Model loading ─────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_load_model(std::uint32_t id, const char* pathC)
{
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	const std::string path = pathC != nullptr ? pathC : "";
	if (ctx.assets == nullptr)
	{
		AE_WARN(aether::LogCategory::App, "load_model: no asset manager");
		return;
	}

	glm::mat4 xform{1.0f};
	if (const auto* tc = w.TryGet<aether::TransformComponent>(aether::Entity{id}))
	{
		xform = tc->localToWorld;
	}

	aether::LoadedModel* modelPtr = nullptr;
	if (const auto it = ctx.loadedModelMap.find(path); it != ctx.loadedModelMap.end())
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

	// Name the logical entity after the model file stem.
	std::string stem = path;
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
	w.EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = stem});

	// Reloading onto the same entity re-links children instead of accumulating.
	if (const auto* h = w.TryGet<aether::HierarchyComponent>(aether::Entity{id}))
	{
		const std::vector<aether::Entity> stale = h->children;
		for (const aether::Entity s: stale)
		{
			aether::ecs::DetachFromParent(w, s);
		}
	}

	std::vector<aether::Entity> meshEntities = ctx.assets->SpawnModel(*modelPtr, id);
	for (std::size_t i = 0; i < meshEntities.size(); ++i)
	{
		const aether::Entity meshEntity = meshEntities[i];
		if (auto* tc = w.TryGet<aether::TransformComponent>(meshEntity))
		{
			tc->localToWorld = xform * tc->localToWorld;
		}
		w.EmplaceOrReplace<aether::NameComponent>(meshEntity, aether::NameComponent{.name = stem + " mesh"});
		w.EmplaceOrReplace<aether::MeshSourceComponent>(meshEntity, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Model, .path = path, .primitiveIndex = static_cast<std::uint32_t>(i)});
		ctx.sceneEntities.push_back(meshEntity);
	}
}

// ── Primitive meshes ──────────────────────────────────────────────────────────

// create_mesh(type) -> handle; caches a primitive ("cube","sphere","plane",
// "quad","triangle"). 0 is returned for an unknown type or missing primitives.
AE_SCRIPT_API std::uint32_t aether_create_mesh(const char* typeC)
{
	auto& ctx = ActiveContext();
	if (ctx.primitives == nullptr)
	{
		return 0;
	}

	aether::PrimitiveMesh primType{};
	// Assigned together with primType in every branch below; the else path
	// returns, so it is always set before the read.
	const char* displayName;
	const std::string_view sv(typeC != nullptr ? typeC : "");
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
		return 0;
	}

	if (!ctx.defaultMaterialInitialized)
	{
		ctx.defaultMaterial = {};
		ctx.defaultMaterial.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
		ctx.defaultMaterial.roughnessFactor = 0.6f;
		ctx.defaultMaterial.metallicFactor = 0.0f;
		ctx.defaultMaterial.modulateVertexColor = true;
		ctx.defaultMaterial.doubleSided = true;
		ctx.defaultMaterialInitialized = true;
	}

	SceneContext::CachedMesh entry;
	entry.mesh = &ctx.primitives->Get(primType);
	entry.materialAsset = ctx.defaultMaterial;
	entry.displayName = displayName;
	entry.kindName = std::string(sv);
	ctx.meshCache.push_back(std::move(entry));
	return static_cast<std::uint32_t>(ctx.meshCache.size() - 1);
}

AE_SCRIPT_API void aether_add_mesh(std::uint32_t entityId, std::uint32_t meshHandle)
{
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	if (meshHandle >= ctx.meshCache.size())
	{
		return;
	}
	const auto& entry = ctx.meshCache[meshHandle];
	if (entry.mesh == nullptr || ctx.assets == nullptr)
	{
		return;
	}

	const aether::Entity e{entityId};
	w.EmplaceOrReplace<aether::MeshComponent>(e, aether::MeshComponent{.mesh = entry.mesh});
	w.EmplaceOrReplace<aether::MeshSourceComponent>(e, aether::MeshSourceComponent{.kind = aether::MeshSourceComponent::Kind::Primitive, .path = entry.kindName, .primitiveIndex = 0});
	aether::MaterialSystem::AssignMaterial(w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), entry.materialAsset);

	const auto* nc = w.TryGet<aether::NameComponent>(e);
	if (nc == nullptr || nc->name.empty() || nc->name == "Entity")
	{
		w.EmplaceOrReplace<aether::NameComponent>(e, aether::NameComponent{.name = entry.displayName});
	}
}

// ── Per-entity solid material paint ───────────────────────────────────────────

AE_SCRIPT_API void aether_set_material(std::uint32_t id, Vec3 color, float metallic, float roughness)
{
	auto& ctx = ActiveContext();
	auto& w = ActiveWorld();
	if (ctx.assets == nullptr)
	{
		return;
	}
	aether::MaterialAsset asset;
	asset.baseColorFactor = glm::vec4(ToGlm(color), 1.0f);
	asset.metallicFactor = metallic;
	asset.roughnessFactor = roughness;
	const aether::Entity e{id};
	w.EmplaceOrReplace<aether::MaterialInstanceComponent>(e, aether::MaterialInstanceComponent{asset});
	aether::MaterialSystem::AssignMaterial(w, e, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), asset);
}

AE_SCRIPT_API void aether_set_material_color(std::uint32_t id, Vec3 color)
{
	aether_set_material(id, color, 0.0f, 0.6f);
}

// ── Shared authoring materials ────────────────────────────────────────────────

AE_SCRIPT_API std::uint32_t aether_make_material(Vec3 color, float metallic, float roughness)
{
	auto& ctx = ActiveContext();
	if (ctx.assets == nullptr)
	{
		return aether::MaterialAuthoring::kInvalidId;
	}
	aether::MaterialAsset asset;
	asset.baseColorFactor = glm::vec4(ToGlm(color), 1.0f);
	asset.metallicFactor = metallic;
	asset.roughnessFactor = roughness;
	return ctx.assets->GetMaterialAuthoring().Create(asset);
}

AE_SCRIPT_API void aether_bind_material(std::uint32_t entityId, std::uint32_t materialId)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().Bind(ActiveWorld(), aether::Entity{entityId}, materialId);
	}
}

AE_SCRIPT_API void aether_material_set_color(std::uint32_t materialId, Vec3 color)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetBaseColor(ActiveWorld(), materialId, ToGlm(color));
	}
}

AE_SCRIPT_API void aether_material_set_metallic(std::uint32_t materialId, float value)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetMetallic(ActiveWorld(), materialId, value);
	}
}

AE_SCRIPT_API void aether_material_set_roughness(std::uint32_t materialId, float value)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetRoughness(ActiveWorld(), materialId, value);
	}
}

AE_SCRIPT_API void aether_material_set_emissive(std::uint32_t materialId, Vec3 color)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		ctx.assets->GetMaterialAuthoring().SetEmissive(ActiveWorld(), materialId, ToGlm(color));
	}
}

// ── Per-entity material instance edits (copy-on-write) ────────────────────────

AE_SCRIPT_API void aether_entity_material_set_color(std::uint32_t entityId, Vec3 color)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetBaseColor(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), ToGlm(color));
	}
}

AE_SCRIPT_API void aether_entity_material_set_metallic(std::uint32_t entityId, float value)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetMetallic(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
	}
}

AE_SCRIPT_API void aether_entity_material_set_roughness(std::uint32_t entityId, float value)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetRoughness(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), value);
	}
}

AE_SCRIPT_API void aether_entity_material_set_emissive(std::uint32_t entityId, Vec3 color)
{
	auto& ctx = ActiveContext();
	if (ctx.assets != nullptr)
	{
		aether::MaterialSystem::SetEmissive(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), ToGlm(color));
	}
}

AE_SCRIPT_API void aether_set_material_texture(std::uint32_t entityId, const char* path)
{
	auto& ctx = ActiveContext();
	if (ctx.assets == nullptr)
	{
		return;
	}
	auto& texReg = ctx.assets->GetTextureRegistry();
	const aether::TextureHandle tex = texReg.Acquire(path != nullptr ? path : "");
	aether::MaterialSystem::SetAlbedoTexture(ActiveWorld(), aether::Entity{entityId}, ctx.assets->GetMaterialRegistry(), ctx.assets->GetPipelineCache(), tex);
	texReg.Release(tex);
}
