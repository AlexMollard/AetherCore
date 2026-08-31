#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "debug/MaterialAssetEditor.hpp"
#include "debug/DebugPanel.hpp"
#include "materialgraph/MaterialGraph.hpp"

namespace aether::editor
{
	// The one window for authoring a material, whether that material is a set of values or a
	// node graph. Selecting a .material.toml gives the property editor; selecting a
	// .materialgraph.toml gives the node canvas. Both share the same lit preview, because the
	// question being asked of either is the same one: what does this look like?
	class MaterialGraphPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Material";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void FollowSelection(app::LayerContext& context);
		void DrawGraphMode(app::LayerContext& context);
		void DrawCanvas();
		void SyncLinks();
		void DeleteSelection();
		bool Compile(app::LayerContext& context);
		void RefreshPreview(app::LayerContext& context, const std::string& materialPath);
		void DrawPreview(float side) const;

		// Graph mode.
		MaterialGraph m_graph = MakeDefaultMaterialGraph();
		std::string m_graphPath;
		std::string m_compiledSignature; // the graph as last compiled, to detect a real change
		std::string m_status;
		bool m_statusIsError = false;
		bool m_positionsApplied = false;
		// Where the add menu was opened, so a new node lands under the cursor.
		float m_addNodeScreenX = 0.0f;
		float m_addNodeScreenY = 0.0f;
		bool m_autoCompile = true;
		float m_idleSeconds = 0.0f;

		// Material mode.
		MaterialAssetEditState m_edit;

		// Shared preview.
		std::uint64_t m_previewImGuiId = 0;
		std::string m_previewError;
		std::string m_previewMaterialPath;
		MaterialPresetSpec m_previewSpec;
		bool m_previewDirty = false;
	};
} // namespace aether::editor
