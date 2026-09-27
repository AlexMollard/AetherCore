// Twinsanity flavor control methods: the level convert (the one-time prefab/area-scene migration)
// and the retired beach level bake. Both live in twinsanity/ so the editor panels and these
// control methods share them.

#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "twinsanity/LevelBake.hpp"
#include "twinsanity/LevelConvert.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;

	void AppendTwinsanityMethods(std::vector<ControlMethod>& methods)
	{
		methods.push_back({"twinsanity.convert",
		        "twinsanity_convert",
		        "The one-time Twinsanity migration (docs/twinsanity-editor.md, 'Level convert'): run the project's TwinsanityConvert command in an emptied world and write the tw_* prefabs, one scene per hub area (HubBeach, HubA, HubB, ...) and the world scene 'Beach' that includes them. mode 'write' refuses to replace an existing tw_* prefab or scene unless it is listed in 'overwrite' ('*' = all); mode 'report' writes nothing but project://.aether/convert/drift-<scene>.md, listing what the current extract would change. Refused during Play or with unsaved edits; the editor ends on the scene that was open, reloaded from disk.",
		        true,
		        Obj({{"mode", json{{"type", "string"}, {"enum", json::array({"write", "report"})}, {"description", "'write' (default) or 'report'."}}},
		                {"areas", json{{"type", "array"}, {"items", json{{"type", "string"}}}, {"description", "Scene names to convert or report (e.g. HubBeach, Beach); empty = all."}}},
		                {"overwrite", json{{"type", "array"}, {"items", json{{"type", "string"}}}, {"description", "Existing prefab/scene names a write may replace; '*' = all."}}}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* playState = ctx.services.TryGet<app::PlayState>();
			        if (playState != nullptr && (playState->IsPlaying() || playState->IsCompiling()))
			        {
				        return json{{"error", "cannot convert while a play session is running"}};
			        }
			        twinsanity::LevelConvertRequest request;
			        request.report = p.value("mode", std::string{"write"}) == "report";
			        request.areas = p.value("areas", std::vector<std::string>{});
			        request.overwrite = p.value("overwrite", std::vector<std::string>{});
			        json result = twinsanity::ConvertLevel(ctx.services, request);
			        if (!result.value("ok", false))
			        {
				        return json{{"error", result.value("error", std::string{"conversion failed"})}};
			        }
			        return result;
		        }});
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
