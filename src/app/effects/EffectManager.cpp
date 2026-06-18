#include "EffectManager.hpp"

#include "assets/AssetManager.hpp"

namespace aether::app::effects
{
	void EffectManager::Register(const char* name, aether::GraphicsPipeline pipeline, const aether::Material& material)
	{
		EffectData data;
		data.pipeline = std::move(pipeline);
		data.material = material;
		m_effects[name] = std::move(data);
	}

	bool EffectManager::CreateAndRegister(
	        const char* name, aether::AssetManager& assets, const void* descriptorHeapMappings, aether::gpu::Format colorFormat, aether::gpu::Format depthFormat, const char* shaderVfsPath, const aether::Material& material)
	{
		auto result = assets.CreateGraphicsPipeline({
		        .shaderVfsPath = shaderVfsPath,
		        .colorFormat = colorFormat,
		        .depthFormat = depthFormat,
		        .depthTestEnable = true,
		        .depthWriteEnable = true,
		        .descriptorHeapMappings = descriptorHeapMappings,
		});

		if (!result)
		{
			return false;
		}

		// Make a copy we can register; the caller can keep theirs for further use.
		aether::Material matCopy = material;
		assets.RegisterMaterial(matCopy);

		Register(name, std::move(result.value()), matCopy);
		return true;
	}

	const EffectData* EffectManager::Find(const char* name) const
	{
		auto it = m_effects.find(name ? name : "");
		return it != m_effects.end() ? &it->second : nullptr;
	}

	void EffectManager::DestroyAll(aether::AssetManager& assets)
	{
		for (auto& [name, data]: m_effects)
		{
			assets.UnregisterMaterial(data.material);
			data.pipeline.Destroy();
		}
		m_effects.clear();
	}
} // namespace aether::app::effects
