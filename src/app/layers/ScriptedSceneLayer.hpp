#pragma once

#include <memory>
#include <string>

#include "AppLayer.hpp"
#include "effects/EffectManager.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/ScriptHandle.hpp"
#include "scripting/SystemFactory.hpp"

namespace aether::app::scripting
{
	class ScriptingSubsystem;
}

namespace aether::app
{
	// Generic scene layer that drives a .das script.
	//
	// Lifecycle:
	//   OnAttach  -> compile script, create default pipeline, call on_attach()
	//   OnUpdate  -> call on_update(); handle hot-reload if requested
	//   OnDetach  -> call on_detach(), destroy scene entities, unload systems
	class ScriptedSceneLayer final : public AppLayer
	{
	public:
		explicit ScriptedSceneLayer(std::string scriptPath, SystemFactory systemFactory);

		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;

	private:
		void BuildDefaultPipeline(LayerContext& context);
		void DestroySceneEntities(LayerContext& context);
		void DoReload(LayerContext& context);

		std::string m_scriptPath;
		SystemFactory m_systemFactory;
		scripting::SceneContext m_sceneCtx;
		scripting::ScriptHandle m_handle;
		aether::GraphicsPipeline m_defaultPipeline;
		aether::app::effects::EffectManager m_effectManager;
		scripting::ScriptingSubsystem* m_scripting = nullptr;
		bool m_scriptBroken = false;
	};
} // namespace aether::app
