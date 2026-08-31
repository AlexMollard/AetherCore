#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"
#include "debug/MaterialAssetEditor.hpp"

namespace aether::editor
{
	// Authoring a material without having to select an object first, with a live preview of
	// what the material actually looks like lit.
	//
	// The Inspector edits a material when you happen to have one selected; this keeps one open
	// while you work, which is what makes it usable for actually dialling a look in.
	class MaterialEditorPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Material Editor";
		}

		// Opened deliberately rather than always present - most sessions never author a
		// material, and a permanently docked preview costs a render pass every frame.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void RefreshPreview(app::LayerContext& context);

		MaterialAssetEditState m_edit;
		std::uint64_t m_previewImGuiId = 0;
		std::string m_previewError;
		bool m_previewDirty = false;
	};
} // namespace aether::editor
