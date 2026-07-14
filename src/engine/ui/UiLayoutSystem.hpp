#pragma once

#include <glm/glm.hpp>

#include "ui/UiComponents.hpp"

namespace aether
{
	class World;
}

namespace aether::ui
{
	[[nodiscard]] glm::vec4 ResolveRect(const glm::vec4& parentRect, const UIRect& rect);

	// layout).
	void ResolveCanvases(World& world, glm::vec2 outputExtent);
} // namespace aether::ui
