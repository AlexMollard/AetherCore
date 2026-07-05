#pragma once

#include <memory>
#include <string>

#include "AppLayer.hpp"
#include "material/EffectManager.hpp"
#include "scripting/SceneContext.hpp"

namespace aether::app::scripting
{
	class CSharpScriptingSubsystem;
}

namespace aether::app
{
	// Scene-bootstrap layer.
	//
	// World content comes from the startup scene file (engine.toml
	// app.startupScene) and behavior from entity scripts (ScriptComponent, run by
	// ScriptComponentSystem through C#). This layer owns the per-scene
	// SceneContext, registers effects, boots the startup scene, and services F5
	// hot-reload (reload the managed assembly + re-apply the scene).
	//
	// Lifecycle:
	//   OnAttach  -> build pipeline cache, register effects, publish SceneContext,
	//                load the startup scene
	//   OnUpdate  -> handle hot-reload if requested
	//   OnDetach  -> destroy scene entities, unpublish SceneContext
	class ScriptedSceneLayer final : public AppLayer
	{
	public:
		ScriptedSceneLayer() = default;

		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;

	private:
		void DestroySceneEntities(LayerContext& context);
		void DoReload(LayerContext& context);
		// Startup-scene boot: additive load of settings->app.startupScene, or
		// auto-generate it from the current world on first run. Called from
		// OnAttach AND after every F5 reload (DoReload destroys the loaded scene
		// entities, so the file must re-apply or the world comes back empty).
		void LoadStartupScene(LayerContext& context);

		scripting::SceneContext m_sceneCtx;
		aether::effects::EffectManager m_effectManager;
		scripting::CSharpScriptingSubsystem* m_csharp = nullptr;
	};
} // namespace aether::app
