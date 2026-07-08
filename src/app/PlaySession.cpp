#include "PlaySession.hpp"

#include <string>

#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "debug/SceneSelection.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app
{
	bool StartPlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		auto* assets = context.TryGet<AssetManager>();
		if (playState == nullptr || assets == nullptr || playState->IsPlaying())
		{
			return false;
		}

		if (auto* scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
		{
			std::string buildError;
			if (!scripting->RebuildFromSource(buildError))
			{
				scripting->ReportScriptError("Script rebuild failed:\n" + buildError);
				return false;
			}
			if (scripting->IsAvailable())
			{
				scripting->ClearErrors();
				scripting->LoadScripts();
			}
		}

		World& world = context.Get<World>();
		playState->stopSnapshot = scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
		playState->SetMode(PlayState::Mode::Playing);
		return true;
	}

	bool StopPlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsPlaying())
		{
			return false;
		}

		World& world = context.Get<World>();
		playState->SetMode(PlayState::Mode::Editing);

		if (auto* scriptSystem = context.TryGet<ScriptComponentSystem>())
		{
			scriptSystem->Invalidate(world);
		}

		if (playState->stopSnapshot)
		{
			scene::RestoreSceneInPlace(*playState->stopSnapshot, world, scene::MakeApplySceneDeps(context.services));
			playState->stopSnapshot.reset();
		}

		if (auto* selection = context.TryGet<SceneSelection>())
		{
			selection->Prune(world);
		}
		return true;
	}

	bool TogglePlaySession(LayerContext& context)
	{
		const auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr)
		{
			return false;
		}
		return playState->IsPlaying() ? StopPlaySession(context) : StartPlaySession(context);
	}
} // namespace aether::app
