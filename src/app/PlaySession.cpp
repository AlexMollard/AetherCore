#include "PlaySession.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
		if (const auto* selection = context.TryGet<SceneSelection>())
		{
			playState->stopSelection = selection->All();
			playState->stopSelectionPrimary = selection->Primary();
		}
		else
		{
			playState->stopSelection.clear();
			playState->stopSelectionPrimary = {};
		}
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
		auto* selection = context.TryGet<SceneSelection>();
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
		return playState->IsPlaying() ? StopPlaySession(context) : StartPlaySession(context);
	}
} // namespace aether::app
