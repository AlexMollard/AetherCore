#include "WebModels.hpp"

#include "scene/World.hpp"
#include <limits>
#include "scene/Components.hpp"

namespace aether::app::web
{
	Expected<std::uint64_t> WebModels::MeasureSnapshotMaterialPaths(World& world, Entity entity) const
	{
		const auto* component = world.TryGet<MaterialInstanceComponent>(entity);
		if (!component)
		{
			return std::uint64_t{0};
		}
		const auto& material = component->asset;
		const TextureHandle handles[] = {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex};
		std::uint64_t bytes = 0;
		for (const auto handle: handles)
		{
			if (!handle.IsValid() || handle == m_textures.DefaultHandle() || handle == m_textures.WhiteHandle())
			{
				continue;
			}
			std::size_t length = 0;
			if (!m_textures.TryGetPathSize(handle, length))
			{
				return std::unexpected(AetherError::Asset("Browser streaming material has an unresolvable texture"));
			}
			if (length >= std::numeric_limits<std::uint64_t>::max() - bytes)
			{
				return std::unexpected(AetherError::Asset("Browser streaming material reference size overflow"));
			}
			bytes += static_cast<std::uint64_t>(length) + 1;
		}
		return bytes;
	}

	Expected<void> WebModels::CaptureSnapshotMaterial(World& world, Entity entity, scene::EntityRecord& record) const
	{
		const auto* component = world.TryGet<MaterialInstanceComponent>(entity);
		if (!component)
		{
			return {};
		}
		scene::MaterialRecord material;
		material.asset = component->asset;
		const TextureHandle handles[] = {material.asset.albedoTex, material.asset.normalTex, material.asset.metallicRoughnessTex, material.asset.occlusionTex, material.asset.emissiveTex};
		std::string* paths[] = {&material.albedoPath, &material.normalPath, &material.metallicRoughnessPath, &material.occlusionPath, &material.emissivePath};
		for (unsigned i = 0; i < 5; ++i)
		{
			if (handles[i].IsValid() && handles[i] != m_textures.DefaultHandle() && handles[i] != m_textures.WhiteHandle() && !m_textures.TryGetPath(handles[i], *paths[i]))
			{
				return std::unexpected(AetherError::Asset("Browser streaming material has an unresolvable texture"));
			}
		}
		material.asset.albedoTex = {};
		material.asset.normalTex = {};
		material.asset.metallicRoughnessTex = {};
		material.asset.occlusionTex = {};
		material.asset.emissiveTex = {};
		record.material = std::move(material);
		return {};
	}
} // namespace aether::app::web
