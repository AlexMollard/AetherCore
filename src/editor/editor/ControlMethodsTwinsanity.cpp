// Twinsanity flavor control methods: the beach level bake. The bake itself lives in
// twinsanity/LevelBake.cpp so the editor panel button and this control method share it.

#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "twinsanity/LevelBake.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;

	void AppendTwinsanityMethods(std::vector<ControlMethod>& methods)
	{
		methods.push_back({"twinsanity.bake_level",
		        "twinsanity_bake_level",
		        "Build the N. Sanity Beach level into the live scene with the project's TwinsanityBake command and save it as the 'beach' prefab (in the project's gitignored assets/prefabs), then expand the scene's linked prefab instance. A re-bake keeps the instance's overrides (moved/edited entities stay edited); overrides whose baked entity no longer exists are dropped with a warning. Run with the Beach scene open, not during Play.",
		        true,
		        Obj({{"saveScene", json{{"type", "boolean"}, {"description", "Also save the scene under its current name after the bake (default false)."}}}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* playState = ctx.services.TryGet<app::PlayState>();
			        if (playState != nullptr && (playState->IsPlaying() || playState->IsCompiling()))
			        {
				        return json{{"error", "cannot bake while a play session is running"}};
			        }
			        const twinsanity::LevelBakeReport report = twinsanity::BakeBeachLevel(ctx.services);
			        if (!report.ok)
			        {
				        return json{{"error", report.error}};
			        }
			        json result{{"prefab", report.prefab},
				        {"prefabEntities", report.prefabEntities},
				        {"overridesKept", report.overridesKept},
				        {"overridesDropped", report.overridesDropped},
				        {"replacedInstance", report.replacedInstance}};
			        if (p.value("saveScene", false))
			        {
				        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
				        auto* assets = ctx.services.TryGet<AssetManager>();
				        if (scenes != nullptr && assets != nullptr)
				        {
					        const std::string scene = scenes->GetCurrentScene();
					        result["sceneSaved"] = app::scene::QuickSave(ctx.services.Get<World>(), scene, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), ctx.services.TryGet<Renderer>());
					        result["scene"] = scene;
				        }
			        }
			        return result;
		        }});
	}
} // namespace aether::editor
