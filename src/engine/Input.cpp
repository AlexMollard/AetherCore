#include "Input.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace aether
{
	Input::~Input()
	{
		if (m_window)
		{
			// Remove our user pointer and scroll callback so GLFW doesn't call
			// back into a destroyed object if the window outlives this instance.
			glfwSetScrollCallback(m_window, nullptr);
			glfwSetWindowUserPointer(m_window, nullptr);
		}
	}

	void Input::Init(GLFWwindow* window)
	{
		m_window = window;
		glfwSetWindowUserPointer(window, this);
		glfwSetScrollCallback(window, &Input::OnScroll);
	}

	void Input::Update()
	{
		// Carry current state into previous before polling the new state.
		m_prevKeys = m_currKeys;
		m_prevMouseButtons = m_currMouseButtons;

		// Snapshot keyboard - glfwGetKey returns GLFW_RELEASE for any
		// key code that is not currently pressed (including unmapped codes).
		for (int i = 0; i < kMaxKeys; ++i)
		{
			m_currKeys[i] = (glfwGetKey(m_window, i) == GLFW_PRESS);
		}

		// Snapshot mouse buttons.
		for (int i = 0; i < kMaxMouseButtons; ++i)
		{
			m_currMouseButtons[i] = (glfwGetMouseButton(m_window, i) == GLFW_PRESS);
		}

		// Capture cursor position.
		m_prevMousePos = m_mousePos;
		double cx = 0.0, cy = 0.0;
		glfwGetCursorPos(m_window, &cx, &cy);
		m_mousePos = { static_cast<float>(cx), static_cast<float>(cy) };

		// On the very first update there is no meaningful "previous" position,
		// so prevent a large spurious delta by seeding prev = current.
		if (m_firstUpdate)
		{
			m_prevMousePos = m_mousePos;
			m_firstUpdate = false;
		}

		// Expose the scroll that was accumulated by the GLFW callback since last
		// frame, then clear the accumulator for the next frame.
		m_scrollDelta = m_pendingScroll;
		m_pendingScroll = {};
	}

	// ── Keyboard ─────────────────────────────────────────────────────────────

	bool Input::IsKeyDown(Key key) const
	{
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kMaxKeys)
		{
			return false;
		}
		return m_currKeys[k];
	}

	bool Input::IsKeyPressed(Key key) const
	{
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kMaxKeys)
		{
			return false;
		}
		return m_currKeys[k] && !m_prevKeys[k];
	}

	bool Input::IsKeyReleased(Key key) const
	{
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kMaxKeys)
		{
			return false;
		}
		return !m_currKeys[k] && m_prevKeys[k];
	}

	// ── Mouse buttons ─────────────────────────────────────────────────────────

	bool Input::IsMouseButtonDown(MouseButton btn) const
	{
		const int b = static_cast<int>(btn);
		if (b < 0 || b >= kMaxMouseButtons)
		{
			return false;
		}
		return m_currMouseButtons[b];
	}

	bool Input::IsMouseButtonPressed(MouseButton btn) const
	{
		const int b = static_cast<int>(btn);
		if (b < 0 || b >= kMaxMouseButtons)
		{
			return false;
		}
		return m_currMouseButtons[b] && !m_prevMouseButtons[b];
	}

	bool Input::IsMouseButtonReleased(MouseButton btn) const
	{
		const int b = static_cast<int>(btn);
		if (b < 0 || b >= kMaxMouseButtons)
		{
			return false;
		}
		return !m_currMouseButtons[b] && m_prevMouseButtons[b];
	}

	// ── Mouse cursor ──────────────────────────────────────────────────────────

	glm::vec2 Input::GetMousePos() const
	{
		return m_mousePos;
	}

	glm::vec2 Input::GetMouseDelta() const
	{
		return m_mousePos - m_prevMousePos;
	}

	glm::vec2 Input::GetScrollDelta() const
	{
		return m_scrollDelta;
	}

	// ── GLFW scroll callback ──────────────────────────────────────────────────

	void Input::OnScroll(GLFWwindow* window, double xOffset, double yOffset)
	{
		auto* self = static_cast<Input*>(glfwGetWindowUserPointer(window));
		if (self)
		{
			self->m_pendingScroll += glm::vec2{ static_cast<float>(xOffset), static_cast<float>(yOffset) };
		}
	}
} // namespace aether
