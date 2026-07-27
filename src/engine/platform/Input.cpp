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

	void Input::PlayInputSequence(std::vector<InputSequenceEvent> events)
	{
		ClearSyntheticKeys();
		m_inputSequence = std::move(events);
		std::sort(m_inputSequence.begin(), m_inputSequence.end(), [](const InputSequenceEvent& a, const InputSequenceEvent& b) { return a.time < b.time; });
		m_inputSequenceNext = 0;
		m_inputSequenceStart = std::chrono::steady_clock::now();
		m_inputSequenceActive = !m_inputSequence.empty();
	}

	void Input::StopInputSequence()
	{
		m_inputSequenceActive = false;
		m_inputSequence.clear();
		m_inputSequenceNext = 0;
		ClearSyntheticKeys();
	}

	void Input::TickInputSequence()
	{
		if (!m_inputSequenceActive)
		{
			return;
		}
		const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_inputSequenceStart).count();
		while (m_inputSequenceNext < m_inputSequence.size() && m_inputSequence[m_inputSequenceNext].time <= elapsed)
		{
			const InputSequenceEvent& ev = m_inputSequence[m_inputSequenceNext];
			++m_inputSequenceNext;
			if (ev.keyCode < 0)
			{
				ClearSyntheticKeys();
			}
			else
			{
				SetSyntheticKey(ev.keyCode, ev.down);
			}
		}
		// Once every event has fired, stop scheduling but leave held keys as-is:
		// an open-ended "hold" should keep holding until an explicit clear/release
		// event, StopInputSequence, or Play-stop (which clears synthetic keys). End
		// a self-contained test with a final "clear" line to release everything.
		if (m_inputSequenceNext >= m_inputSequence.size())
		{
			m_inputSequenceActive = false;
			m_inputSequence.clear();
			m_inputSequenceNext = 0;
		}
	}

	void Input::Update()
	{
		AE_PROFILE_ZONE();
		// Apply any due auto-test sequence events before sampling key state so this
		// frame reflects them (they OR into m_currKeys below via m_syntheticKeys).
		TickInputSequence();
		m_prevKeys = m_currKeys;
		m_prevMouseButtons = m_currMouseButtons;

		for (int i = 0; i < kMaxKeys; ++i)
		{
			// Synthetic keys (control-server injection) OR into the real state, so
			// headless playtests drive the same IsKeyDown/IsKeyPressed paths.
			m_currKeys[i] = (glfwGetKey(m_window, i) == GLFW_PRESS) || m_syntheticKeys[i];
		}

		for (int i = 0; i < kMaxMouseButtons; ++i)
		{
			// Synthetic buttons OR in like synthetic keys, so injected clicks drive the
			// same IsMouseButtonDown/Pressed/Released paths a real click does.
			m_currMouseButtons[i] = (glfwGetMouseButton(m_window, i) == GLFW_PRESS) || m_syntheticMouseButtons[i];
		}

		m_prevMousePos = m_mousePos;
		double cx = 0.0, cy = 0.0;
		glfwGetCursorPos(m_window, &cx, &cy);
		m_mousePos = {static_cast<float>(cx), static_cast<float>(cy)};

		if (m_firstUpdate)
		{
			m_prevMousePos = m_mousePos;
			m_firstUpdate = false;
		}

		m_scrollDelta = m_pendingScroll;
		m_pendingScroll = {};

		m_typedChars = std::move(m_pendingChars);
		m_pendingChars.clear();
	}

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

	glm::vec2 Input::GetMousePos() const
	{
		// An injected cursor wins outright: it is already expressed in the space this
		// returns (see GetMouseTargetSize), and it must work even when the real cursor
		// sits outside the viewport - otherwise a headless test could never aim.
		if (m_hasSyntheticMousePos)
		{
			return m_syntheticMousePos;
		}
		return TransformMousePos(m_mousePos);
	}

	glm::vec2 Input::GetMouseTargetSize() const
	{
		if (m_mouseViewportTransformActive)
		{
			return m_mouseViewportTargetSize;
		}
		if (m_window != nullptr)
		{
			int w = 0;
			int h = 0;
			glfwGetWindowSize(m_window, &w, &h);
			return {static_cast<float>(w), static_cast<float>(h)};
		}
		return {0.0f, 0.0f};
	}

	// Sentinel written by TransformMousePos when the cursor is outside the viewport;
	// no real viewport-local coordinate comes anywhere near this.
	constexpr float kOutsideViewport = -1000000.0f;

	glm::vec2 Input::GetMouseDelta() const
	{
		const glm::vec2 current = TransformMousePos(m_mousePos);
		const glm::vec2 previous = TransformMousePos(m_prevMousePos);
		if (current.x <= kOutsideViewport || previous.x <= kOutsideViewport)
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

	std::string Input::GetClipboardText() const
	{
		if (m_window != nullptr)
		{
			const char* text = glfwGetClipboardString(m_window);
			return text != nullptr ? std::string{text} : std::string{};
		}
		return m_clipboardFallback;
	}

	void Input::SetClipboardText(std::string_view text)
	{
		const std::string owned{text};
		if (m_window != nullptr)
		{
			glfwSetClipboardString(m_window, owned.c_str());
			return;
		}
		m_clipboardFallback = owned;
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
			return {kOutsideViewport, kOutsideViewport};
		}

		const glm::vec2 uv = local / m_mouseViewportSize;
		return {
		        std::clamp(uv.x, 0.0f, 1.0f) * m_mouseViewportTargetSize.x,
		        std::clamp(uv.y, 0.0f, 1.0f) * m_mouseViewportTargetSize.y,
		};
	}

	void Input::OnScroll(GLFWwindow* window, double xOffset, double yOffset)
	{
		auto* self = static_cast<Input*>(glfwGetWindowUserPointer(window));
		if (self)
		{
			self->m_pendingScroll += glm::vec2{static_cast<float>(xOffset), static_cast<float>(yOffset)};
		}
	}

	void Input::OnChar(GLFWwindow* window, unsigned int codepoint)
	{
		auto* self = static_cast<Input*>(glfwGetWindowUserPointer(window));
		if (!self)
		{
			return;
		}

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
	void Input::SetOsCursorVisible(bool visible)
	{
		if (m_window == nullptr || visible == m_osCursorVisible)
		{
			return;
		}
		m_osCursorVisible = visible;
		glfwSetInputMode(m_window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
	}

} // namespace aether
