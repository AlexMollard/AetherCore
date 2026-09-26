#pragma once

#include <string>

#include "debug/DebugPanel.hpp"

namespace aether::editor::twinsanity
{
	// Twinsanity flavor panel: the beach level bake button. Runs the same code as the
	// twinsanity.bake_level control method (LevelBake.cpp) and reports what it did - the
	// prefab size, the instance overrides kept or dropped, and any warnings. Registered
	// only for the twinsanity flavor.
	class LevelBakePanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Level Bake";
		}

		// The flavor's second half: the level is a button press away, not a findable panel.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		std::string m_report = "The bake builds the whole beach from the extracted JSON under one root,\n"
		                       "saves it as the 'beach' prefab, and keeps the scene's instance as a\n"
		                       "reference with your edits as overrides.\n"
		                       "Open the Beach scene first.";
		bool m_running = false; // one at a time; the bake is synchronous and seconds-long
	};
} // namespace aether::editor::twinsanity
