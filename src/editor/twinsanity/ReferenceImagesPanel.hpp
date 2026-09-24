#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "material/Texture.hpp"

namespace aether::editor::twinsanity
{
	// Twinsanity flavor panel: a viewer for the PNGs a decompilation session piles up in
	// the project's reference/ folder (PCSX2 captures, sheet scans, timing charts). Lists
	// everything in project://reference/ and shows the selected image, zoomable, beside
	// the editor's own capture_texture output. Registered only for the twinsanity flavor.
	class ReferenceImagesPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Reference Images";
		}

		// The whole point of the flavor is this panel; hiding it behind the Window menu
		// on a fresh open would make the workbench look like nothing happened.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnDetach(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	private:
		struct Entry
		{
			std::filesystem::path path;
			std::string name;
		};

		void Scan(const std::filesystem::path& referenceDir);
		// Decode + upload the selected image (idempotent: re-selecting the current image
		// keeps the uploaded texture).
		bool LoadPreview(app::LayerContext& context, int index);
		void ReleasePreview(app::LayerContext& context);

		std::vector<Entry> m_entries;
		// Root whose reference/ folder m_entries reflects, so a project switch rescans.
		std::filesystem::path m_scannedRoot;
		std::filesystem::file_time_type m_referenceStamp{};
		bool m_stampValid = false;
		int m_selected = -1;
		Texture m_previewTexture;
		std::uint64_t m_previewImguiId = 0;
		int m_previewIndex = -1;
		int m_previewWidth = 0;
		int m_previewHeight = 0;
		std::string m_error;
		// Fit-to-pane by default; the slider multiplies the fitted size (or the native
		// size once zoomed past 1x).
		bool m_fit = true;
		float m_zoom = 1.0f;
	};
} // namespace aether::editor::twinsanity
