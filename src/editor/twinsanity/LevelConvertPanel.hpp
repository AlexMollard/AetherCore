#pragma once

#include <string>

#include "debug/DebugPanel.hpp"

namespace aether::editor::twinsanity
{
	// Twinsanity flavor panel: the one-time level convert (LevelConvert.cpp, also the
	// twinsanity.convert control method). "Report" lists what the current extract would change
	// against the saved prefabs and area scenes; "Write" writes only what does not exist yet, and
	// "Overwrite all" replaces everything the conversion owns. Registered only for the flavor.
	class LevelConvertPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Level Convert";
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		std::string m_report = "Converts the extracted hub into tw_* prefabs, one scene per area\n"
		                       "(HubBeach, HubA, HubB, ...) and the world scene Beach that includes them.\n"
		                       "A one-time migration: after it, edit the scenes; Report shows what a\n"
		                       "re-extract changed. Save your scene first - this replaces the live world.";
	};
} // namespace aether::editor::twinsanity
