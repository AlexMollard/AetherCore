#include "platform/Input.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>

#include "utils/Profiler.hpp"

namespace aether
{
	Input::~Input()
	{
		if (m_window)
		{
			// Remove our user pointer and scroll callback so GLFW doesn't call
			// back into a destroyed object if the window outlives this instance.
			glfwSetScrollCallback(m_window, nullptr);
			glfwSetCharCallback(m_window, nullptr);
			glfwSetWindowUserPointer(m_window, nullptr);
		}
	}

	void Input::Init(GLFWwindow* window)
	{
		AE_PROFILE_ZONE();
		m_window = window;
		glfwSetWindowUserPointer(window, this);
		glfwSetScrollCallback(window, &Input::OnScroll);
		glfwSetCharCallback(window, &Input::OnChar);
	}

	void Input::Update()
	{
		AE_PROFILE_ZONE();
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
		m_mousePos = {static_cast<float>(cx), static_cast<float>(cy)};

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

		// Expose typed characters and reset the accumulator.
		m_typedChars = std::move(m_pendingChars);
		m_pendingChars.clear();
	}

	// -- Keyboard -------------------------------------------------------------

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

	// -- Mouse buttons ---------------------------------------------------------

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

	// -- Mouse cursor ----------------------------------------------------------

	glm::vec2 Input::GetMousePos() const
	{
		return TransformMousePos(m_mousePos);
	}

	glm::vec2 Input::GetMouseDelta() const
	{
		const glm::vec2 current = TransformMousePos(m_mousePos);
		const glm::vec2 previous = TransformMousePos(m_prevMousePos);
		if (current.x < -999999.0f || previous.x < -999999.0f)
		{
			return {};
		}
		return current - previous;
	}

	glm::vec2 Input::GetScrollDelta() const
	{
		return m_scrollDelta;
	}

	const std::string& Input::GetTypedChars() const
	{
		return m_typedChars;
	}

	void Input::SetMouseViewportTransform(glm::vec2 viewportMin, glm::vec2 viewportSize, glm::vec2 targetSize)
	{
		m_mouseViewportTransformActive = viewportSize.x > 0.0f && viewportSize.y > 0.0f && targetSize.x > 0.0f && targetSize.y > 0.0f;
		m_mouseViewportMin = viewportMin;
		m_mouseViewportSize = viewportSize;
		m_mouseViewportTargetSize = targetSize;
	}

	void Input::ClearMouseViewportTransform()
	{
		m_mouseViewportTransformActive = false;
		m_mouseViewportInputActive = false;
		m_mouseViewportMin = {};
		m_mouseViewportSize = {};
		m_mouseViewportTargetSize = {};
	}

	glm::vec2 Input::TransformMousePos(glm::vec2 windowMousePos) const
	{
		if (!m_mouseViewportTransformActive)
		{
			return windowMousePos;
		}

		const glm::vec2 local = windowMousePos - m_mouseViewportMin;
		if (local.x < 0.0f || local.y < 0.0f || local.x >= m_mouseViewportSize.x || local.y >= m_mouseViewportSize.y)
		{
			return {-1000000.0f, -1000000.0f};
		}

		const glm::vec2 uv = local / m_mouseViewportSize;
		return {
		        std::clamp(uv.x, 0.0f, 1.0f) * m_mouseViewportTargetSize.x,
		        std::clamp(uv.y, 0.0f, 1.0f) * m_mouseViewportTargetSize.y,
		};
	}

	// -- GLFW callbacks --------------------------------------------------------

	void Input::OnScroll(GLFWwindow* window, double xOffset, double yOffset)
	{
		auto self = static_cast<Input*>(glfwGetWindowUserPointer(window));
		if (self)
		{
			self->m_pendingScroll += glm::vec2{static_cast<float>(xOffset), static_cast<float>(yOffset)};
		}
	}

	void Input::OnChar(GLFWwindow* window, unsigned int codepoint)
	{
		auto self = static_cast<Input*>(glfwGetWindowUserPointer(window));
		if (!self)
		{
			return;
		}

		// Encode codepoint to UTF-8 and append to the pending buffer.
		if (codepoint < 0x80u)
		{
			self->m_pendingChars += static_cast<char>(codepoint);
		}
		else if (codepoint < 0x800u)
		{
			self->m_pendingChars += static_cast<char>(0xC0u | (codepoint >> 6u));
			self->m_pendingChars += static_cast<char>(0x80u | (codepoint & 0x3Fu));
		}
		else if (codepoint < 0x10000u)
		{
			self->m_pendingChars += static_cast<char>(0xE0u | (codepoint >> 12u));
			self->m_pendingChars += static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
			self->m_pendingChars += static_cast<char>(0x80u | (codepoint & 0x3Fu));
		}
		else
		{
			self->m_pendingChars += static_cast<char>(0xF0u | (codepoint >> 18u));
			self->m_pendingChars += static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu));
			self->m_pendingChars += static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
			self->m_pendingChars += static_cast<char>(0x80u | (codepoint & 0x3Fu));
		}
	}
} // namespace aether
