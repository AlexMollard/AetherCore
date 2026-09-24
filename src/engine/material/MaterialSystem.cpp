#include "material/MaterialSystem.hpp"

#include <entt/entt.hpp>

#include "gpu/GpuTypes.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/PipelineCache.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		void OnMaterialDestroyed(MaterialRegistry& registry, entt::registry& r, entt::entity e)
		{
			registry.Release(r.get<MaterialComponent>(e).handle);
		}

		// Owns the one connection ConnectLifecycle made, so DisconnectLifecycle can
		// drop exactly that sink. sink.disconnect() detaches EVERY observer on the
		// signal, and disconnect<&Fn>(registry) needs the registry the API no
		// longer receives; release() needs neither. No-op until connected.
		entt::connection s_materialConnection{};
	} // namespace

	void MaterialSystem::ConnectLifecycle(World& world, MaterialRegistry& registry)
	{
		s_materialConnection = world.GetRegistry().on_destroy<MaterialComponent>().connect<&OnMaterialDestroyed>(registry);
	}

	void MaterialSystem::DisconnectLifecycle(World&)
	{
		s_materialConnection.release();
	}

	void MaterialSystem::AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const MaterialAsset& asset)
	{
		auto& r = world.GetRegistry();
		const entt::entity e = World::ToEntt(entity);
		MaterialHandle old{};
		if (const auto* existing = r.try_get<MaterialComponent>(e))
		{
			old = existing->handle;
		}
		const MaterialHandle h = registry.Acquire(asset);
		registry.Release(old);
		r.emplace_or_replace<MaterialComponent>(e, MaterialComponent{h, registry.ResolveSlot(h)});

		if (r.all_of<EffectParamsComponent>(e))
		{
			return;
		}

		MaterialTemplate tmpl = asset.templateDesc;
		tmpl.cullMode = asset.doubleSided ? gpu::CullMode::None : gpu::CullMode::Back;
		tmpl.blendEnable = asset.alphaBlend;
		// Transparent geometry must not write depth: the surfaces behind it still have to be
		// visible through it, and a depth write would reject them. Only ever cleared, never
		// set - an opaque material keeps whatever its own template asked for.
		if (asset.alphaBlend)
		{
			tmpl.depthWriteEnable = false;
		}
		const GraphicsPipeline* pipeline = pipelineCache.Acquire(tmpl);
		r.emplace_or_replace<PipelineComponent>(e, PipelineComponent{pipeline, asset.alphaBlend});
	}

	namespace
	{
		struct InstanceRef
		{
			MaterialInstanceComponent& inst;
			bool created;
		};

		InstanceRef GetOrSeedInstance(entt::registry& r, entt::entity e, const MaterialRegistry& registry)
		{
			if (auto* existing = r.try_get<MaterialInstanceComponent>(e))
			{
				return {*existing, false};
			}
			MaterialAsset seed{};
			if (const auto* mc = r.try_get<MaterialComponent>(e))
			{
				registry.TryDescribe(mc->handle, seed);
			}
			return {r.emplace<MaterialInstanceComponent>(e, MaterialInstanceComponent{seed}), true};
		}
	} // namespace

	void MaterialSystem::SetBaseColor(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && glm::vec3(ref.inst.asset.baseColorFactor) == color)
		{
			return;
		}
		ref.inst.asset.baseColorFactor = glm::vec4(color, ref.inst.asset.baseColorFactor.w);
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}

	void MaterialSystem::SetMetallic(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && ref.inst.asset.metallicFactor == value)
		{
			return;
		}
		ref.inst.asset.metallicFactor = value;
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}

	void MaterialSystem::SetRoughness(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && ref.inst.asset.roughnessFactor == value)
		{
			return;
		}
		ref.inst.asset.roughnessFactor = value;
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}

	void MaterialSystem::SetEmissive(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, const glm::vec3& color)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && ref.inst.asset.emissiveFactor == color)
		{
			return;
		}
		ref.inst.asset.emissiveFactor = color;
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}

	glm::vec3 MaterialSystem::GetEmissive(World& world, Entity entity, const MaterialRegistry& registry)
	{
		// Deliberately NOT GetOrSeedInstance - a read must not have the side effect of
		// emplacing a MaterialInstanceComponent (that would make a plain query start
		// looking, to the scene serializer's own CaptureMaterial, exactly like a real
		// per-entity override). Same priority order as the setters would have seeded
		// from, just without writing it back: an existing instance override wins, else
		// whatever the entity's shared MaterialComponent asset describes, else black.
		const auto& r = world.GetRegistry();
		const entt::entity e = World::ToEntt(entity);
		if (const auto* inst = r.try_get<MaterialInstanceComponent>(e))
		{
			return inst->asset.emissiveFactor;
		}
		if (const auto* mc = r.try_get<MaterialComponent>(e))
		{
			MaterialAsset described{};
			if (registry.TryDescribe(mc->handle, described))
			{
				return described.emissiveFactor;
			}
		}
		return glm::vec3(0.0f);
	}

	void MaterialSystem::SetOcclusion(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, float value)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && ref.inst.asset.occlusionStrength == value)
		{
			return;
		}
		ref.inst.asset.occlusionStrength = value;
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}

	void MaterialSystem::SetAlbedoTexture(World& world, Entity entity, MaterialRegistry& registry, PipelineCache& pipelineCache, TextureHandle texture)
	{
		const InstanceRef ref = GetOrSeedInstance(world.GetRegistry(), World::ToEntt(entity), registry);
		if (!ref.created && ref.inst.asset.albedoTex == texture)
		{
			return;
		}
		// so texture ownership follows the material with no extra bookkeeping here.
		ref.inst.asset.albedoTex = texture;
		AssignMaterial(world, entity, registry, pipelineCache, ref.inst.asset);
	}
} // namespace aether
