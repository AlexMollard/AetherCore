#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "debug/MaterialAssetEditor.hpp"
#include "debug/DebugPanel.hpp"
#include "materialgraph/MaterialGraph.hpp"
#include "mesh/PrimitiveMeshes.hpp"

namespace aether::editor
{
	// The one window for authoring a material.
	//
	// A material is ONE file. Its properties, its textures and - when it has one - the node
	// graph that generates its shader all live in the same .material.toml and are all edited
	// here at once: the graph reads the material's texture slots, so an editor that showed
	// one or the other meant a graph whose textures could not be assigned.
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
		void Open(app::LayerContext& context, const std::string& materialPath);
		void DrawToolbar(app::LayerContext& context);
		void DrawSidebar(app::LayerContext& context, float width);
		void DrawCanvas();
		void DrawAddNodeMenu();
		void SyncLinks();
		void DeleteSelection();
		bool Compile(app::LayerContext& context);
		bool WriteMaterial(app::LayerContext& context);
		void RefreshPreview(app::LayerContext& context);
		void DrawPreviewControls(app::LayerContext& context, float width);
		void DrawPreview(float side) const;
		[[nodiscard]] std::string GraphSignature() const;

		// The material being edited, and its properties/textures.
		std::string m_path;
		MaterialAssetEditState m_edit;

		// Present only when this material's shader comes from a graph.
		std::optional<MaterialGraph> m_graph;
		std::string m_compiledSignature; // the graph as last compiled, to detect a real change
		std::string m_status;
		bool m_statusIsError = false;
		bool m_positionsApplied = false;
		bool m_autoCompile = true;
		float m_idleSeconds = 0.0f;

		// The add menu is opened from inside the canvas child but SUBMITTED at the window
		// level: a popup opened in one window and begun in another never appears, which is
		// why right-click did nothing at all.
		bool m_addMenuRequested = false;
		float m_addNodeScreenX = 0.0f;
		float m_addNodeScreenY = 0.0f;
		// Where a node dropped from the toolbar button lands, since there is no cursor to
		// take a position from.
		float m_canvasOriginX = 0.0f;
		float m_canvasOriginY = 0.0f;
		// The add menu filters as you type, because a categorised list of two dozen nodes is
		// still slower than knowing the name.
		char m_addFilter[64] = {};
		bool m_addFilterFocus = false;

		// Preview.
		std::uint64_t m_previewImGuiId = 0;
		std::string m_previewError;
		MaterialPresetSpec m_previewSpec;
		bool m_previewValid = false;
		PrimitiveMesh m_previewMesh = PrimitiveMesh::Sphere;
	};
} // namespace aether::editor
