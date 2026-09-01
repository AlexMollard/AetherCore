#pragma once

#include <cstdint>
#include <string>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class PixelArtDocument;

	// Pixel-art canvas editor: pencil/eraser/fill/line/rect/ellipse/eyedropper,
	// a colour palette + picker, undo/redo, PNG new/open/save, zoom + grid. Edits
	// a PixelArtDocument shared (as a service) with the control server, so canvases
	// can also be authored over MCP.
	class PixelArtPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Pixel Art";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		// A canvas waiting to be opened over unsaved artwork. The document has always known
		// it was dirty; the Open button simply never asked.
		std::string m_pendingOpenPath;
		void DrawUnsavedCanvasPrompt(app::LayerContext& context, PixelArtDocument& doc);

		enum class Tool
		{
			Pencil,
			Eraser,
			Fill,
			Line,
			Rectangle,
			Ellipse,
			Eyedropper,
		};

		void DrawToolbar(app::LayerContext& context, PixelArtDocument& doc);
		void DrawPalette(PixelArtDocument& doc);
		void DrawCanvas(PixelArtDocument& doc);

		Tool m_tool = Tool::Pencil;
		int m_zoom = 12; // display pixels per texel
		bool m_showGrid = true;
		bool m_filledShape = false;

		// In-progress shape (line/rect/ellipse): committed on mouse release.
		bool m_shaping = false;
		int m_shapeX0 = 0;
		int m_shapeY0 = 0;
		int m_shapeX1 = 0;
		int m_shapeY1 = 0;
		// Pencil/eraser drag continuity.
		bool m_painting = false;
		int m_lastX = 0;
		int m_lastY = 0;

		std::string m_savePath = "project://assets/textures/new_sprite.png";
		std::string m_status;
		int m_newW = 32;
		int m_newH = 32;
	};
} // namespace aether::editor
