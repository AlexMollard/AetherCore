#include "UiCanvasPanel.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ranges>
#include <vector>

#include <glm/common.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "Color.hpp"
#include "assets/AssetManager.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "ui/UiEntities.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr float kMinZoom = 0.1f;
		constexpr float kMaxZoom = 8.f;
		constexpr float kHandleSize = 8.f;
		constexpr float kAnchorHandleSize = 9.f;
		constexpr float kGridStep = 64.f;
		constexpr float kSnapScreenDistance = 8.f;
		constexpr float kGapGuideMinPixels = 1.f;

		enum class GuideAxis : std::uint8_t
		{
			Horizontal,
			Vertical
		};

		struct UiElement
		{
			Entity entity{};
			ui::UIRect* rect = nullptr;
			glm::vec4 canvasRect{0.f};
			glm::vec4 parentRect{0.f};
			ImVec2 min;
			ImVec2 max;
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

		struct SnapGuide
		{
			GuideAxis axis = GuideAxis::Vertical;
			float position = 0.f;
		};

		struct GapGuide
		{
			GuideAxis axis = GuideAxis::Horizontal;
			float from = 0.f;
			float to = 0.f;
			float cross = 0.f;
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

		[[nodiscard]] Entity CreateParentForNewElement(World& world, SceneSelection* selection, Entity canvas)
		{
			if (selection == nullptr)
			{
				return canvas;
			}
			const Entity selected = selection->Primary();
			if (IsAlive(world, selected) && IsInCanvasSubtree(world, selected, canvas))
			{
				return selected;
			}
			return canvas;
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

		void AppendElementsPreOrder(World& world, Entity entity, Entity canvas, glm::vec2 extent, ImVec2 origin, ImVec2 pan, float zoom, std::vector<UiElement>& elements)
		{
			if (entity != canvas)
			{
				if (auto* rect = world.TryGet<ui::UIRect>(entity))
				{
					const glm::vec4 parentRect = ResolveParentRect(world, entity, canvas, extent);
					const glm::vec4 canvasRect = ui::ResolveRect(parentRect, *rect);
					elements.push_back({entity, rect, canvasRect, parentRect, RectMin(canvasRect, origin, pan, zoom), RectMax(canvasRect, origin, pan, zoom)});
				}
			}

			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			{
				for (const Entity child: hierarchy->children)
				{
					AppendElementsPreOrder(world, child, canvas, extent, origin, pan, zoom, elements);
				}
			}
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

		[[nodiscard]] ImGuiMouseCursor CursorForResizeHandle(UiRectResizeHandle handle)
		{
			switch (handle)
			{
				case UiRectResizeHandle::Left:
				case UiRectResizeHandle::Right:
					return ImGuiMouseCursor_ResizeEW;
				case UiRectResizeHandle::Top:
				case UiRectResizeHandle::Bottom:
					return ImGuiMouseCursor_ResizeNS;
				case UiRectResizeHandle::TopLeft:
				case UiRectResizeHandle::BottomRight:
					return ImGuiMouseCursor_ResizeNWSE;
				case UiRectResizeHandle::TopRight:
				case UiRectResizeHandle::BottomLeft:
					return ImGuiMouseCursor_ResizeNESW;
			}
			return ImGuiMouseCursor_Arrow;
		}

		[[nodiscard]] bool TryHitResizeHandle(ImVec2 mouse, ImVec2 rectMin, ImVec2 rectMax, UiRectResizeHandle& outHandle)
		{
			for (const ResizeHit& hit: ResizeHandles(rectMin, rectMax))
			{
				const ImVec2 half{kHandleSize * 0.5f, kHandleSize * 0.5f};
				if (Contains(mouse, Sub(hit.center, half), Add(hit.center, half)))
				{
					outHandle = hit.handle;
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] float RectLeft(const glm::vec4& rect)
		{
			return rect.x;
		}

		[[nodiscard]] float RectRight(const glm::vec4& rect)
		{
			return rect.x + rect.z;
		}

		[[nodiscard]] float RectTop(const glm::vec4& rect)
		{
			return rect.y;
		}

		[[nodiscard]] float RectBottom(const glm::vec4& rect)
		{
			return rect.y + rect.w;
		}

		[[nodiscard]] float RectCenterX(const glm::vec4& rect)
		{
			return rect.x + rect.z * 0.5f;
		}

		[[nodiscard]] float RectCenterY(const glm::vec4& rect)
		{
			return rect.y + rect.w * 0.5f;
		}

		[[nodiscard]] bool RangesOverlap(float a0, float a1, float b0, float b1)
		{
			return std::max(a0, b0) <= std::min(a1, b1);
		}

		void AppendRectSnapCandidates(const glm::vec4& rect, std::vector<float>& xCandidates, std::vector<float>& yCandidates)
		{
			xCandidates.push_back(RectLeft(rect));
			xCandidates.push_back(RectCenterX(rect));
			xCandidates.push_back(RectRight(rect));
			yCandidates.push_back(RectTop(rect));
			yCandidates.push_back(RectCenterY(rect));
			yCandidates.push_back(RectBottom(rect));
		}

		void BuildSnapCandidates(World& world, Entity active, const std::vector<UiElement>& elements, glm::vec2 extent, const glm::vec4& parentRect, std::vector<float>& xCandidates, std::vector<float>& yCandidates)
		{
			AppendRectSnapCandidates({0.f, 0.f, extent.x, extent.y}, xCandidates, yCandidates);
			AppendRectSnapCandidates(parentRect, xCandidates, yCandidates);

			for (const UiElement& element: elements)
			{
				if (IsInCanvasSubtree(world, element.entity, active))
				{
					continue;
				}
				AppendRectSnapCandidates(element.canvasRect, xCandidates, yCandidates);
			}
		}

		[[nodiscard]] bool TryBestSnapDelta(const std::vector<float>& sources, const std::vector<float>& candidates, float threshold, float& outDelta, float& outPosition)
		{
			float bestDistance = threshold;
			bool found = false;
			for (const float source: sources)
			{
				for (const float candidate: candidates)
				{
					const float delta = candidate - source;
					const float distance = std::abs(delta);
					if (distance <= bestDistance)
					{
						bestDistance = distance;
						outDelta = delta;
						outPosition = candidate;
						found = true;
					}
				}
			}
			return found;
		}

		void ApplyMoveSnap(World& world, ui::UIRect& rect, Entity active, const std::vector<UiElement>& elements, glm::vec2 extent, const glm::vec4& parentRect, float zoom, std::vector<SnapGuide>& snapGuides)
		{
			std::vector<float> xCandidates;
			std::vector<float> yCandidates;
			BuildSnapCandidates(world, active, elements, extent, parentRect, xCandidates, yCandidates);

			const float threshold = kSnapScreenDistance / std::max(zoom, 0.001f);
			glm::vec4 visualRect = ui::ResolveRect(parentRect, rect);

			{
				const float gridStep = 10.f;
				const float startX = std::floor((visualRect.x - threshold) / gridStep) * gridStep;
				const float endX = visualRect.x + visualRect.z + threshold;
				for (float x = startX; x <= endX; x += gridStep)
				{
					xCandidates.push_back(x);
				}
				const float startY = std::floor((visualRect.y - threshold) / gridStep) * gridStep;
				const float endY = visualRect.y + visualRect.w + threshold;
				for (float y = startY; y <= endY; y += gridStep)
				{
					yCandidates.push_back(y);
				}
			}

			float delta = 0.f;
			float position = 0.f;
			if (TryBestSnapDelta({RectLeft(visualRect), RectCenterX(visualRect), RectRight(visualRect)}, xCandidates, threshold, delta, position))
			{
				rect.offsetMin.x += delta;
				rect.offsetMax.x += delta;
				visualRect.x += delta;
				snapGuides.push_back({GuideAxis::Vertical, position});
			}
			if (TryBestSnapDelta({RectTop(visualRect), RectCenterY(visualRect), RectBottom(visualRect)}, yCandidates, threshold, delta, position))
			{
				rect.offsetMin.y += delta;
				rect.offsetMax.y += delta;
				snapGuides.push_back({GuideAxis::Horizontal, position});
			}
		}

		[[nodiscard]] bool ResizeKeepsMinimum(const glm::vec4& visualRect, UiRectResizeHandle handle, GuideAxis axis, float target)
		{
			constexpr float minSize = 1.f;
			if (axis == GuideAxis::Vertical)
			{
				const bool left = handle == UiRectResizeHandle::Left || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::BottomLeft;
				const bool right = handle == UiRectResizeHandle::Right || handle == UiRectResizeHandle::TopRight || handle == UiRectResizeHandle::BottomRight;
				return (!left || target <= RectRight(visualRect) - minSize) && (!right || target >= RectLeft(visualRect) + minSize);
			}

			const bool top = handle == UiRectResizeHandle::Top || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::TopRight;
			const bool bottom = handle == UiRectResizeHandle::Bottom || handle == UiRectResizeHandle::BottomLeft || handle == UiRectResizeHandle::BottomRight;
			return (!top || target <= RectBottom(visualRect) - minSize) && (!bottom || target >= RectTop(visualRect) + minSize);
		}

		[[nodiscard]] bool TryBestResizeSnap(float source, const std::vector<float>& candidates, float threshold, const glm::vec4& visualRect, UiRectResizeHandle handle, GuideAxis axis, float& outDelta, float& outPosition)
		{
			float bestDistance = threshold;
			bool found = false;
			for (const float candidate: candidates)
			{
				if (!ResizeKeepsMinimum(visualRect, handle, axis, candidate))
				{
					continue;
				}
				const float delta = candidate - source;
				const float distance = std::abs(delta);
				if (distance <= bestDistance)
				{
					bestDistance = distance;
					outDelta = delta;
					outPosition = candidate;
					found = true;
				}
			}
			return found;
		}

		void ApplyResizeSnap(World& world, ui::UIRect& rect, UiRectResizeHandle handle, Entity active, const std::vector<UiElement>& elements, glm::vec2 extent, const glm::vec4& parentRect, float zoom, std::vector<SnapGuide>& snapGuides)
		{
			std::vector<float> xCandidates;
			std::vector<float> yCandidates;
			BuildSnapCandidates(world, active, elements, extent, parentRect, xCandidates, yCandidates);

			const bool left = handle == UiRectResizeHandle::Left || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::BottomLeft;
			const bool right = handle == UiRectResizeHandle::Right || handle == UiRectResizeHandle::TopRight || handle == UiRectResizeHandle::BottomRight;
			const bool top = handle == UiRectResizeHandle::Top || handle == UiRectResizeHandle::TopLeft || handle == UiRectResizeHandle::TopRight;
			const bool bottom = handle == UiRectResizeHandle::Bottom || handle == UiRectResizeHandle::BottomLeft || handle == UiRectResizeHandle::BottomRight;

			const float threshold = kSnapScreenDistance / std::max(zoom, 0.001f);
			const glm::vec4 visualRect = ui::ResolveRect(parentRect, rect);

			{
				const float gridStep = 10.f;
				const float startX = std::floor((visualRect.x - threshold) / gridStep) * gridStep;
				const float endX = visualRect.x + visualRect.z + threshold;
				for (float x = startX; x <= endX; x += gridStep)
				{
					xCandidates.push_back(x);
				}
				const float startY = std::floor((visualRect.y - threshold) / gridStep) * gridStep;
				const float endY = visualRect.y + visualRect.w + threshold;
				for (float y = startY; y <= endY; y += gridStep)
				{
					yCandidates.push_back(y);
				}
			}

			float delta = 0.f;
			float position = 0.f;
			if (left && TryBestResizeSnap(RectLeft(visualRect), xCandidates, threshold, visualRect, handle, GuideAxis::Vertical, delta, position))
			{
				rect.offsetMin.x += delta;
				snapGuides.push_back({GuideAxis::Vertical, position});
			}
			else if (right && TryBestResizeSnap(RectRight(visualRect), xCandidates, threshold, visualRect, handle, GuideAxis::Vertical, delta, position))
			{
				rect.offsetMax.x += delta;
				snapGuides.push_back({GuideAxis::Vertical, position});
			}

			if (top && TryBestResizeSnap(RectTop(visualRect), yCandidates, threshold, visualRect, handle, GuideAxis::Horizontal, delta, position))
			{
				rect.offsetMin.y += delta;
				snapGuides.push_back({GuideAxis::Horizontal, position});
			}
			else if (bottom && TryBestResizeSnap(RectBottom(visualRect), yCandidates, threshold, visualRect, handle, GuideAxis::Horizontal, delta, position))
			{
				rect.offsetMax.y += delta;
				snapGuides.push_back({GuideAxis::Horizontal, position});
			}
		}

		void AppendGapGuide(float from, float to, float cross, GuideAxis axis, std::vector<GapGuide>& gapGuides)
		{
			if (std::abs(to - from) >= kGapGuideMinPixels)
			{
				gapGuides.push_back({axis, from, to, cross});
			}
		}

		void BuildGapGuides(World& world, const glm::vec4& visualRect, Entity active, const std::vector<UiElement>& elements, const glm::vec4& bounds, std::vector<GapGuide>& gapGuides)
		{
			const float left = RectLeft(visualRect);
			const float right = RectRight(visualRect);
			const float top = RectTop(visualRect);
			const float bottom = RectBottom(visualRect);
			const float centerX = RectCenterX(visualRect);
			const float centerY = RectCenterY(visualRect);

			float nearestLeft = RectLeft(bounds);
			float nearestRight = RectRight(bounds);
			float nearestTop = RectTop(bounds);
			float nearestBottom = RectBottom(bounds);

			for (const UiElement& element: elements)
			{
				if (IsInCanvasSubtree(world, element.entity, active))
				{
					continue;
				}

				const glm::vec4& other = element.canvasRect;
				if (RangesOverlap(top, bottom, RectTop(other), RectBottom(other)))
				{
					const float otherRight = RectRight(other);
					const float otherLeft = RectLeft(other);
					if (otherRight <= left && otherRight > nearestLeft)
					{
						nearestLeft = otherRight;
					}
					if (otherLeft >= right && otherLeft < nearestRight)
					{
						nearestRight = otherLeft;
					}
				}

				if (RangesOverlap(left, right, RectLeft(other), RectRight(other)))
				{
					const float otherBottom = RectBottom(other);
					const float otherTop = RectTop(other);
					if (otherBottom <= top && otherBottom > nearestTop)
					{
						nearestTop = otherBottom;
					}
					if (otherTop >= bottom && otherTop < nearestBottom)
					{
						nearestBottom = otherTop;
					}
				}
			}

			if (nearestLeft <= left)
			{
				AppendGapGuide(nearestLeft, left, centerY, GuideAxis::Horizontal, gapGuides);
			}
			if (nearestRight >= right)
			{
				AppendGapGuide(right, nearestRight, centerY, GuideAxis::Horizontal, gapGuides);
			}
			if (nearestTop <= top)
			{
				AppendGapGuide(nearestTop, top, centerX, GuideAxis::Vertical, gapGuides);
			}
			if (nearestBottom >= bottom)
			{
				AppendGapGuide(bottom, nearestBottom, centerX, GuideAxis::Vertical, gapGuides);
			}
		}

		void DrawSnapGuides(ImDrawList* drawList, const std::vector<SnapGuide>& snapGuides, ImVec2 origin, ImVec2 pan, float zoom, glm::vec2 extent)
		{
			const ImU32 guideColor = ToU32(colors::detail::rgba(56, 189, 255, 0.78f));
			for (const SnapGuide& guide: snapGuides)
			{
				if (guide.axis == GuideAxis::Vertical)
				{
					drawList->AddLine(CanvasToScreen({guide.position, 0.f}, origin, pan, zoom), CanvasToScreen({guide.position, extent.y}, origin, pan, zoom), guideColor, 1.5f);
				}
				else
				{
					drawList->AddLine(CanvasToScreen({0.f, guide.position}, origin, pan, zoom), CanvasToScreen({extent.x, guide.position}, origin, pan, zoom), guideColor, 1.5f);
				}
			}
		}

		void DrawGapGuides(ImDrawList* drawList, const std::vector<GapGuide>& gapGuides, ImVec2 origin, ImVec2 pan, float zoom)
		{
			const ImU32 lineColor = ToU32(colors::detail::rgba(226, 214, 196, 0.74f));
			const ImU32 textColor = ToU32(colors::TextPrimary);
			const ImU32 textBg = ToU32(colors::detail::rgba(30, 27, 24, 0.86f));
			for (const GapGuide& guide: gapGuides)
			{
				const bool horizontal = guide.axis == GuideAxis::Horizontal;
				const ImVec2 a = horizontal ? CanvasToScreen({guide.from, guide.cross}, origin, pan, zoom) : CanvasToScreen({guide.cross, guide.from}, origin, pan, zoom);
				const ImVec2 b = horizontal ? CanvasToScreen({guide.to, guide.cross}, origin, pan, zoom) : CanvasToScreen({guide.cross, guide.to}, origin, pan, zoom);
				const float screenDistance = horizontal ? std::abs(b.x - a.x) : std::abs(b.y - a.y);
				if (screenDistance < 6.f)
				{
					continue;
				}

				drawList->AddLine(a, b, lineColor, 1.f);
				const float cap = 4.f;
				if (horizontal)
				{
					drawList->AddLine({a.x, a.y - cap}, {a.x, a.y + cap}, lineColor, 1.f);
					drawList->AddLine({b.x, b.y - cap}, {b.x, b.y + cap}, lineColor, 1.f);
				}
				else
				{
					drawList->AddLine({a.x - cap, a.y}, {a.x + cap, a.y}, lineColor, 1.f);
					drawList->AddLine({b.x - cap, b.y}, {b.x + cap, b.y}, lineColor, 1.f);
				}

				char label[32]{};
				std::snprintf(label, sizeof(label), "%.0f px", std::abs(guide.to - guide.from));
				const ImVec2 labelSize = ImGui::CalcTextSize(label);
				const ImVec2 midpoint{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
				const ImVec2 labelMin{midpoint.x - labelSize.x * 0.5f - 4.f, midpoint.y - labelSize.y * 0.5f - 2.f};
				const ImVec2 labelMax{midpoint.x + labelSize.x * 0.5f + 4.f, midpoint.y + labelSize.y * 0.5f + 2.f};
				drawList->AddRectFilled(labelMin, labelMax, textBg, 3.f);
				drawList->AddText({labelMin.x + 4.f, labelMin.y + 2.f}, textColor, label);
			}
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

		void DrawPreviewElement(ImDrawList* drawList, World& world, const UiElement& element, float zoom, ImTextureID texId)
		{
			if (const auto* image = world.TryGet<ui::UIImage>(element.entity))
			{
				const ImU32 tint = ToU32(image->color);
				const float rounding = image->cornerRadius * zoom;
				if (texId != ImTextureID_Invalid)
				{
					// Render the real texture tinted by the image colour, matching the game viewport.
					if (rounding > 0.f)
					{
						drawList->AddImageRounded(texId, element.min, element.max, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), tint, rounding);
					}
					else
					{
						drawList->AddImage(texId, element.min, element.max, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), tint);
					}
				}
				else
				{
					drawList->AddRectFilled(element.min, element.max, tint, rounding);
				}
			}

			const auto* text = world.TryGet<ui::UIText>(element.entity);
			if (text == nullptr || text->text.empty())
			{
				return;
			}

			const float fontSize = std::clamp(text->pixelSize * zoom, 6.f, 160.f);
			const float width = std::max(element.max.x - element.min.x, 1.f);
			const float height = std::max(element.max.y - element.min.y, 1.f);
			const float wrapWidth = text->wrap ? width : FLT_MAX;
			ImFont* font = ImGui::GetFont();
			const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, text->text.c_str());

			ImVec2 pos = element.min;
			switch (text->hAlign)
			{
				case ui::UIText::HAlign::Left:
					break;
				case ui::UIText::HAlign::Center:
					pos.x += std::max((width - textSize.x) * 0.5f, 0.f);
					break;
				case ui::UIText::HAlign::Right:
					pos.x += std::max(width - textSize.x, 0.f);
					break;
			}
			switch (text->vAlign)
			{
				case ui::UIText::VAlign::Top:
					break;
				case ui::UIText::VAlign::Middle:
					pos.y += std::max((height - textSize.y) * 0.5f, 0.f);
					break;
				case ui::UIText::VAlign::Bottom:
					pos.y += std::max(height - textSize.y, 0.f);
					break;
			}

			const ImVec4 clip{element.min.x, element.min.y, element.max.x, element.max.y};
			drawList->AddText(font, fontSize, pos, ToU32(text->color), text->text.c_str(), nullptr, wrapWidth, &clip);
		}

		[[nodiscard]] bool RectsOverlap(ImVec2 min1, ImVec2 max1, ImVec2 min2, ImVec2 max2)
		{
			return min1.x < max2.x && max1.x > min2.x && min1.y < max2.y && max1.y > min2.y;
		}

		void DrawDashedLine(ImDrawList* drawList, ImVec2 a, ImVec2 b, ImU32 col, float dashLen = 6.f, float gapLen = 4.f)
		{
			ImVec2 dir = Sub(b, a);
			const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			if (len < 0.001f)
			{
				return;
			}
			dir.x /= len;
			dir.y /= len;

			float dist = 0.f;
			while (dist < len)
			{
				const ImVec2 p1 = Add(a, ImVec2{dir.x * dist, dir.y * dist});
				const float end = std::min(dist + dashLen, len);
				const ImVec2 p2 = Add(a, ImVec2{dir.x * end, dir.y * end});
				drawList->AddLine(p1, p2, col, 1.5f);
				dist += dashLen + gapLen;
			}
		}

		void DrawTooltip(ImDrawList* drawList, ImVec2 mousePos, const char* text)
		{
			const ImVec2 padding{6.f, 4.f};
			const ImVec2 textSize = ImGui::CalcTextSize(text);
			const ImVec2 min = Add(mousePos, ImVec2{15.f, 15.f});
			const ImVec2 max = Add(min, Add(textSize, Add(padding, padding)));

			drawList->AddRectFilled(min, max, ToU32(colors::detail::rgba(20, 20, 20, 0.9f)), 4.f);
			drawList->AddRect(min, max, ToU32(colors::Border), 4.f);
			drawList->AddText(Add(min, padding), ToU32(colors::TextPrimary), text);
		}
	} // namespace

	void UiCanvasPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("UI Canvas", VisiblePtr());

		World& world = context.Get<World>();
		auto* selection = context.TryGet<SceneSelection>();
		Entity canvas = ActiveCanvas(world, selection);

		auto* imgui = context.TryGet<aether::ImguiSubsystem>();
		auto* assets = context.TryGet<aether::AssetManager>();
		aether::TextureRegistry* textures = assets != nullptr ? &assets->GetTextureRegistry() : nullptr;
		auto resolveTexId = [&](const UiElement& element) -> ImTextureID
		{
			if (imgui == nullptr || textures == nullptr)
			{
				return ImTextureID_Invalid;
			}
			const auto* image = world.TryGet<ui::UIImage>(element.entity);
			if (image == nullptr || image->texturePath.empty())
			{
				return ImTextureID_Invalid;
			}
			if (auto it = m_texturePreviews.find(image->texturePath); it != m_texturePreviews.end())
			{
				return it->second;
			}
			// Mirror the file-explorer preview path: resolve the bindless slot, skip the
			// fallback (magenta) slot so a not-yet-loaded texture just retries next frame,
			// then pull the sampled view from the debug-texture registry (GetView() is the
			// wrong view for an ImGui descriptor and samples white).
			const TextureHandle handle = textures->Acquire(image->texturePath);
			const std::uint32_t slot = textures->ResolveSlot(handle);
			const std::uint32_t fallbackSlot = textures->ResolveSlot(textures->DefaultHandle());
			if (!handle.IsValid() || slot == 0xFFFFFFFFu || slot == fallbackSlot)
			{
				return ImTextureID_Invalid;
			}
			for (const gpu::DebugTextureInfo& info: gpu::ResourceRegistry::ListDebugTextures())
			{
				if (info.hasBindlessSampled && info.bindlessSampledSlot == slot && info.view != nullptr)
				{
					const ImTextureID id = imgui->RegisterTexture(info.view, gpu::ImageLayout::ShaderReadOnly);
					if (id != ImTextureID_Invalid)
					{
						m_texturePreviews.emplace(image->texturePath, id);
						return id;
					}
					break;
				}
			}
			return ImTextureID_Invalid;
		};

		if (ImGui::Button(ICON_FA_PLUS "  Add Element"))
		{
			ImGui::OpenPopup("##AddElementPopup");
		}

		if (ImGui::BeginPopup("##AddElementPopup"))
		{
			if (ImGui::MenuItem(ICON_FA_IMAGE " Image"))
			{
				canvas = canvas.IsValid() ? canvas : ui::CreateCanvasEntity(world);
				const Entity parent = CreateParentForNewElement(world, selection, canvas);
				const Entity created = ui::CreateImageEntity(world, canvas);
				if (parent != canvas)
				{
					ecs::SetParent(world, created, parent);
				}
				if (selection != nullptr)
				{
					selection->Select(created);
				}
			}
			if (ImGui::MenuItem(ICON_FA_CODE " Text"))
			{
				canvas = canvas.IsValid() ? canvas : ui::CreateCanvasEntity(world);
				const Entity parent = CreateParentForNewElement(world, selection, canvas);
				const Entity created = ui::CreateTextEntity(world, canvas);
				if (parent != canvas)
				{
					ecs::SetParent(world, created, parent);
				}
				if (selection != nullptr)
				{
					selection->Select(created);
				}
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_FA_SITEMAP " Canvas (Root)"))
			{
				const Entity created = ui::CreateCanvasEntity(world);
				if (selection != nullptr)
				{
					selection->Select(created);
				}
				canvas = created;
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::Checkbox("Preview", &m_previewContent);

		ImGui::SetNextItemWidth(96.f);
		ImGui::SameLine();
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
		ImGui::SameLine();
		ImGui::Checkbox("Snap", &m_snappingEnabled);
		ImGui::SetItemTooltip("Toggle snapping (Hold Shift to temporarily invert)");

		const ImVec2 contentMin = ImGui::GetCursorScreenPos();
		ImVec2 canvasArea = ImGui::GetContentRegionAvail();
		canvasArea.x = std::max(canvasArea.x, 1.f);
		canvasArea.y = std::max(canvasArea.y, 1.f);
		const ImVec2 contentMax = Add(contentMin, canvasArea);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(contentMin, contentMax, ToU32(colors::Surface));
		ImGui::InvisibleButton("##ui-canvas-surface", canvasArea, ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonLeft);
		const bool surfaceHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		const bool surfaceActive = ImGui::IsItemActive();

		if (!canvas.IsValid())
		{
			ImGui::End();
			return;
		}

		glm::vec2 extent = CanvasExtent(world, canvas);

		auto canvasSizeForZoom = [&](float zoom)
		{
			return ImVec2{extent.x * zoom, extent.y * zoom};
		};
		auto originForZoom = [&](float zoom)
		{
			const ImVec2 size = canvasSizeForZoom(zoom);
			return ImVec2{contentMin.x + (canvasArea.x - size.x) * 0.5f, contentMin.y + (canvasArea.y - size.y) * 0.5f};
		};

		ImVec2 canvasSize = canvasSizeForZoom(m_zoom);
		ImVec2 origin = originForZoom(m_zoom);

		const ImGuiIO& io = ImGui::GetIO();
		if (surfaceHovered && io.MouseWheel != 0.f && m_drag.kind == DragKind::None)
		{
			const ImVec2 mouse = ImGui::GetMousePos();
			const glm::vec2 anchor = ScreenToCanvas(mouse, origin, m_pan, m_zoom);
			const float zoomScale = std::pow(1.12f, io.MouseWheel);
			m_zoom = std::clamp(m_zoom * zoomScale, kMinZoom, kMaxZoom);
			canvasSize = canvasSizeForZoom(m_zoom);
			origin = originForZoom(m_zoom);
			m_pan = Sub(mouse, Add(origin, ImVec2{anchor.x * m_zoom, anchor.y * m_zoom}));
		}

		const ImVec2 canvasMin = Add(origin, m_pan);
		const ImVec2 canvasMax = Add(canvasMin, canvasSize);

		if ((surfaceHovered || surfaceActive) && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f) && m_drag.kind == DragKind::None)
		{
			m_pan = Add(m_pan, io.MouseDelta);
		}
		if (surfaceHovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}

		drawList->PushClipRect(contentMin, contentMax, true);

		std::vector<UiElement> elements;
		AppendElementsPreOrder(world, canvas, canvas, extent, origin, m_pan, m_zoom, elements);

		if (m_previewContent)
		{
			drawList->AddRectFilled(canvasMin, canvasMax, ToU32(colors::Background));
			for (const UiElement& element: elements)
			{
				DrawPreviewElement(drawList, world, element, m_zoom, resolveTexId(element));
			}
		}
		DrawGrid(drawList, canvasMin, canvasMax, origin, m_pan, m_zoom, extent);

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

		m_hoveredEntity = {};
		if (surfaceHovered && m_drag.kind == DragKind::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			for (auto& element: std::views::reverse(elements))
			{
				if (Contains(io.MousePos, element.min, element.max))
				{
					m_hoveredEntity = element.entity;
					break;
				}
			}
		}

		for (const UiElement& element: elements)
		{
			const bool isSelected = selection != nullptr && selection->Contains(element.entity);
			const bool isHovered = element.entity == m_hoveredEntity;
			const ImU32 outline = isSelected ? ToU32(colors::Primary) : (isHovered ? ToU32(colors::Info) : ToU32(colors::Neutral));
			const ImU32 fill = isSelected ? ToU32(colors::detail::rgba(255, 124, 50, 0.10f)) : (isHovered ? ToU32(colors::detail::rgba(56, 189, 255, 0.06f)) : ToU32(colors::detail::rgba(226, 214, 196, 0.05f)));
			drawList->AddRectFilled(element.min, element.max, fill);
			drawList->AddRect(element.min, element.max, outline, 0.f, 0, isSelected ? 2.f : 1.5f);
		}

		if (m_hoveredEntity.IsValid() && m_drag.kind == DragKind::None)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		}
		else if (surfaceHovered && m_drag.kind == DragKind::None && Contains(io.MousePos, canvasMin, canvasMax))
		{
			const float bt = 8.f;
			const bool cl = std::abs(io.MousePos.x - canvasMin.x) < bt;
			const bool cr = std::abs(io.MousePos.x - canvasMax.x) < bt;
			const bool ct = std::abs(io.MousePos.y - canvasMin.y) < bt;
			const bool cb = std::abs(io.MousePos.y - canvasMax.y) < bt;
			if (cl && ct)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
			}
			else if (cr && ct)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
			}
			else if (cl && cb)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
			}
			else if (cr && cb)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
			}
			else if (cl || cr)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
			}
			else if (ct || cb)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
			}
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

			const ImVec2 selectedVisualCenter{(selectedMin.x + selectedMax.x) * 0.5f, (selectedMin.y + selectedMax.y) * 0.5f};
			const ImU32 anchorColor = ToU32(colors::Info);
			const ImU32 anchorLineColor = ToU32(colors::detail::rgba(56, 189, 255, 0.4f));
			for (const AnchorHit& hit: AnchorHandles(*selectedRect, selectedParentRect, origin, m_pan, m_zoom))
			{
				const ImVec2 half{kAnchorHandleSize * 0.5f, kAnchorHandleSize * 0.5f};
				DrawDashedLine(drawList, hit.center, selectedVisualCenter, anchorLineColor);
				drawList->AddRect(Sub(hit.center, half), Add(hit.center, half), anchorColor, 0.f, ImDrawFlags_RoundCornersAll, 2.f);
				drawList->AddLine({hit.center.x - half.x, hit.center.y}, {hit.center.x + half.x, hit.center.y}, anchorColor);
				drawList->AddLine({hit.center.x, hit.center.y - half.y}, {hit.center.x, hit.center.y + half.y}, anchorColor);
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
			{
				UiRectResizeHandle hoverHandle = UiRectResizeHandle::BottomRight;
				if (m_drag.kind == DragKind::Resize && m_drag.entity == selected)
				{
					ImGui::SetMouseCursor(CursorForResizeHandle(m_drag.resize));
				}
				else if (surfaceHovered && TryHitResizeHandle(ImGui::GetMousePos(), selectedMin, selectedMax, hoverHandle))
				{
					ImGui::SetMouseCursor(CursorForResizeHandle(hoverHandle));
				}
			}
		}

		if (m_drag.kind == DragKind::None && selectedRect != nullptr)
		{
			const float nudgeAmount = io.KeyShift ? 10.f : 1.f;
			glm::vec2 nudgeDelta{0.f};
			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			{
				nudgeDelta.x -= nudgeAmount;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			{
				nudgeDelta.x += nudgeAmount;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			{
				nudgeDelta.y -= nudgeAmount;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			{
				nudgeDelta.y += nudgeAmount;
			}

			if (nudgeDelta.x != 0.f || nudgeDelta.y != 0.f)
			{
				selectedRect->offsetMin += nudgeDelta;
				selectedRect->offsetMax += nudgeDelta;
			}

			if (ImGui::IsKeyPressed(ImGuiKey_F))
			{
				const glm::vec4 visualRect = ui::ResolveRect(selectedParentRect, *selectedRect);
				const float zoomX = (canvasArea.x * 0.8f) / std::max(visualRect.z, 1.f);
				const float zoomY = (canvasArea.y * 0.8f) / std::max(visualRect.w, 1.f);
				m_zoom = std::clamp(std::min(zoomX, zoomY), kMinZoom, kMaxZoom);
				canvasSize = canvasSizeForZoom(m_zoom);
				origin = originForZoom(m_zoom);
				const ImVec2 targetScreen = CanvasToScreen({visualRect.x + visualRect.z * 0.5f, visualRect.y + visualRect.w * 0.5f}, origin, ImVec2{0.f, 0.f}, m_zoom);
				const ImVec2 screenCenter = Add(contentMin, ImVec2{canvasArea.x * 0.5f, canvasArea.y * 0.5f});
				m_pan = Sub(screenCenter, targetScreen);
			}
		}

		std::vector<SnapGuide> snapGuides;
		std::vector<GapGuide> gapGuides;
		if (surfaceHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			DragState newDrag{};
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
						newDrag.kind = DragKind::Anchor;
						newDrag.entity = selected;
						newDrag.anchor = hit.handle;
						break;
					}
				}
				if (newDrag.kind == DragKind::None)
				{
					UiRectResizeHandle hitHandle = UiRectResizeHandle::BottomRight;
					if (TryHitResizeHandle(mouse, selectedMin, selectedMax, hitHandle))
					{
						newDrag.kind = DragKind::Resize;
						newDrag.entity = selected;
						newDrag.resize = hitHandle;
					}
				}
			}
			if (newDrag.kind == DragKind::None)
			{
				for (auto& element: std::views::reverse(elements))
				{
					if (Contains(mouse, element.min, element.max))
					{
						if (selection != nullptr)
						{
							if (!selection->Contains(element.entity))
							{
								if (io.KeyShift)
								{
									selection->AddToSelection(element.entity);
								}
								else
								{
									selection->Select(element.entity);
								}
							}
						}
						newDrag.kind = DragKind::Move;
						newDrag.entity = element.entity;
						break;
					}
				}
			}
			if (newDrag.kind == DragKind::None && Contains(mouse, canvasMin, canvasMax))
			{
				const float borderThreshold = 8.f;
				const bool onLeft = std::abs(mouse.x - canvasMin.x) < borderThreshold;
				const bool onRight = std::abs(mouse.x - canvasMax.x) < borderThreshold;
				const bool onTop = std::abs(mouse.y - canvasMin.y) < borderThreshold;
				const bool onBottom = std::abs(mouse.y - canvasMax.y) < borderThreshold;

				if (onLeft && onTop)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::TopLeft;
				}
				else if (onRight && onTop)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::TopRight;
				}
				else if (onLeft && onBottom)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::BottomLeft;
				}
				else if (onRight && onBottom)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::BottomRight;
				}
				else if (onLeft)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::Left;
				}
				else if (onRight)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::Right;
				}
				else if (onTop)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::Top;
				}
				else if (onBottom)
				{
					newDrag.kind = DragKind::CanvasResize;
					newDrag.canvasResizeHandle = UiRectResizeHandle::Bottom;
				}

				if (newDrag.kind == DragKind::CanvasResize)
				{
					newDrag.entity = canvas;
					newDrag.startMouseCanvas = ScreenToCanvas(mouse, origin, m_pan, m_zoom);
					if (auto* canvasComp = world.TryGet<ui::UICanvas>(canvas))
					{
						newDrag.startOffsetMin = {canvasComp->referenceResolution.x, canvasComp->referenceResolution.y};
					}
				}
				else
				{
					if (!io.KeyShift && selection != nullptr)
					{
						selection->Clear();
					}
					newDrag.kind = DragKind::Marquee;
					m_marqueeStart = mouse;
					m_marqueeEnd = mouse;
				}
			}

			if (newDrag.kind == DragKind::Move || newDrag.kind == DragKind::Resize)
			{
				if (auto* rect = world.TryGet<ui::UIRect>(newDrag.entity))
				{
					newDrag.startMouseCanvas = ScreenToCanvas(mouse, origin, m_pan, m_zoom);
					newDrag.startOffsetMin = rect->offsetMin;
					newDrag.startOffsetMax = rect->offsetMax;
				}
			}

			if (newDrag.kind == DragKind::Move && selection != nullptr)
			{
				m_multiDragOrigins.clear();
				for (const Entity e: selection->All())
				{
					if (e == newDrag.entity)
					{
						continue;
					}
					if (auto* r = world.TryGet<ui::UIRect>(e))
					{
						m_multiDragOrigins.push_back({e, r->offsetMin, r->offsetMax});
					}
				}
			}

			m_drag = newDrag;
		}

		if (m_drag.kind != DragKind::None)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				if (auto* r = world.TryGet<ui::UIRect>(m_drag.entity))
				{
					r->offsetMin = m_drag.startOffsetMin;
					r->offsetMax = m_drag.startOffsetMax;
				}
				for (const auto& dragOrigin: m_multiDragOrigins)
				{
					if (auto* r = world.TryGet<ui::UIRect>(dragOrigin.entity))
					{
						r->offsetMin = dragOrigin.startOffsetMin;
						r->offsetMax = dragOrigin.startOffsetMax;
					}
				}
				m_multiDragOrigins.clear();
				m_drag = {};
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || (m_drag.kind != DragKind::CanvasResize && !IsAlive(world, m_drag.entity)))
			{
				if (m_drag.kind != DragKind::Marquee)
				{
					m_multiDragOrigins.clear();
					m_drag = {};
				}
			}

			if (m_drag.kind == DragKind::CanvasResize)
			{
				if (auto* canvasComp = world.TryGet<ui::UICanvas>(canvas))
				{
					const ImVec2 mousePos = ImGui::GetMousePos();
					const glm::vec2 currentMouseCanvas = ScreenToCanvas(mousePos, origin, m_pan, m_zoom);
					const glm::vec2 delta = currentMouseCanvas - m_drag.startMouseCanvas;
					glm::vec2 newRes = m_drag.startOffsetMin;

					if (m_drag.canvasResizeHandle == UiRectResizeHandle::Right || m_drag.canvasResizeHandle == UiRectResizeHandle::TopRight || m_drag.canvasResizeHandle == UiRectResizeHandle::BottomRight)
					{
						newRes.x += delta.x;
					}
					if (m_drag.canvasResizeHandle == UiRectResizeHandle::Bottom || m_drag.canvasResizeHandle == UiRectResizeHandle::BottomLeft || m_drag.canvasResizeHandle == UiRectResizeHandle::BottomRight)
					{
						newRes.y += delta.y;
					}

					canvasComp->referenceResolution = glm::max(newRes, glm::vec2(100.f));

					char tooltipBuf[64];
					std::snprintf(tooltipBuf, sizeof(tooltipBuf), "Res: %.0f x %.0f", canvasComp->referenceResolution.x, canvasComp->referenceResolution.y);
					DrawTooltip(drawList, mousePos, tooltipBuf);
				}
			}
			else if (auto* rect = world.TryGet<ui::UIRect>(m_drag.entity))
			{
				const ImVec2 mousePos = ImGui::GetMousePos();
				const glm::vec2 currentMouseCanvas = ScreenToCanvas(mousePos, origin, m_pan, m_zoom);
				const glm::vec2 delta = currentMouseCanvas - m_drag.startMouseCanvas;
				const bool shouldSnap = m_snappingEnabled != io.KeyShift;

				if (m_drag.kind == DragKind::Move)
				{
					rect->offsetMin = m_drag.startOffsetMin + delta;
					rect->offsetMax = m_drag.startOffsetMax + delta;

					const glm::vec4 parentRect = ResolveParentRect(world, m_drag.entity, canvas, extent);

					const glm::vec2 preSnapMin = rect->offsetMin;
					if (shouldSnap)
					{
						ApplyMoveSnap(world, *rect, m_drag.entity, elements, extent, parentRect, m_zoom, snapGuides);
					}

					const glm::vec2 snapDelta = shouldSnap ? (rect->offsetMin - preSnapMin) : glm::vec2{0.f, 0.f};

					for (const auto& dragOrigin: m_multiDragOrigins)
					{
						if (auto* r = world.TryGet<ui::UIRect>(dragOrigin.entity))
						{
							r->offsetMin = dragOrigin.startOffsetMin + delta + snapDelta;
							r->offsetMax = dragOrigin.startOffsetMax + delta + snapDelta;
						}
					}

					BuildGapGuides(world, ui::ResolveRect(parentRect, *rect), m_drag.entity, elements, parentRect, gapGuides);

					const glm::vec4 vis = ui::ResolveRect(parentRect, *rect);
					char dimBuf[64];
					std::snprintf(dimBuf, sizeof(dimBuf), "X: %.0f  Y: %.0f", vis.x, vis.y);
					DrawTooltip(drawList, mousePos, dimBuf);
				}
				else if (m_drag.kind == DragKind::Resize)
				{
					glm::vec2 targetMin = m_drag.startOffsetMin;
					glm::vec2 targetMax = m_drag.startOffsetMax;

					const bool left = m_drag.resize == UiRectResizeHandle::Left || m_drag.resize == UiRectResizeHandle::TopLeft || m_drag.resize == UiRectResizeHandle::BottomLeft;
					const bool right = m_drag.resize == UiRectResizeHandle::Right || m_drag.resize == UiRectResizeHandle::TopRight || m_drag.resize == UiRectResizeHandle::BottomRight;
					const bool top = m_drag.resize == UiRectResizeHandle::Top || m_drag.resize == UiRectResizeHandle::TopLeft || m_drag.resize == UiRectResizeHandle::TopRight;
					const bool bottom = m_drag.resize == UiRectResizeHandle::Bottom || m_drag.resize == UiRectResizeHandle::BottomLeft || m_drag.resize == UiRectResizeHandle::BottomRight;

					if (left)
					{
						targetMin.x += delta.x;
					}
					if (right)
					{
						targetMax.x += delta.x;
					}
					if (top)
					{
						targetMin.y += delta.y;
					}
					if (bottom)
					{
						targetMax.y += delta.y;
					}

					const glm::vec4 parentRect = ResolveParentRect(world, m_drag.entity, canvas, extent);
					const glm::vec2 parentExtent{parentRect.z, parentRect.w};
					const glm::vec2 anchorSpan = (rect->anchorMax - rect->anchorMin) * glm::max(parentExtent, glm::vec2(1.f));
					constexpr float minSize = 1.f;

					const float width = anchorSpan.x + targetMax.x - targetMin.x;
					if (width < minSize)
					{
						if (left && !right)
						{
							targetMin.x = targetMax.x + anchorSpan.x - minSize;
						}
						else
						{
							targetMax.x = targetMin.x - anchorSpan.x + minSize;
						}
					}
					const float height = anchorSpan.y + targetMax.y - targetMin.y;
					if (height < minSize)
					{
						if (top && !bottom)
						{
							targetMin.y = targetMax.y + anchorSpan.y - minSize;
						}
						else
						{
							targetMax.y = targetMin.y - anchorSpan.y + minSize;
						}
					}

					rect->offsetMin = targetMin;
					rect->offsetMax = targetMax;

					if (shouldSnap)
					{
						ApplyResizeSnap(world, *rect, m_drag.resize, m_drag.entity, elements, extent, parentRect, m_zoom, snapGuides);
					}
					BuildGapGuides(world, ui::ResolveRect(parentRect, *rect), m_drag.entity, elements, parentRect, gapGuides);

					const glm::vec4 vis = ui::ResolveRect(parentRect, *rect);
					char dimBuf[64];
					std::snprintf(dimBuf, sizeof(dimBuf), "%.0f x %.0f", vis.z, vis.w);
					DrawTooltip(drawList, mousePos, dimBuf);
				}
				else if (m_drag.kind == DragKind::Anchor)
				{
					const glm::vec4 parentRect = ResolveParentRect(world, m_drag.entity, canvas, extent);
					const glm::vec4 visualRect = ui::ResolveRect(parentRect, *rect);
					MoveAnchor(*rect, m_drag.anchor, currentMouseCanvas, parentRect, visualRect, shouldSnap);
				}
			}
		}

		if (m_drag.kind == DragKind::Marquee)
		{
			m_marqueeEnd = ImGui::GetMousePos();

			const ImVec2 mMin = ImVec2{std::min(m_marqueeStart.x, m_marqueeEnd.x), std::min(m_marqueeStart.y, m_marqueeEnd.y)};
			const ImVec2 mMax = ImVec2{std::max(m_marqueeStart.x, m_marqueeEnd.x), std::max(m_marqueeStart.y, m_marqueeEnd.y)};

			drawList->AddRectFilled(mMin, mMax, ToU32(colors::detail::rgba(56, 189, 255, 0.15f)));
			drawList->AddRect(mMin, mMax, ToU32(colors::Primary), 0.f, 0, 1.f);

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				if (selection != nullptr)
				{
					for (const UiElement& el: elements)
					{
						if (RectsOverlap(el.min, el.max, mMin, mMax))
						{
							selection->AddToSelection(el.entity);
						}
					}
				}
				m_multiDragOrigins.clear();
				m_drag = {};
			}
		}

		DrawSnapGuides(drawList, snapGuides, origin, m_pan, m_zoom, extent);
		DrawGapGuides(drawList, gapGuides, origin, m_pan, m_zoom);

		if (surfaceHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			if (m_hoveredEntity.IsValid())
			{
				if (selection != nullptr)
				{
					if (io.KeyShift)
					{
						selection->AddToSelection(m_hoveredEntity);
					}
					else
					{
						selection->Select(m_hoveredEntity);
					}
				}
				ImGui::OpenPopup("##ElementContext");
			}
			else
			{
				ImGui::OpenPopup("##CanvasContext");
			}
		}

		if (ImGui::BeginPopup("##CanvasContext"))
		{
			if (ImGui::MenuItem(ICON_FA_IMAGE " Add Image"))
			{
				canvas = canvas.IsValid() ? canvas : ui::CreateCanvasEntity(world);
				const Entity parent = CreateParentForNewElement(world, selection, canvas);
				const Entity created = ui::CreateImageEntity(world, canvas);
				if (parent != canvas)
				{
					ecs::SetParent(world, created, parent);
				}
				if (selection != nullptr)
				{
					selection->Select(created);
				}
			}
			if (ImGui::MenuItem(ICON_FA_CODE " Add Text"))
			{
				canvas = canvas.IsValid() ? canvas : ui::CreateCanvasEntity(world);
				const Entity parent = CreateParentForNewElement(world, selection, canvas);
				const Entity created = ui::CreateTextEntity(world, canvas);
				if (parent != canvas)
				{
					ecs::SetParent(world, created, parent);
				}
				if (selection != nullptr)
				{
					selection->Select(created);
				}
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reset View"))
			{
				m_pan = {0.f, 0.f};
				m_zoom = 1.f;
			}
			if (ImGui::MenuItem("Zoom to Fit (F)"))
			{
				if (selectedRect != nullptr)
				{
					const glm::vec4 visualRect = ui::ResolveRect(selectedParentRect, *selectedRect);
					const float zoomX = (canvasArea.x * 0.8f) / std::max(visualRect.z, 1.f);
					const float zoomY = (canvasArea.y * 0.8f) / std::max(visualRect.w, 1.f);
					m_zoom = std::clamp(std::min(zoomX, zoomY), kMinZoom, kMaxZoom);
					canvasSize = canvasSizeForZoom(m_zoom);
					origin = originForZoom(m_zoom);
					const ImVec2 targetScreen = CanvasToScreen({visualRect.x + visualRect.z * 0.5f, visualRect.y + visualRect.w * 0.5f}, origin, ImVec2{0.f, 0.f}, m_zoom);
					const ImVec2 screenCenter = Add(contentMin, ImVec2{canvasArea.x * 0.5f, canvasArea.y * 0.5f});
					m_pan = Sub(screenCenter, targetScreen);
				}
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopup("##ElementContext"))
		{
			if (ImGui::MenuItem("Delete", "Del"))
			{
				if (selection != nullptr)
				{
					selection->Clear();
				}
				world.Destroy(m_hoveredEntity);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Zoom to Fit (F)"))
			{
				if (selectedRect != nullptr)
				{
					const glm::vec4 visualRect = ui::ResolveRect(selectedParentRect, *selectedRect);
					const float zoomX = (canvasArea.x * 0.8f) / std::max(visualRect.z, 1.f);
					const float zoomY = (canvasArea.y * 0.8f) / std::max(visualRect.w, 1.f);
					m_zoom = std::clamp(std::min(zoomX, zoomY), kMinZoom, kMaxZoom);
					canvasSize = canvasSizeForZoom(m_zoom);
					origin = originForZoom(m_zoom);
					const ImVec2 targetScreen = CanvasToScreen({visualRect.x + visualRect.z * 0.5f, visualRect.y + visualRect.w * 0.5f}, origin, ImVec2{0.f, 0.f}, m_zoom);
					const ImVec2 screenCenter = Add(contentMin, ImVec2{canvasArea.x * 0.5f, canvasArea.y * 0.5f});
					m_pan = Sub(screenCenter, targetScreen);
				}
			}
			ImGui::EndPopup();
		}

		drawList->PopClipRect();
		ImGui::End();
	}
} // namespace aether::editor
