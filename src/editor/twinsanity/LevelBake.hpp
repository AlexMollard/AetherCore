#pragma once

#include <string>

#include "utils/ServiceContainer.hpp"

namespace aether::editor::twinsanity
{
	// What one bake run did; the panel and the control method surface this verbatim.
	struct LevelBakeReport
	{
		bool ok = false;
		std::string error;
		std::string prefab;            // the prefab's save name (its file lives in the project's gitignored assets/prefabs)
		int prefabEntities = 0;        // entities captured into the prefab file
		int overridesKept = 0;         // instance overrides carried across a re-bake
		int overridesDropped = 0;      // overrides whose entity no longer exists (level content changed)
		bool replacedInstance = false; // the scene already had an instance; its record was carried over
	};

	// Builds the beach level into the live scene via the project's TwinsanityBake C# command
	// (a public static Run() - the same builder TwinsanityLevel runs at play), saves the built
	// subtree as the beach prefab, and re-expands the scene's linked instance. A re-bake keeps
	// the instance's overrides (a moved crate stays moved): they route by the prefab's stable
	// entity guids, which survive because the bake is deterministic - and an override whose
	// entity vanished in the new bake is dropped with a warning rather than silently.
	[[nodiscard]] LevelBakeReport BakeBeachLevel(ServiceContainer& services);
} // namespace aether::editor::twinsanity
