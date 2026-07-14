#include "ui/UiLayoutSystem.hpp"

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::ui
{
	glm::vec4 ResolveRect(const glm::vec4& parentRect, const UIRect& rect)
	{
		const glm::vec2 parentMin{parentRect.x, parentRect.y};
		const glm::vec2 parentSize{parentRect.z, parentRect.w};

		const glm::vec2 anchorMinPx = parentMin + rect.anchorMin * parentSize;
		const glm::vec2 anchorMaxPx = parentMin + rect.anchorMax * parentSize;

		const glm::vec2 rectMin = anchorMinPx + rect.offsetMin;
		const glm::vec2 rectMax = anchorMaxPx + rect.offsetMax;
		const glm::vec2 size = glm::max(rectMax - rectMin, glm::vec2(0.f));

		return {rectMin.x, rectMin.y, size.x, size.y};
	}

	static void ResolveSubtree(World& world, Entity entity, const glm::vec4& parentRect)
	{
		glm::vec4 selfRect = parentRect;
		if (auto* rect = world.TryGet<UIRect>(entity))
		{
			rect->resolvedRect = ResolveRect(parentRect, *rect);
			selfRect = rect->resolvedRect;
		}

		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
		{
			for (const Entity child: hierarchy->children)
			{
				ResolveSubtree(world, child, selfRect);
			}
		}
	}

	void ResolveCanvases(World& world, glm::vec2 outputExtent)
	{
		const glm::vec4 screenRect{0.f, 0.f, outputExtent.x, outputExtent.y};
		world.View<UICanvas, UIRect>().each(
		        [&](entt::entity canvasEntity, UICanvas&, UIRect& rect)
		        {
			        rect.resolvedRect = screenRect;

			        const Entity entity = World::FromEntt(canvasEntity);
			        if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			        {
				        for (const Entity child: hierarchy->children)
				        {
					        ResolveSubtree(world, child, screenRect);
				        }
			        }
		        });
	}
} // namespace aether::ui
