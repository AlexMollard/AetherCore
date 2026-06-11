#pragma once

#include <string>
#include <unordered_map>

#include "gpu/DescriptorSetLayout.hpp"
#include "material/Material.hpp"
#include "rendering/GraphicsPipeline.hpp"

namespace aether
{
	class AssetManager;
} // namespace aether

namespace aether::app::effects
{
	// Per-effect data: pipeline + a registered default material.
	struct EffectData
	{
		aether::GraphicsPipeline pipeline;
		aether::Material material{};
	};

	// Application-level service owning all runtime shader effects.
	// Effects are registered once at startup and looked up by name (no strcmp).
	// Each effect owns a dedicated GraphicsPipeline and a default Material
	// registered with the GPU material buffer.
	class EffectManager
	{
	public:
		EffectManager() = default;
		~EffectManager() = default;

		EffectManager(const EffectManager&) = delete;
		EffectManager& operator=(const EffectManager&) = delete;

		// Register a named effect. Takes ownership of the pipeline and material.
		void Register(const char* name, aether::GraphicsPipeline pipeline, const aether::Material& material);

		// Convenience: build a GraphicsPipeline from a shader path and register
		// it with a default material in one call.  Returns true on success.
		// The pipeline inherits depthTestEnable=true, depthWriteEnable=true.
		bool CreateAndRegister(const char* name,
		        aether::AssetManager& assets,
		        aether::gpu::DescriptorSetLayout bindlessLayout,
		        aether::gpu::DescriptorSetLayout lightingLayout,
		        VkFormat colorFormat,
		        VkFormat depthFormat,
		        const char* shaderVfsPath,
		        const aether::Material& material);

		// Lookup. Returns nullptr if name not found.
		[[nodiscard]] const EffectData* Find(const char* name) const;

		// Unregister all effects and free GPU material slots.
		void DestroyAll(aether::AssetManager& assets);

		// Number of registered effects.
		[[nodiscard]] std::size_t Count() const
		{
			return m_effects.size();
		}

	private:
		std::unordered_map<std::string, EffectData> m_effects;
	};
} // namespace aether::app::effects
