#pragma once

#include <cstdint>
#include <string_view>

#include <glm/vec2.hpp>
#include <imgui.h>

#include "debug/DebugPanel.hpp"
#include "scene/Entity.hpp"
#include "ui/UiComponents.hpp"

namespace aether::app
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

	class UiCanvasPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "UI Canvas";
		}

		void OnImGui(LayerContext& context) override;

	private:
		enum class DragKind : std::uint8_t
		{
			None,
			Move,
			Resize,
			Anchor
		};

		struct DragState
		{
			DragKind kind = DragKind::None;
			Entity entity{};
			UiRectResizeHandle resize = UiRectResizeHandle::BottomRight;
			UiAnchorHandle anchor = UiAnchorHandle::Point;

			glm::vec2 startMouseCanvas{0.f};
			glm::vec2 startOffsetMin{0.f};
			glm::vec2 startOffsetMax{0.f};
		};

		ImVec2 m_pan{0.f, 0.f};
		float m_zoom = 1.f;
		bool m_previewContent = true;
		DragState m_drag;
	};
} // namespace aether::app
