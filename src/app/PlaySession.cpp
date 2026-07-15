#include "PlaySession.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "PlayState.hpp"
#include "animation/SpriteAnimationSystem.hpp"
#include "assets/AssetManager.hpp"
#include "debug/SceneSelection.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app
{
	namespace
	{
		// thread once the async script build has succeeded and scripts are loaded,
		void EnterPlayingMode(LayerContext& context, PlayState& playState, AssetManager& assets)
		{
			World& world = context.Get<World>();
			playState.stopSnapshot = scene::CaptureScene(world, assets.GetMaterialRegistry(), assets.GetTextureRegistry(), context.TryGet<Renderer>());
			if (const auto* selection = context.TryGet<aether::editor::SceneSelection>())
			{
				playState.stopSelection = selection->All();
				playState.stopSelectionPrimary = selection->Primary();
			}
			else
			{
				playState.stopSelection.clear();
				playState.stopSelectionPrimary = {};
			}
			if (auto* spriteAnimations = context.TryGet<SpriteAnimationSystem>())
			{
				spriteAnimations->ResetForPlay(world);
			}
			playState.SetMode(PlayState::Mode::Playing);
		}
	} // namespace

	bool StartPlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		auto* assets = context.TryGet<AssetManager>();
		if (playState == nullptr || assets == nullptr || playState->IsPlaying() || playState->IsCompiling())
		{
			return false;
		}

		if (auto* scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
		{
			// Kick the rebuild off on a worker thread and enter Compiling. The editor
			scripting->BeginRebuildFromSource();
			playState->SetMode(PlayState::Mode::Compiling);
			return true;
		}

		EnterPlayingMode(context, *playState, *assets);
		return true;
	}

	void UpdatePlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsCompiling())
		{
			return;
		}

		auto* scripting = context.TryGet<scripting::CSharpScriptingSubsystem>();
		auto* assets = context.TryGet<AssetManager>();
		if (scripting == nullptr || assets == nullptr)
		{
			playState->SetMode(PlayState::Mode::Editing);
			return;
		}

		using BuildStatus = scripting::CSharpScriptingSubsystem::BuildStatus;
		std::string buildError;
		switch (scripting->PollRebuildStatus(buildError))
		{
			case BuildStatus::Running:
				return;

			case BuildStatus::Idle:
				playState->SetMode(PlayState::Mode::Editing);
				return;

			case BuildStatus::Failed:
				scripting->ReportScriptError("Script rebuild failed:\n" + buildError);
				scripting->ClearRebuild();
				playState->SetMode(PlayState::Mode::Editing);
				return;

			case BuildStatus::Succeeded:
				scripting->ClearRebuild();
				if (scripting->IsAvailable())
				{
					scripting->ClearErrors();
					scripting->LoadScripts();
				}
				EnterPlayingMode(context, *playState, *assets);
				return;
		}
	}

	bool StopPlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr)
		{
			return false;
		}

		if (playState->IsCompiling())
		{
			if (auto* scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
			{
				scripting->ClearRebuild();
			}
			playState->SetMode(PlayState::Mode::Editing);
			return true;
		}

		if (!playState->IsPlaying())
		{
			return false;
		}

		World& world = context.Get<World>();
		auto* selection = context.TryGet<aether::editor::SceneSelection>();
		std::vector<Entity> selectionToRestore = playState->stopSelection;
		Entity primaryToRestore = playState->stopSelectionPrimary;
		if (selection != nullptr && !selection->All().empty())
		{
			selectionToRestore = selection->All();
			primaryToRestore = selection->Primary();
		}

		playState->SetMode(PlayState::Mode::Editing);

		if (auto* scriptSystem = context.TryGet<ScriptComponentSystem>())
		{
			scriptSystem->Invalidate(world);
		}

		if (playState->stopSnapshot)
		{
			const auto& snapshot = *playState->stopSnapshot;
			const std::vector<Entity> restored = scene::RestoreSceneInPlace(snapshot, world, scene::MakeApplySceneDeps(context.services));
			const auto remapRestored = [&snapshot, &restored](Entity e)
			{
				for (std::size_t i = 0; i < snapshot.entities.size() && i < restored.size(); ++i)
				{
					if (snapshot.entities[i].entityId == e.id)
					{
						return restored[i];
					}
				}
				return e;
			};
			for (Entity& e: selectionToRestore)
			{
				e = remapRestored(e);
			}
			primaryToRestore = remapRestored(primaryToRestore);
			playState->stopSnapshot.reset();
		}
		playState->stopSelection.clear();
		playState->stopSelectionPrimary = {};

		if (selection != nullptr)
		{
			selection->Replace(std::move(selectionToRestore), primaryToRestore);
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
		return (playState->IsPlaying() || playState->IsCompiling()) ? StopPlaySession(context) : StartPlaySession(context);
	}
} // namespace aether::app
