#pragma once

#include <glm/glm.hpp>

#include "Entity.hpp"

namespace aether::ui
{
	// Per-frame UI interaction state, populated by UiSystem::BeginFrame() and
	// read by widgets during OnGui to determine hover/press/drag behaviour.
	struct UiContext
	{
		// Hit-test results (reset each BeginFrame).
		Entity hotEntity;     // entity whose rect contains the cursor (highest z wins)
		Entity activeEntity;  // entity being held / dragged (persists across frames)
		Entity focusedEntity; // entity that has keyboard focus

		// Mouse state for this frame.
		glm::vec2 mousePos{};
		glm::vec2 mouseDelta{};
		bool mousePressed = false;  // left button just went down
		bool mouseDown = false;     // left button held
		bool mouseReleased = false; // left button just went up

		// Drag state (for draggable panels).
		Entity draggedEntity;
		glm::vec2 dragStartMousePos{};
		glm::vec2 dragStartRectMin{}; // rect.offsetMinPx when drag began
		glm::vec2 dragStartRectMax{}; // rect.offsetMaxPx when drag began
		bool isDragging = false;
	};

} // namespace aether::ui
