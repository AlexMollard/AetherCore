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
		// entities, so the file must re-apply or the world comes back empty).
		void LoadStartupScene(LayerContext& context);

		scripting::SceneContext m_sceneCtx;
		aether::effects::EffectManager m_effectManager;
		scripting::CSharpScriptingSubsystem* m_csharp = nullptr;
	};
} // namespace aether::app
