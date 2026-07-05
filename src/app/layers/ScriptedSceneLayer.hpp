#pragma once

#include <memory>
#include <string>

#include "AppLayer.hpp"
#include "material/EffectManager.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/ScriptHandle.hpp"

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
	//   OnDetach  -> call on_detach(), destroy scene entities
	class ScriptedSceneLayer final : public AppLayer
	{
	public:
		explicit ScriptedSceneLayer(std::string scriptPath);

		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;

	private:
		void DestroySceneEntities(LayerContext& context);
		void DoReload(LayerContext& context);
		// Startup-scene boot: additive load of settings->app.startupScene, or
		// auto-generate it from legacy script content on first run. Called from
		// OnAttach AND after every F5 reload (DoReload destroys the loaded scene
		// entities along with the script's, so the file must re-apply or the
		// world comes back missing everything the script no longer builds).
		void LoadStartupScene(LayerContext& context);

		std::string m_scriptPath;
		scripting::SceneContext m_sceneCtx;
		scripting::ScriptHandle m_handle;
		aether::effects::EffectManager m_effectManager;
		scripting::ScriptingSubsystem* m_scripting = nullptr;
		bool m_scriptBroken = false;
	};
} // namespace aether::app
