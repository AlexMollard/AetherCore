#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "utils/ServiceContainer.hpp"

namespace aether::editor::twinsanity
{
	// twinsanity.convert's input (DESIGN.md §2.8): `report` only diffs, `write` writes; `areas`
	// limits the run to those scene names (the world scene "Beach" included), empty = all;
	// `overwrite` names the existing tw_* prefabs and scenes a write may replace ("*" = all).
	struct LevelConvertRequest
	{
		bool report = false;
		std::vector<std::string> areas;
		std::vector<std::string> overwrite;
	};

	// The one-time Twinsanity migration: runs the project's TwinsanityConvert C# command in an
	// emptied world (prefab templates, per-area scenery and collision, the manifest), then writes
	// the tw_* prefabs, one scene per hub area (linked prefab instances with their disc data as
	// root script overrides, Point/Path children, Link/Target entity references) and the world
	// scene that includes them. `report` writes nothing but logs/prefabs/drift-<scene>.md: what the
	// current extract would change against the saved prefabs and scenes. Either way the editor
	// ends on the world scene, reloaded from disk; a scene with unsaved edits is refused.
	// Returns {ok:false, error} or the run's summary (counts, written/kept files, drift).
	[[nodiscard]] nlohmann::json ConvertLevel(ServiceContainer& services, const LevelConvertRequest& request);
} // namespace aether::editor::twinsanity
