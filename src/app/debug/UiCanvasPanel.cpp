#include "UiCanvasPanel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/common.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "Color.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	namespace
	{
		constexpr float kMinZoom = 0.1f;
		constexpr float kMaxZoom = 8.f;
		constexpr float kHandleSize = 8.f;
		constexpr float kAnchorHandleSize = 9.f;
		constexpr float kGridStep = 64.f;

		struct UiElement
		{
			Entity entity{};
			ui::UIRect* rect = nullptr;
			glm::vec4 canvasRect{0.f};
			glm::vec4 parentRect{0.f};
			ImVec2 min{};
			ImVec2 max{};
		};

		struct ResizeHit
		{
			UiRectResizeHandle handle;
			ImVec2 center;
		};

		struct AnchorHit
		{
			UiAnchorHandle handle;
			ImVec2 center;
		};

		[[nodiscard]] bool IsAlive(const World& world, Entity entity)
		{
			return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
		}

		[[nodiscard]] bool Contains(ImVec2 p, ImVec2 min, ImVec2 max)
		{
			return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
		}

		[[nodiscard]] Entity FirstCanvas(World& world)
		{
			Entity canvas{};
			world.View<ui::UICanvas>().each(
			        [&](entt::entity canvasEntity, ui::UICanvas&)
			        {
				        if (!canvas.IsValid())
				        {
					        canvas = World::FromEntt(canvasEntity);
				        }
			        });
			return canvas;
		}

		[[nodiscard]] Entity CanvasForEntity(World& world, Entity entity)
		{
			Entity current = entity;
			while (IsAlive(world, current))
			{
				if (world.Has<ui::UICanvas>(current))
				{
					return current;
				}
				const auto* hierarchy = world.TryGet<HierarchyComponent>(current);
				if (hierarchy == nullptr)
				{
					break;
				}
				current = hierarchy->parent;
			}
			return {};
		}

		[[nodiscard]] Entity ActiveCanvas(World& world, const SceneSelection* selection)
		{
			if (selection != nullptr)
			{
				const Entity selectedCanvas = CanvasForEntity(world, selection->Primary());
				if (selectedCanvas.IsValid())
				{
					return selectedCanvas;
				}
			}
			return FirstCanvas(world);
		}

		[[nodiscard]] bool IsInCanvasSubtree(World& world, Entity entity, Entity canvas)
		{
			Entity current = entity;
			while (IsAlive(world, current))
			{
				if (current == canvas)
				{
					return true;
				}
				const auto* hierarchy = world.TryGet<HierarchyComponent>(current);
				if (hierarchy == nullptr)
				{
					return false;
				}
				current = hierarchy->parent;
			}
			return false;
		}

		[[nodiscard]] glm::vec2 CanvasExtent(World& world, Entity canvas)
		{
			const auto* rect = world.TryGet<ui::UIRect>(canvas);
			if (rect != nullptr && rect->resolvedRect.z > 1.f && rect->resolvedRect.w > 1.f)
			{
				return {rect->resolvedRect.z, rect->resolvedRect.w};
			}
			const auto* canvasComponent = world.TryGet<ui::UICanvas>(canvas);
			if (canvasComponent != nullptr)
			{
				return glm::max(canvasComponent->referenceResolution, glm::vec2(1.f));
			}
			return {1920.f, 1080.f};
		}

		[[nodiscard]] ImVec2 Add(ImVec2 a, ImVec2 b)
		{
			return {a.x + b.x, a.y + b.y};
		}

		[[nodiscard]] ImVec2 Sub(ImVec2 a, ImVec2 b)
		{
			return {a.x - b.x, a.y - b.y};
		}

		[[nodiscard]] ImVec2 CanvasToScreen(glm::vec2 canvas, ImVec2 origin, ImVec2 pan, float zoom)
		{
			return Add(Add(origin, pan), ImVec2(canvas.x * zoom, canvas.y * zoom));
		}

		[[nodiscard]] glm::vec2 ScreenToCanvas(ImVec2 screen, ImVec2 origin, ImVec2 pan, float zoom)
		{
			const ImVec2 local = Sub(Sub(screen, origin), pan);
			return {local.x / zoom, local.y / zoom};
		}

		[[nodiscard]] ImVec2 RectMin(const glm::vec4& rect, ImVec2 origin, ImVec2 pan, float zoom)
		{
			return CanvasToScreen({rect.x, rect.y}, origin, pan, zoom);
		}

		[[nodiscard]] ImVec2 RectMax(const glm::vec4& rect, ImVec2 origin, ImVec2 pan, float zoom)
		{
			return CanvasToScreen({rect.x + rect.z, rect.y + rect.w}, origin, pan, zoom);
		}

		[[nodiscard]] std::vector<Entity> EntityChainFromCanvas(World& world, Entity entity, Entity canvas)
		{
			std::vector<Entity> chain;
			Entity current = entity;
			while (IsAlive(world, current) && current != canvas)
			{
				chain.push_back(current);
				const auto* hierarchy = world.TryGet<HierarchyComponent>(current);
				if (hierarchy == nullptr)
				{
					chain.clear();
					break;
				}
				current = hierarchy->parent;
			}
			if (current != canvas)
			{
				chain.clear();
			}
			std::reverse(chain.begin(), chain.end());
			return chain;
		}

		[[nodiscard]] glm::vec4 ResolveParentRect(World& world, Entity entity, Entity canvas, glm::vec2 extent)
		{
			glm::vec4 parentRect{0.f, 0.f, extent.x, extent.y};
			std::vector<Entity> chain = EntityChainFromCanvas(world, entity, canvas);
			if (!chain.empty())
			{
				chain.pop_back();
			}
			for (const Entity ancestor: chain)
			{
				if (const auto* rect = world.TryGet<ui::UIRect>(ancestor))
				{
					parentRect = ui::ResolveRect(parentRect, *rect);
				}
			}
			return parentRect;
		}

		[[nodiscard]] glm::vec4 ResolveElementRect(World& world, Entity entity, Entity canvas, glm::vec2 extent)
		{
			const glm::vec4 parentRect = ResolveParentRect(world, entity, canvas, extent);
			if (const auto* rect = world.TryGet<ui::UIRect>(entity))
			{
				return ui::ResolveRect(parentRect, *rect);
			}
			return parentRect;
		}

		[[nodiscard]] std::array<ResizeHit, 8> ResizeHandles(ImVec2 min, ImVec2 max)
		{
			const ImVec2 mid{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
			return {{{UiRectResizeHandle::TopLeft, min},
			        {UiRectResizeHandle::Top, {mid.x, min.y}},
			        {UiRectResizeHandle::TopRight, {max.x, min.y}},
			        {UiRectResizeHandle::Right, {max.x, mid.y}},
			        {UiRectResizeHandle::BottomRight, max},
			        {UiRectResizeHandle::Bottom, {mid.x, max.y}},
			        {UiRectResizeHandle::BottomLeft, {min.x, max.y}},
			        {UiRectResizeHandle::Left, {min.x, mid.y}}}};
		}

		[[nodiscard]] glm::vec2 SnapAnchor(glm::vec2 anchor, bool snap)
		{
			anchor = glm::clamp(anchor, glm::vec2(0.f), glm::vec2(1.f));
			if (!snap)
			{
				return anchor;
			}
			const auto snapOne = [](float value)
			{
				const float toZero = std::abs(value);
				const float toHalf = std::abs(value - 0.5f);
				const float toOne = std::abs(value - 1.f);
				if (toZero <= toHalf && toZero <= toOne)
				{
					return 0.f;
				}
				return toHalf <= toOne ? 0.5f : 1.f;
			};
			return {snapOne(anchor.x), snapOne(anchor.y)};
		}

		void PreserveVisualRectAfterAnchorChange(ui::UIRect& rect, const glm::vec4& parentRect, const glm::vec4& visualRect, glm::vec2 anchorMin, glm::vec2 anchorMax)
		{
			const glm::vec2 parentMin{parentRect.x, parentRect.y};
			const glm::vec2 parentSize{parentRect.z, parentRect.w};
			const glm::vec2 visualMin{visualRect.x, visualRect.y};
			const glm::vec2 visualMax{visualRect.x + visualRect.z, visualRect.y + visualRect.w};
			rect.anchorMin = glm::min(anchorMin, anchorMax);
			rect.anchorMax = glm::max(anchorMin, anchorMax);
			rect.offsetMin = visualMin - (parentMin + rect.anchorMin * parentSize);
			rect.offsetMax = visualMax - (parentMin + rect.anchorMax * parentSize);
		}

		[[nodiscard]] std::vector<AnchorHit> AnchorHandles(const ui::UIRect& rect, const glm::vec4& parentRect, ImVec2 origin, ImVec2 pan, float zoom)
		{
			const glm::vec2 parentMin{parentRect.x, parentRect.y};
			const glm::vec2 parentSize{parentRect.z, parentRect.w};
			const bool collapsed = rect.anchorMin == rect.anchorMax;
			if (collapsed)
			{
				const ImVec2 point = CanvasToScreen(parentMin + rect.anchorMin * parentSize, origin, pan, zoom);
				return {{UiAnchorHandle::Point, point}};
			}

			return {{UiAnchorHandle::TopLeft, CanvasToScreen(parentMin + glm::vec2{rect.anchorMin.x * parentSize.x, rect.anchorMin.y * parentSize.y}, origin, pan, zoom)},
			        {UiAnchorHandle::TopRight, CanvasToScreen(parentMin + glm::vec2{rect.anchorMax.x * parentSize.x, rect.anchorMin.y * parentSize.y}, origin, pan, zoom)},
			        {UiAnchorHandle::BottomLeft, CanvasToScreen(parentMin + glm::vec2{rect.anchorMin.x * parentSize.x, rect.anchorMax.y * parentSize.y}, origin, pan, zoom)},
			        {UiAnchorHandle::BottomRight, CanvasToScreen(parentMin + glm::vec2{rect.anchorMax.x * parentSize.x, rect.anchorMax.y * parentSize.y}, origin, pan, zoom)}};
		}

		void MoveAnchor(ui::UIRect& rect, UiAnchorHandle handle, glm::vec2 canvasPoint, const glm::vec4& parentRect, const glm::vec4& visualRect, bool snap)
		{
			const glm::vec2 parentMin{parentRect.x, parentRect.y};
			const glm::vec2 parentSize = glm::max(glm::vec2{parentRect.z, parentRect.w}, glm::vec2(1.f));
			const glm::vec2 anchor = SnapAnchor((canvasPoint - parentMin) / parentSize, snap);
			glm::vec2 anchorMin = rect.anchorMin;
			glm::vec2 anchorMax = rect.anchorMax;
			switch (handle)
			{
				case UiAnchorHandle::Point:
					anchorMin = anchor;
					anchorMax = anchor;
					break;
				case UiAnchorHandle::TopLeft:
					anchorMin = anchor;
					break;
				case UiAnchorHandle::TopRight:
					anchorMax.x = anchor.x;
					anchorMin.y = anchor.y;
					break;
				case UiAnchorHandle::BottomLeft:
					anchorMin.x = anchor.x;
					anchorMax.y = anchor.y;
					break;
				case UiAnchorHandle::BottomRight:
					anchorMax = anchor;
					break;
			}
			PreserveVisualRectAfterAnchorChange(rect, parentRect, visualRect, anchorMin, anchorMax);
		}

		void DrawGrid(ImDrawList* drawList, ImVec2 canvasMin, ImVec2 canvasMax, ImVec2 origin, ImVec2 pan, float zoom, glm::vec2 extent)
		{
			const float step = kGridStep * zoom;
			if (step < 8.f)
			{
				return;
			}
			const ImU32 minor = ToU32(colors::detail::rgba(226, 214, 196, 0.08f));
			const ImU32 major = ToU32(colors::detail::rgba(226, 214, 196, 0.16f));
			for (float x = 0.f; x <= extent.x; x += kGridStep)
			{
				const ImVec2 a = CanvasToScreen({x, 0.f}, origin, pan, zoom);
				const ImVec2 b = CanvasToScreen({x, extent.y}, origin, pan, zoom);
				drawList->AddLine(a, b, (static_cast<int>(x) % 256 == 0) ? major : minor);
			}
			for (float y = 0.f; y <= extent.y; y += kGridStep)
			{
				const ImVec2 a = CanvasToScreen({0.f, y}, origin, pan, zoom);
				const ImVec2 b = CanvasToScreen({extent.x, y}, origin, pan, zoom);
				drawList->AddLine(a, b, (static_cast<int>(y) % 256 == 0) ? major : minor);
			}
			drawList->AddRect(canvasMin, canvasMax, ToU32(colors::Border), 0.f, 0, 1.5f);
		}
	} // namespace

	void TranslateUiRectOffsets(ui::UIRect& rect, glm::vec2 canvasDelta)
	{
		rect.offsetMin += canvasDelta;
		rect.offsetMax += canvasDelta;
	}

	void ResizeUiRectOffsets(ui::UIRect& rect, UiRectResizeHandle handle, glm::vec2 canvasDelta, glm::vec2 parentExtent)
	{
		const bool left = handle == UiRectResizeHandle::Left || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::BottomLeft;
		const bool right = handle == UiRectResizeHandle::Right || handle == UiRectResizeHandle::TopRight || handle == UiRectResizeHandle::BottomRight;
		const bool top = handle == UiRectResizeHandle::Top || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::TopRight;
		const bool bottom = handle == UiRectResizeHandle::Bottom || handle == UiRectResizeHandle::BottomLeft || handle == UiRectResizeHandle::BottomRight;

		if (left)
		{
			rect.offsetMin.x += canvasDelta.x;
		}
		if (right)
		{
			rect.offsetMax.x += canvasDelta.x;
		}
		if (top)
		{
			rect.offsetMin.y += canvasDelta.y;
		}
		if (bottom)
		{
			rect.offsetMax.y += canvasDelta.y;
		}

		const glm::vec2 anchorSpan = (rect.anchorMax - rect.anchorMin) * glm::max(parentExtent, glm::vec2(1.f));
		constexpr float minSize = 1.f;
		const float width = anchorSpan.x + rect.offsetMax.x - rect.offsetMin.x;
		if (width < minSize)
		{
			if (left && !right)
			{
				rect.offsetMin.x = rect.offsetMax.x + anchorSpan.x - minSize;
			}
			else
			{
				rect.offsetMax.x = rect.offsetMin.x - anchorSpan.x + minSize;
			}
		}
		const float height = anchorSpan.y + rect.offsetMax.y - rect.offsetMin.y;
		if (height < minSize)
		{
			if (top && !bottom)
			{
				rect.offsetMin.y = rect.offsetMax.y + anchorSpan.y - minSize;
			}
			else
			{
				rect.offsetMax.y = rect.offsetMin.y - anchorSpan.y + minSize;
			}
		}
	}

	void UiCanvasPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("UI Canvas", VisiblePtr());

		World& world = context.Get<World>();
		auto* selection = context.TryGet<SceneSelection>();
		const Entity canvas = ActiveCanvas(world, selection);

		ImGui::SetNextItemWidth(96.f);
		if (ImGui::DragFloat("Zoom", &m_zoom, 0.01f, kMinZoom, kMaxZoom, "%.2fx"))
		{
			m_zoom = std::clamp(m_zoom, kMinZoom, kMaxZoom);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_ROTATE))
		{
			m_pan = {0.f, 0.f};
			m_zoom = 1.f;
		}
		ImGui::SetItemTooltip("Reset view");

		const ImVec2 contentMin = ImGui::GetCursorScreenPos();
		ImVec2 canvasArea = ImGui::GetContentRegionAvail();
		canvasArea.x = std::max(canvasArea.x, 1.f);
		canvasArea.y = std::max(canvasArea.y, 1.f);
		const ImVec2 contentMax = Add(contentMin, canvasArea);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(contentMin, contentMax, ToU32(colors::Surface));
		ImGui::InvisibleButton("##ui-canvas-surface", canvasArea, ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonLeft);

		if (!canvas.IsValid())
		{
			ImGui::End();
			return;
		}

		glm::vec2 extent = CanvasExtent(world, canvas);

		const ImVec2 canvasSize{extent.x * m_zoom, extent.y * m_zoom};
		const ImVec2 origin{contentMin.x + (canvasArea.x - canvasSize.x) * 0.5f, contentMin.y + (canvasArea.y - canvasSize.y) * 0.5f};
		const ImVec2 canvasMin = Add(origin, m_pan);
		const ImVec2 canvasMax = Add(canvasMin, canvasSize);
		const bool surfaceHovered = ImGui::IsWindowHovered() && Contains(ImGui::GetMousePos(), contentMin, contentMax);

		if (surfaceHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
		{
			const ImVec2 delta = ImGui::GetIO().MouseDelta;
			m_pan = Add(m_pan, delta);
		}

		drawList->PushClipRect(contentMin, contentMax, true);
		DrawGrid(drawList, canvasMin, canvasMax, origin, m_pan, m_zoom, extent);

		std::vector<UiElement> elements;
		world.View<ui::UIRect>().each(
		        [&](entt::entity enttEntity, ui::UIRect& rect)
		        {
			        const Entity entity = World::FromEntt(enttEntity);
			        if (entity == canvas || !IsInCanvasSubtree(world, entity, canvas))
			        {
				        return;
			        }
			        const glm::vec4 parentRect = ResolveParentRect(world, entity, canvas, extent);
			        const glm::vec4 canvasRect = ui::ResolveRect(parentRect, rect);
			        elements.push_back({entity, &rect, canvasRect, parentRect, RectMin(canvasRect, origin, m_pan, m_zoom), RectMax(canvasRect, origin, m_pan, m_zoom)});
		        });

		const Entity selected = selection != nullptr ? selection->Primary() : Entity{};
		glm::vec4 selectedCanvasRect{0.f};
		glm::vec4 selectedParentRect{0.f};
		for (const UiElement& element: elements)
		{
			if (element.entity == selected)
			{
				selectedCanvasRect = element.canvasRect;
				selectedParentRect = element.parentRect;
				break;
			}
		}
		for (const UiElement& element: elements)
		{
			const bool isSelected = element.entity == selected;
			const ImU32 outline = isSelected ? ToU32(colors::Primary) : ToU32(colors::Neutral);
			const ImU32 fill = isSelected ? ToU32(colors::detail::rgba(255, 124, 50, 0.10f)) : ToU32(colors::detail::rgba(226, 214, 196, 0.05f));
			drawList->AddRectFilled(element.min, element.max, fill);
			drawList->AddRect(element.min, element.max, outline, 0.f, 0, isSelected ? 2.f : 1.f);
		}

		ui::UIRect* selectedRect = selected.IsValid() ? world.TryGet<ui::UIRect>(selected) : nullptr;
		if (selectedRect != nullptr && IsInCanvasSubtree(world, selected, canvas))
		{
			selectedParentRect = selectedParentRect.z > 0.f && selectedParentRect.w > 0.f ? selectedParentRect : ResolveParentRect(world, selected, canvas, extent);
			selectedCanvasRect = selectedCanvasRect.z > 0.f && selectedCanvasRect.w > 0.f ? selectedCanvasRect : ui::ResolveRect(selectedParentRect, *selectedRect);
			const ImVec2 selectedMin = RectMin(selectedCanvasRect, origin, m_pan, m_zoom);
			const ImVec2 selectedMax = RectMax(selectedCanvasRect, origin, m_pan, m_zoom);
			const ImU32 handleFill = ToU32(colors::Primary);
			for (const ResizeHit& hit: ResizeHandles(selectedMin, selectedMax))
			{
				const ImVec2 half{kHandleSize * 0.5f, kHandleSize * 0.5f};
				drawList->AddRectFilled(Sub(hit.center, half), Add(hit.center, half), handleFill);
			}

			const ImU32 anchorColor = ToU32(colors::Info);
			for (const AnchorHit& hit: AnchorHandles(*selectedRect, selectedParentRect, origin, m_pan, m_zoom))
			{
				const ImVec2 half{kAnchorHandleSize * 0.5f, kAnchorHandleSize * 0.5f};
				drawList->AddRect(Sub(hit.center, half), Add(hit.center, half), anchorColor, 0.f, ImDrawFlags_RoundCornersAll, 2.f);
				drawList->AddLine({hit.center.x - half.x, hit.center.y}, {hit.center.x + half.x, hit.center.y}, anchorColor);
				drawList->AddLine({hit.center.x, hit.center.y - half.y}, {hit.center.x, hit.center.y + half.y}, anchorColor);
			}
		}

		if (surfaceHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_drag = {};
			const ImVec2 mouse = ImGui::GetMousePos();
			if (selectedRect != nullptr && IsInCanvasSubtree(world, selected, canvas))
			{
				selectedParentRect = selectedParentRect.z > 0.f && selectedParentRect.w > 0.f ? selectedParentRect : ResolveParentRect(world, selected, canvas, extent);
				selectedCanvasRect = selectedCanvasRect.z > 0.f && selectedCanvasRect.w > 0.f ? selectedCanvasRect : ui::ResolveRect(selectedParentRect, *selectedRect);
				const ImVec2 selectedMin = RectMin(selectedCanvasRect, origin, m_pan, m_zoom);
				const ImVec2 selectedMax = RectMax(selectedCanvasRect, origin, m_pan, m_zoom);
				for (const AnchorHit& hit: AnchorHandles(*selectedRect, selectedParentRect, origin, m_pan, m_zoom))
				{
					const ImVec2 half{kAnchorHandleSize * 0.5f, kAnchorHandleSize * 0.5f};
					if (Contains(mouse, Sub(hit.center, half), Add(hit.center, half)))
					{
						m_drag.kind = DragKind::Anchor;
						m_drag.entity = selected;
						m_drag.anchor = hit.handle;
						break;
					}
				}
				if (m_drag.kind == DragKind::None)
				{
					for (const ResizeHit& hit: ResizeHandles(selectedMin, selectedMax))
					{
						const ImVec2 half{kHandleSize * 0.5f, kHandleSize * 0.5f};
						if (Contains(mouse, Sub(hit.center, half), Add(hit.center, half)))
						{
							m_drag.kind = DragKind::Resize;
							m_drag.entity = selected;
							m_drag.resize = hit.handle;
							break;
						}
					}
				}
			}
			if (m_drag.kind == DragKind::None)
			{
				for (auto it = elements.rbegin(); it != elements.rend(); ++it)
				{
					if (Contains(mouse, it->min, it->max))
					{
						if (selection != nullptr)
						{
							selection->Select(it->entity);
						}
						m_drag.kind = DragKind::Move;
						m_drag.entity = it->entity;
						break;
					}
				}
			}
			if (m_drag.kind == DragKind::None && Contains(mouse, canvasMin, canvasMax) && selection != nullptr)
			{
				selection->Select(canvas);
			}
		}

		if (m_drag.kind != DragKind::None)
		{
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !IsAlive(world, m_drag.entity))
			{
				m_drag = {};
			}
			else if (auto* rect = world.TryGet<ui::UIRect>(m_drag.entity))
			{
				const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
				const glm::vec2 canvasDelta{mouseDelta.x / m_zoom, mouseDelta.y / m_zoom};
				if (m_drag.kind == DragKind::Move)
				{
					TranslateUiRectOffsets(*rect, canvasDelta);
				}
				else if (m_drag.kind == DragKind::Resize)
				{
					const glm::vec4 parentRect = ResolveParentRect(world, m_drag.entity, canvas, extent);
					ResizeUiRectOffsets(*rect, m_drag.resize, canvasDelta, {parentRect.z, parentRect.w});
				}
				else if (m_drag.kind == DragKind::Anchor)
				{
					const glm::vec4 parentRect = ResolveParentRect(world, m_drag.entity, canvas, extent);
					const glm::vec4 visualRect = ui::ResolveRect(parentRect, *rect);
					MoveAnchor(*rect, m_drag.anchor, ScreenToCanvas(ImGui::GetMousePos(), origin, m_pan, m_zoom), parentRect, visualRect, ImGui::GetIO().KeyShift);
				}
			}
		}

		drawList->PopClipRect();
		ImGui::End();
	}
} // namespace aether::app
