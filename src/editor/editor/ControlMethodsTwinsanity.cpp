// Twinsanity flavor control methods: the level convert (the one-time prefab/area-scene migration).
// It lives in twinsanity/ so the editor panel and this control method share it.

#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include "PlayState.hpp"
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
	}
} // namespace aether::editor
