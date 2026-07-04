#pragma once

#include <string>
#include <unordered_map>

#include "material/MaterialAsset.hpp"
#include "rendering/GraphicsPipeline.hpp"

namespace aether
{
	class AssetManager;
} // namespace aether

namespace aether::app::effects
{
	// Per-effect data: pipeline + default material authoring data. A registry
	// handle is acquired per entity when the effect is applied.
	struct EffectData
	{
		aether::GraphicsPipeline pipeline;
		aether::MaterialAsset material{};
	};

	// Per-entity authoring state for an applied effect. The effect parameter
	// setters mutate this asset and re-acquire a registry handle (materials are
	// immutable; a changed asset is a different material).
	struct EffectMaterialComponent
	{
		aether::MaterialAsset asset{};
	};

	// Application-level service owning all runtime shader effects.
	// Effects are registered once at startup and looked up by name (no strcmp).
	// Each effect owns a dedicated GraphicsPipeline and default material data.
	class EffectManager
	{
	public:
		EffectManager() = default;
		~EffectManager() = default;

		EffectManager(const EffectManager&) = delete;
		EffectManager& operator=(const EffectManager&) = delete;

		// Register a named effect. Takes ownership of the pipeline and material.
		void Register(const char* name, aether::GraphicsPipeline pipeline, const aether::MaterialAsset& material);

		// Convenience: build a GraphicsPipeline from a shader path and register
		// it with a default material in one call.  Returns true on success.
		// The pipeline inherits depthTestEnable=true, depthWriteEnable=true.
		bool CreateAndRegister(const char* name, aether::AssetManager& assets, const void* descriptorHeapMappings, aether::gpu::Format colorFormat, aether::gpu::Format depthFormat, const char* shaderVfsPath, const aether::MaterialAsset& material);

		// Lookup. Returns nullptr if name not found.
		[[nodiscard]] const EffectData* Find(const char* name) const;

		// Unregister all effects and destroy their pipelines. Entity material
		// handles are released by the ECS lifecycle hook, not here.
		void DestroyAll();

		// Number of registered effects.
		[[nodiscard]] std::size_t Count() const
		{
			return m_effects.size();
		}

	private:
		std::unordered_map<std::string, EffectData> m_effects;
	};
} // namespace aether::app::effects
