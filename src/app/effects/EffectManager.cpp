#include "EffectManager.hpp"

#include "assets/AssetManager.hpp"
#include "utils/Profiler.hpp"

namespace aether::app::effects
{
	void EffectManager::Register(const char* name, aether::GraphicsPipeline pipeline, const aether::MaterialAsset& material)
	{
		AE_PROFILE_ZONE();
		EffectData data;
		data.pipeline = std::move(pipeline);
		data.material = material;
		m_effects[name] = std::move(data);
	}

	bool EffectManager::CreateAndRegister(
	        const char* name, aether::AssetManager& assets, const void* descriptorHeapMappings, aether::gpu::Format colorFormat, aether::gpu::Format depthFormat, const char* shaderVfsPath, const aether::MaterialAsset& material)
	{
		AE_PROFILE_ZONE();
		auto result = assets.CreateGraphicsPipeline({
		        .shaderVfsPath = shaderVfsPath,
		        .colorFormat = colorFormat,
		        .depthFormat = depthFormat,
		        .depthTestEnable = true,
		        .depthWriteEnable = true,
		        .depthCompareOp = aether::gpu::CompareOp::LessOrEqual,
		        .descriptorHeapMappings = descriptorHeapMappings,
		});

		if (!result)
		{
			return false;
		}

		Register(name, std::move(result.value()), material);
		return true;
	}

	const EffectData* EffectManager::Find(const char* name) const
	{
		auto it = m_effects.find(name ? name : "");
		return it != m_effects.end() ? &it->second : nullptr;
	}

	void EffectManager::DestroyAll()
	{
		AE_PROFILE_ZONE();
		for (auto& [name, data]: m_effects)
		{
			data.pipeline.Destroy();
		}
		m_effects.clear();
	}
} // namespace aether::app::effects
