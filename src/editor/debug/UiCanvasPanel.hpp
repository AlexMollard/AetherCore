#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glm/vec2.hpp>
#include <imgui.h>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "scene/Entity.hpp"
#include "ui/UiComponents.hpp"

namespace aether::editor
{
	enum class UiRectResizeHandle : std::uint8_t
	{
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight
	};

	enum class UiAnchorHandle : std::uint8_t
	{
		Point,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight
	};

	struct DragOrigin
	{
		Entity entity;
		glm::vec2 startOffsetMin;
		glm::vec2 startOffsetMax;
	};

	class UiCanvasPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "UI Canvas";
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		enum class DragKind : std::uint8_t
		{
			None,
			Move,
			Resize,
			Anchor,
			CanvasResize,
			Marquee
		};

		struct DragState
		{
			DragKind kind = DragKind::None;
			Entity entity{};
			UiRectResizeHandle resize = UiRectResizeHandle::BottomRight;
			UiAnchorHandle anchor = UiAnchorHandle::Point;
			UiRectResizeHandle canvasResizeHandle = UiRectResizeHandle::BottomRight;

			glm::vec2 startMouseCanvas{0.f};
			glm::vec2 startOffsetMin{0.f};
			glm::vec2 startOffsetMax{0.f};
		};

		ImVec2 m_pan{0.f, 0.f};
		float m_zoom = 1.f;
		bool m_previewContent = true;
		DragState m_drag;
		bool m_snappingEnabled = true;
		Entity m_hoveredEntity{};
		ImVec2 m_marqueeStart{0.f, 0.f};
		ImVec2 m_marqueeEnd{0.f, 0.f};
		std::vector<DragOrigin> m_multiDragOrigins;

		// ImGui texture ids registered for UI-image previews, keyed by the image's
		// texture path. Lets the preview draw real textures like the game viewport
		// instead of a flat colour. The path is acquired once (kept for the session)
		// and its descriptor is reclaimed when the ImGui subsystem tears down.
		std::unordered_map<std::string, ImTextureID> m_texturePreviews;
	};
} // namespace aether::editor
