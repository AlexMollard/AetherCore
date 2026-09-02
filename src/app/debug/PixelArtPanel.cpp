#include "debug/PixelArtPanel.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/PixelArtDocument.hpp"
#include "editor/EditorProjectContext.hpp"
#include "layers/AppLayer.hpp"

namespace aether::editor
{
	namespace
	{
		ImVec4 ToVec4(std::uint32_t rgba)
		{
			return ImVec4(static_cast<float>(rgba & 0xFFu) / 255.0f, static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f, static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f, static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f);
		}

		std::uint32_t FromVec4(const ImVec4& c)
		{
			const auto ch = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return ch(c.w) << 24 | ch(c.z) << 16 | ch(c.y) << 8 | ch(c.x);
		}

	} // namespace

	void PixelArtPanel::DrawUnsavedCanvasPrompt(app::LayerContext& context, PixelArtDocument& doc)
	{
		if (m_pendingOpenPath.empty())
		{
			return;
		}
		ImGui::OpenPopup("Unsaved canvas##pixelOpen");
		if (ImGui::BeginPopupModal("Unsaved canvas##pixelOpen", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("This canvas has unsaved changes.");
			ImGui::TextDisabled("Opening %s will paint over them.", m_pendingOpenPath.c_str());
			ImGui::Spacing();
			const auto openPending = [&]()
			{
				const std::filesystem::path disk = app::ResolveProjectPath(context.TryGet<app::EditorProjectContext>(), m_pendingOpenPath);
				m_status = doc.Load(disk) ? "Opened " + m_pendingOpenPath : "Open failed: " + m_pendingOpenPath;
				m_pendingOpenPath.clear();
			};
			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save and open", ImVec2(150.0f, 0.0f)))
			{
				const std::filesystem::path disk = app::ResolveProjectPath(context.TryGet<app::EditorProjectContext>(), m_savePath);
				if (doc.Save(disk))
				{
					openPending();
				}
				else
				{
					// Failing to write is not a reason to lose the pixels.
					m_status = "Save failed: " + m_savePath;
					m_pendingOpenPath.clear();
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Discard", ImVec2(110.0f, 0.0f)))
			{
				openPending();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Keep editing", ImVec2(130.0f, 0.0f)))
			{
				m_pendingOpenPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void PixelArtPanel::OnImGui(app::LayerContext& context)
	{
		// Fetched before Begin so the title can carry the unsaved marker.
		auto* doc = context.TryGet<PixelArtDocument>();
		ImGui::Begin(editor::DocumentTitle(GetName(), doc != nullptr && doc->Dirty()).c_str(), VisiblePtr());
		m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		if (doc == nullptr)
		{
			ImGui::TextDisabled("Pixel-art document unavailable.");
			ImGui::End();
			return;
		}

		DrawToolbar(context, *doc);
		DrawUnsavedCanvasPrompt(context, *doc);
		ImGui::Separator();

		// Left: colour + palette + file. Right: the canvas.
		if (ImGui::BeginTable("##pixelLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("tools", ImGuiTableColumnFlags_WidthFixed, 210.0f);
			ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			DrawPalette(*doc);

			ImGui::TableSetColumnIndex(1);
			ImGui::BeginChild("##pixelCanvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			DrawCanvas(*doc);
			ImGui::EndChild();

			ImGui::EndTable();
		}

		ImGui::End();
	}

	void PixelArtPanel::DrawToolbar(app::LayerContext& context, PixelArtDocument& doc)
	{
		const auto toolButton = [&](const char* label, Tool tool, const char* tip)
		{
			const bool active = m_tool == tool;
			if (active ? chrome::PrimaryButton(label) : chrome::OutlineButton(label))
			{
				m_tool = tool;
			}
			ImGui::SetItemTooltip("%s", tip);
			ImGui::SameLine();
		};
		toolButton(ICON_FA_PEN, Tool::Pencil, "Pencil");
		toolButton(ICON_FA_XMARK, Tool::Eraser, "Eraser");
		toolButton(ICON_FA_FILL_DRIP, Tool::Fill, "Bucket fill");
		toolButton("/", Tool::Line, "Line");
		toolButton(ICON_FA_SQUARE, Tool::Rectangle, "Rectangle");
		toolButton(ICON_FA_CIRCLE, Tool::Ellipse, "Ellipse");
		toolButton(ICON_FA_EYE_DROPPER, Tool::Eyedropper, "Eyedropper");

		// Each group wraps as a unit rather than splitting across the edge. The row holds
		// seven tool buttons before it even starts, so on a narrow panel - or at a larger UI
		// scale, where every control grows - it does not come close to fitting on one line.
		const ImGuiStyle& style = ImGui::GetStyle();
		chrome::SameLineOrWrap(chrome::CheckboxWidth("Filled") + style.ItemSpacing.x + chrome::CheckboxWidth("Grid"), 16.0f);
		ImGui::Checkbox("Filled", &m_filledShape);
		ImGui::SameLine();
		ImGui::Checkbox("Grid", &m_showGrid);

		chrome::SameLineOrWrap(chrome::ButtonWidth(ICON_FA_ROTATE " Undo") + style.ItemSpacing.x + chrome::ButtonWidth("Redo"), 16.0f);
		ImGui::BeginDisabled(!doc.CanUndo());
		if (chrome::GhostButton(ICON_FA_ROTATE " Undo"))
		{
			doc.Undo();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!doc.CanRedo());
		if (chrome::GhostButton("Redo"))
		{
			doc.Redo();
		}
		ImGui::EndDisabled();

		// The slider plus its trailing label, so the whole control wraps together.
		constexpr float kZoomSliderWidth = 90.0f;
		chrome::SameLineOrWrap(kZoomSliderWidth + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Zoom").x, 16.0f);
		ImGui::SetNextItemWidth(kZoomSliderWidth);
		ImGui::SliderInt("Zoom", &m_zoom, 1, 40, "%dx");

		// New / Open / Save row.
		ImGui::SetNextItemWidth(60.0f);
		ImGui::DragInt("##nw", &m_newW, 1, 1, PixelArtDocument::kMaxSize);
		ImGui::SameLine();
		ImGui::TextUnformatted("x");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(60.0f);
		ImGui::DragInt("##nh", &m_newH, 1, 1, PixelArtDocument::kMaxSize);
		ImGui::SameLine();
		if (chrome::OutlineButton(ICON_FA_PLUS " New"))
		{
			doc.New(m_newW, m_newH);
			m_status = "New " + std::to_string(m_newW) + "x" + std::to_string(m_newH) + " canvas";
		}

		// Reserve what Open and Save actually measure. A round 200 was enough at the default
		// UI scale and nowhere near it at a larger one, which pushed Save off the panel.
		ImGui::SetNextItemWidth(-(chrome::ButtonWidth(ICON_FA_FOLDER_OPEN " Open") + chrome::ButtonWidth(ICON_FA_FLOPPY_DISK " Save") + style.ItemSpacing.x * 2.0f));
		ImGui::InputText("##savepath", &m_savePath);
		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Open"))
		{
			// Freehand pixels are not recoverable once loaded over.
			if (doc.Dirty())
			{
				m_pendingOpenPath = m_savePath;
			}
			else
			{
				const std::filesystem::path disk = app::ResolveProjectPath(context.TryGet<app::EditorProjectContext>(), m_savePath);
				m_status = doc.Load(disk) ? "Opened " + m_savePath : "Open failed: " + m_savePath;
			}
		}
		ImGui::SameLine();
		if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK " Save"))
		{
			const std::filesystem::path disk = app::ResolveProjectPath(context.TryGet<app::EditorProjectContext>(), m_savePath);
			m_status = doc.Save(disk) ? "Saved " + m_savePath : "Save failed: " + m_savePath;
		}
		if (!m_status.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", m_status.c_str());
		}
	}

	void PixelArtPanel::DrawPalette(PixelArtDocument& doc)
	{
		chrome::SectionTag("COLOUR");
		ImGui::Spacing();
		ImVec4 col = ToVec4(doc.Color());
		if (ImGui::ColorPicker4("##color", &col.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
		{
			doc.SetColor(FromVec4(col));
		}

		ImGui::Spacing();
		chrome::SectionTag("PALETTE");
		ImGui::Spacing();
		auto& palette = doc.Palette();
		const float swatch = 22.0f;
		// n swatches occupy n*swatch + (n-1)*spacing. Assuming a 4px gap when the style says
		// 8 overcounted the row, so the last swatch was drawn past the edge and cut in half.
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (swatch + spacing)));
		for (std::size_t i = 0; i < palette.size(); ++i)
		{
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::ColorButton("##sw", ToVec4(palette[i]), ImGuiColorEditFlags_AlphaPreview, ImVec2(swatch, swatch)))
			{
				doc.SetColor(palette[i]);
			}
			ImGui::PopID();
			if ((static_cast<int>(i) % perRow) != perRow - 1)
			{
				ImGui::SameLine();
			}
		}
		ImGui::NewLine();
		if (chrome::GhostButton(ICON_FA_PLUS " Add current"))
		{
			palette.push_back(doc.Color());
		}
	}

	void PixelArtPanel::DrawCanvas(PixelArtDocument& doc)
	{
		const float zoom = static_cast<float>(m_zoom);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 canvasSize(doc.Width() * zoom, doc.Height() * zoom);
		ImDrawList* draw = ImGui::GetWindowDrawList();

		// Transparency checkerboard.
		const float check = std::max(zoom, 4.0f);
		for (float y = 0; y < canvasSize.y; y += check)
		{
			for (float x = 0; x < canvasSize.x; x += check)
			{
				const bool dark = (static_cast<int>(x / check) + static_cast<int>(y / check)) % 2 == 0;
				const ImU32 c = dark ? IM_COL32(70, 70, 76, 255) : IM_COL32(96, 96, 104, 255);
				draw->AddRectFilled(ImVec2(origin.x + x, origin.y + y), ImVec2(origin.x + std::min(x + check, canvasSize.x), origin.y + std::min(y + check, canvasSize.y)), c);
			}
		}

		// Painted pixels (skip fully transparent so the checkerboard shows).
		for (int y = 0; y < doc.Height(); ++y)
		{
			for (int x = 0; x < doc.Width(); ++x)
			{
				const std::uint32_t p = doc.GetPixel(x, y);
				if ((p >> 24) == 0)
				{
					continue;
				}
				const ImVec2 a(origin.x + x * zoom, origin.y + y * zoom);
				draw->AddRectFilled(a, ImVec2(a.x + zoom, a.y + zoom), p);
			}
		}

		// Grid.
		if (m_showGrid && m_zoom >= 6)
		{
			const ImU32 grid = IM_COL32(0, 0, 0, 40);
			for (int x = 0; x <= doc.Width(); ++x)
			{
				draw->AddLine(ImVec2(origin.x + x * zoom, origin.y), ImVec2(origin.x + x * zoom, origin.y + canvasSize.y), grid);
			}
			for (int y = 0; y <= doc.Height(); ++y)
			{
				draw->AddLine(ImVec2(origin.x, origin.y + y * zoom), ImVec2(origin.x + canvasSize.x, origin.y + y * zoom), grid);
			}
		}

		// Interaction.
		ImGui::InvisibleButton("##canvasInput", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const int cx = static_cast<int>(std::floor((mouse.x - origin.x) / zoom));
		const int cy = static_cast<int>(std::floor((mouse.y - origin.y) / zoom));
		const std::uint32_t paintColor = m_tool == Tool::Eraser ? 0u : doc.Color();

		if (hovered && doc.InBounds(cx, cy))
		{
			// Hover cell highlight.
			const ImVec2 a(origin.x + cx * zoom, origin.y + cy * zoom);
			draw->AddRect(a, ImVec2(a.x + zoom, a.y + zoom), IM_COL32(255, 255, 255, 180));

			if (ImGui::IsItemActivated())
			{
				switch (m_tool)
				{
					case Tool::Pencil:
					case Tool::Eraser:
						doc.Snapshot();
						doc.SetPixel(cx, cy, paintColor);
						m_painting = true;
						m_lastX = cx;
						m_lastY = cy;
						break;
					case Tool::Fill:
						doc.Snapshot();
						doc.FloodFill(cx, cy, doc.Color());
						break;
					case Tool::Eyedropper:
						doc.SetColor(doc.GetPixel(cx, cy));
						break;
					case Tool::Line:
					case Tool::Rectangle:
					case Tool::Ellipse:
						m_shaping = true;
						m_shapeX0 = cx;
						m_shapeY0 = cy;
						m_shapeX1 = cx;
						m_shapeY1 = cy;
						break;
				}
			}
		}

		// Pencil/eraser drag: interpolate to avoid gaps at speed.
		if (m_painting && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (cx != m_lastX || cy != m_lastY)
			{
				doc.DrawLine(m_lastX, m_lastY, cx, cy, paintColor);
				m_lastX = cx;
				m_lastY = cy;
			}
		}
		else
		{
			m_painting = false;
		}

		// Shape tools: live preview, commit on release.
		if (m_shaping)
		{
			m_shapeX1 = cx;
			m_shapeY1 = cy;
			const ImU32 preview = doc.Color() | 0xFF000000u; // preview always visible
			// Live outline preview; the real per-pixel op runs on mouse release.
			const int lx = std::min(m_shapeX0, m_shapeX1);
			const int hx = std::max(m_shapeX0, m_shapeX1);
			const int ly = std::min(m_shapeY0, m_shapeY1);
			const int hy = std::max(m_shapeY0, m_shapeY1);
			if (m_tool == Tool::Line)
			{
				const ImVec2 a(origin.x + (m_shapeX0 + 0.5f) * zoom, origin.y + (m_shapeY0 + 0.5f) * zoom);
				const ImVec2 b(origin.x + (m_shapeX1 + 0.5f) * zoom, origin.y + (m_shapeY1 + 0.5f) * zoom);
				draw->AddLine(a, b, preview, std::max(1.0f, zoom * 0.5f));
			}
			else
			{
				const ImVec2 a(origin.x + lx * zoom, origin.y + ly * zoom);
				const ImVec2 b(origin.x + (hx + 1) * zoom, origin.y + (hy + 1) * zoom);
				if (m_tool == Tool::Ellipse)
				{
					draw->AddEllipse(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ImVec2((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f), preview);
				}
				else
				{
					draw->AddRect(a, b, preview);
				}
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				doc.Snapshot();
				switch (m_tool)
				{
					case Tool::Line:
						doc.DrawLine(m_shapeX0, m_shapeY0, m_shapeX1, m_shapeY1, doc.Color());
						break;
					case Tool::Rectangle:
						doc.DrawRect(m_shapeX0, m_shapeY0, m_shapeX1, m_shapeY1, doc.Color(), m_filledShape);
						break;
					case Tool::Ellipse:
						doc.DrawEllipse(m_shapeX0, m_shapeY0, m_shapeX1, m_shapeY1, doc.Color(), m_filledShape);
						break;
					// Not drag-to-shape tools: they act per pixel as the mouse moves, so a release
					// has nothing left to commit.
					case Tool::Pencil:
					case Tool::Eraser:
					case Tool::Fill:
					case Tool::Eyedropper:
						break;
				}
				m_shaping = false;
			}
		}
	}

	bool PixelArtPanel::UndoIfFocused(app::LayerContext& context)
	{
		// IsVisible as well as focus: a hidden panel never draws, so m_focused would
		// keep whatever it held when the panel was last closed and swallow the key.
		auto* doc = context.TryGet<PixelArtDocument>();
		if (!m_focused || !IsVisible() || doc == nullptr || !doc->CanUndo())
		{
			return false;
		}
		doc->Undo();
		return true;
	}

	bool PixelArtPanel::RedoIfFocused(app::LayerContext& context)
	{
		auto* doc = context.TryGet<PixelArtDocument>();
		if (!m_focused || !IsVisible() || doc == nullptr || !doc->CanRedo())
		{
			return false;
		}
		doc->Redo();
		return true;
	}

	bool PixelArtPanel::HasUnsavedWork(app::LayerContext& context) const
	{
		// The canvas is a service, not panel state, so it stays dirty even while this window
		// is closed - which is exactly when it would otherwise be lost without a word.
		const auto* doc = context.TryGet<PixelArtDocument>();
		return doc != nullptr && doc->Dirty();
	}

	bool PixelArtPanel::SaveUnsavedWork(app::LayerContext& context)
	{
		auto* doc = context.TryGet<PixelArtDocument>();
		if (doc == nullptr)
		{
			return true;
		}
		const std::filesystem::path disk = app::ResolveProjectPath(context.TryGet<app::EditorProjectContext>(), m_savePath);
		return doc->Save(disk) && !doc->Dirty();
	}
} // namespace aether::editor
