#include "PlaySession.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "PlayState.hpp"
#include "animation/SpriteAnimationSystem.hpp"
#include "assets/AssetManager.hpp"
#include "assets/TileAssetStore.hpp"
#include "debug/SceneSelection.hpp"
#include "net/NetworkContext.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "platform/Input.hpp"
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
			if (const auto* scenes = context.TryGet<SceneSubsystem>())
			{
				playState.stopSceneName = scenes->GetCurrentScene();
			}
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
			// Snapshot tile cells too - they live in shared assets, not the ECS, so a
			// script painting during play would otherwise persist past Stop.
			if (auto* tiles = context.TryGet<TileAssetStore>())
			{
				playState.tileStopSnapshot = tiles->SnapshotTileMaps();
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

		// Recompile project shaders (editor-provided hook) so shader edits are picked up on Play,
		// the same way the C# scripts are rebuilt below. Refreshing the overlay bumps the shader
		// generation, which makes the renderer rebuild effect pipelines from the fresh .spv.
		if (auto* shaderHook = context.TryGet<ProjectShaderRecompileHook>(); shaderHook != nullptr && shaderHook->recompile)
		{
			shaderHook->recompile();
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

		// Drop any synthetic keys/mouse a headless playtest was holding so they don't
		// leak into edit mode.
		if (auto* input = context.TryGet<Input>())
		{
			input->ClearSyntheticKeys();
			input->ClearSyntheticMouse();
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

		// A NET SESSION IS RUNTIME STATE AND MUST NOT OUTLIVE PLAY, exactly like the
		// script instances torn down above and the scene restored below. Left running,
		// the next Play in this process is silently still a client: Net.IsClient and
		// Net.IsConnected are both still true, so a game takes its client branch and
		// spawns nothing, and an editor that has ever joined a host can never go back to
		// single-player without a restart.
		//
		// Through Stop(), never by dropping the socket. Stop is what gives back the 2D
		// bodies this peer took off local simulation (a player character left kinematic
		// never falls again), destroys the entities the session spawned rather than
		// leaving them to be duplicated by the next join, and tells the other peers the
		// session ended on purpose. A bare Disconnect would leave every one of those
		// behind.
		//
		// Placed after Invalidate so no live script instance is holding an entity Stop
		// is about to destroy, and before the scene restore so the restore sees a world
		// with no session leftovers in it. Generic: nothing here knows what game is
		// playing, only that its session ends when play does.
		if (auto* network = context.TryGet<aether::net::NetworkContext>())
		{
			network->Stop(world);
			// And the verdict on the link that just ended goes with it. Stop deliberately
			// keeps the reason - a game reads it after the session is already gone - but
			// it describes a link in a world that no longer exists, and a game asking
			// "why did my session end?" on the NEXT Play must not be answered by the
			// last one.
			network->SetDisconnectReason({});
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
			// A script may have switched scenes mid-play; the snapshot restore
			// brings back the edited scene, so the name must follow it.
			if (auto* scenes = context.TryGet<SceneSubsystem>())
			{
				scenes->SetCurrentScene(playState->stopSceneName);
			}
		}
		// Revert any tile edits a script made during play (mirrors the ECS restore).
		// Wholesale swap: play-time paints undo, maps loaded during play drop out.
		if (auto* tiles = context.TryGet<TileAssetStore>())
		{
			tiles->RestoreTileMaps(std::move(playState->tileStopSnapshot));
		}
		playState->tileStopSnapshot.clear();

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

	bool PausePlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsPlaying())
		{
			return false;
		}
		playState->SetPaused(true);
		return true;
	}

	bool ResumePlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsPlaying())
		{
			return false;
		}
		playState->SetPaused(false);
		return true;
	}

	bool TogglePausePlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsPlaying())
		{
			return false;
		}
		playState->SetPaused(!playState->IsPaused());
		return true;
	}

	bool StepPlaySession(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr || !playState->IsPlaying())
		{
			return false;
		}
		// A step only makes sense against a frozen sim, so pause first if needed.
		if (!playState->IsPaused())
		{
			playState->SetPaused(true);
		}
		playState->RequestStep();
		return true;
	}
} // namespace aether::app
