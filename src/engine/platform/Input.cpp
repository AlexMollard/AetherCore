#include "platform/Input.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <sstream>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Radial, rescaled deadzone - see Input::GetGamepadStick for why this is not
		// applied per axis, which is the usual way to get this wrong.
		glm::vec2 ApplyRadialDeadzone(glm::vec2 v, float deadzone)
		{
			const float length = glm::length(v);
			if (length <= deadzone)
			{
				return {0.0f, 0.0f};
			}
			// Clamped at 1: real sticks routinely report a magnitude slightly past full
			// deflection on the diagonals, and without this the rescale hands back a
			// vector longer than 1 - a character that sprints only when moving diagonally.
			const float scaled = std::min((length - deadzone) / (1.0f - deadzone), 1.0f);
			return v * (scaled / length);
		}

		// Raw axis pair -> usable stick vector. The y negation lives here so the poll and the
		// accessor cannot drift apart on which way is up.
		glm::vec2 StickVector(float rawX, float rawY, float deadzone)
		{
			return ApplyRadialDeadzone({rawX, -rawY}, deadzone);
		}

		// How far a stick must lean to register as a direction, and how far it must fall back
		// before it can register again. Two values, not one: a single threshold makes a stick
		// held near the line flick repeatedly as it jitters across it.
		constexpr float kFlickEnter = 0.5f;
		constexpr float kFlickExit = 0.35f;

		// GLFW reports a trigger on the same -1..+1 scale as a stick. See
		// Input::GetGamepadTrigger.
		float TriggerToUnit(float glfwAxis, float deadzone)
		{
			const float unit = (glfwAxis + 1.0f) * 0.5f;
			if (unit <= deadzone)
			{
				return 0.0f;
			}
			return std::min((unit - deadzone) / (1.0f - deadzone), 1.0f);
		}
	} // namespace

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

	int Input::KeyCodeFromName(std::string name)
	{
		for (char& c: name)
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
		{
			return 65 + (name[0] - 'a');
		}
		if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
		{
			return 48 + (name[0] - '0');
		}
		if (name == "left")
		{
			return 263;
		}
		if (name == "right")
		{
			return 262;
		}
		if (name == "up")
		{
			return 265;
		}
		if (name == "down")
		{
			return 264;
		}
		if (name == "space")
		{
			return 32;
		}
		if (name == "enter" || name == "return")
		{
			return 257;
		}
		if (name == "escape" || name == "esc")
		{
			return 256;
		}
		if (name == "tab")
		{
			return 258;
		}
		if (name == "shift" || name == "lshift")
		{
			return 340;
		}
		if (name == "ctrl" || name == "lctrl")
		{
			return 341;
		}
		if (name == "alt" || name == "lalt")
		{
			return 342;
		}
		return -1;
	}

	std::vector<Input::InputSequenceEvent> Input::ParseInputSequence(const std::string& text, std::string& error)
	{
		std::vector<InputSequenceEvent> events;
		std::istringstream stream(text);
		std::string line;
		int lineNo = 0;
		while (std::getline(stream, line))
		{
			++lineNo;
			if (const auto hash = line.find('#'); hash != std::string::npos)
			{
				line.erase(hash);
			}
			std::istringstream ls(line);
			std::string timeTok;
			if (!(ls >> timeTok))
			{
				continue; // blank / comment-only line
			}
			float t = 0.0f;
			try
			{
				t = std::stof(timeTok);
			}
			catch (...)
			{
				error = "line " + std::to_string(lineNo) + ": expected a time, got '" + timeTok + "'";
				return {};
			}
			std::string op;
			if (!(ls >> op))
			{
				continue;
			}
			for (char& c: op)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			std::vector<std::string> keys;
			for (std::string k; ls >> k;)
			{
				keys.push_back(k);
			}

			if (op == "clear" || (op == "release" && keys.size() == 1 && keys[0] == "all"))
			{
				events.push_back({t, -1, false});
				continue;
			}
			// "press" and "tap" both produce a REAL key edge every time: down now,
			// auto-released ~0.1s later. (press used to be a synonym of hold, which
			// made every press after the first a silent no-op - an already-down key
			// re-set never produces a second IsKeyPressed edge, so a stateful device
			// flipped once and then ignored every later press while reporting
			// events:1/ok:true. hold is the op that keeps keys down; use it for that.)
			const bool down = (op == "hold" || op == "press" || op == "tap");
			const bool release = (op == "release" || op == "up");
			const bool autoRelease = (op == "press" || op == "tap");
			if (!down && !release)
			{
				error = "line " + std::to_string(lineNo) + ": unknown op '" + op + "' (use hold/press/tap/release/up/clear)";
				return {};
			}
			for (const std::string& key: keys)
			{
				const int code = KeyCodeFromName(key);
				if (code < 0)
				{
					error = "line " + std::to_string(lineNo) + ": unknown key '" + key + "'";
					return {};
				}
				events.push_back({t, code, !release});
				if (autoRelease)
				{
					events.push_back({t + 0.1f, code, false});
				}
			}
		}
		return events;
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
		// A consumption is a statement about ONE frame's keyboard, so it is dropped
		// here rather than by whoever made it. Nothing has to remember to give a key
		// back, and a consumer that dies mid-frame cannot leave a key muted forever.
		m_consumedKeys.fill(false);

		// GLFW does not synthesise release events when the window loses focus: a
		// key or mouse button held at the moment of an alt-tab keeps reporting
		// PRESS from the stale pre-focus-loss state, so IsKeyDown sticks forever
		// and HadActivityThisFrame pins the editor awake. Real device state is
		// therefore only trusted while the window has focus; synthetic injection
		// is unaffected.
		const bool focused = glfwGetWindowAttrib(m_window, GLFW_FOCUSED) != 0;

		for (int i = 0; i < kMaxKeys; ++i)
		{
			// Synthetic keys (control-server injection) OR into the real state, so
			// headless playtests drive the same IsKeyDown/IsKeyPressed paths.
			m_currKeys[i] = (focused && glfwGetKey(m_window, i) == GLFW_PRESS) || m_syntheticKeys[i];
		}

		for (int i = 0; i < kMaxMouseButtons; ++i)
		{
			// Synthetic buttons OR in like synthetic keys, so injected clicks drive the
			// same IsMouseButtonDown/Pressed/Released paths a real click does.
			m_currMouseButtons[i] = (focused && glfwGetMouseButton(m_window, i) == GLFW_PRESS) || m_syntheticMouseButtons[i];
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

		UpdateGamepads();
	}

	void Input::ConsumeKey(Key key)
	{
		const int k = static_cast<int>(key);
		if (k >= 0 && k < kMaxKeys)
		{
			m_consumedKeys[k] = true;
		}
	}

	bool Input::IsKeyConsumed(Key key) const
	{
		const int k = static_cast<int>(key);
		return k >= 0 && k < kMaxKeys && m_consumedKeys[k];
	}

	bool Input::IsKeyDown(Key key) const
	{
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kMaxKeys)
		{
			return false;
		}
		return m_currKeys[k] && !m_consumedKeys[k];
	}

	bool Input::IsKeyPressed(Key key) const
	{
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kMaxKeys)
		{
			return false;
		}
		return m_currKeys[k] && !m_prevKeys[k] && !m_consumedKeys[k];
	}

	Key Input::GetKeyPressedThisFrame() const
	{
		for (int i = 0; i < kMaxKeys; ++i)
		{
			if (m_currKeys[i] && !m_prevKeys[i] && !m_consumedKeys[i])
			{
				return static_cast<Key>(i);
			}
		}
		// -1 is already the "no key" sentinel every range check above treats as invalid
		// (IsKeyDown/IsKeyPressed/IsKeyReleased all reject k < 0), matching managed
		// Key.None.
		return static_cast<Key>(-1);
	}

	// Deliberately NOT gated on consumption. A consumer takes the key's MEANING for
	// this frame - "Escape closed the field" - and a release is the end of a press
	// somebody may have been tracking since before the consume existed. Muting it
	// would strand a held-key state machine in the down state with no edge to close it.
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
		if (m_cursorLocked)
		{
			// GLFW_CURSOR_DISABLED reports an unbounded virtual position built exactly for
			// this, and there is no absolute on-screen position for a viewport transform to
			// map into pixel space anyway while the pointer is hidden and confined - the raw
			// window-space delta IS the answer (see SetCursorLocked/ApplyCursorMode).
			return m_mousePos - m_prevMousePos;
		}
		const glm::vec2 current = TransformMousePos(m_mousePos);
		const glm::vec2 previous = TransformMousePos(m_prevMousePos);
		if (current.x <= kOutsideViewport || previous.x <= kOutsideViewport)
		{
			return {};
		}
		return current - previous;
	}

	bool Input::HadActivityThisFrame() const
	{
		if (m_mousePos != m_prevMousePos || m_scrollDelta != glm::vec2{} || !m_typedChars.empty())
		{
			return true;
		}
		// Held counts as well as pressed: dragging a slider or holding a camera key produces
		// no edge after the first frame, and going idle mid-drag would be worse than useless.
		for (std::size_t i = 0; i < kMaxMouseButtons; ++i)
		{
			if (m_currMouseButtons[i] || m_prevMouseButtons[i])
			{
				return true;
			}
		}
		for (std::size_t i = 0; i < kMaxKeys; ++i)
		{
			if (m_currKeys[i] || m_prevKeys[i])
			{
				return true;
			}
		}
		return false;
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
		if (visible == m_osCursorVisible)
		{
			return;
		}
		m_osCursorVisible = visible;
		ApplyCursorMode();
	}

	void Input::SetCursorLocked(bool locked)
	{
		if (locked == m_cursorLocked)
		{
			return;
		}
		m_cursorLocked = locked;
		ApplyCursorMode();
	}

	// A locked cursor always wins over whatever custom-cursor visibility the game
	// separately asked for via SetOsCursorVisible, and is restored to exactly that the
	// moment it unlocks - so the two settings never fight over which GLFW call runs last.
	void Input::ApplyCursorMode()
	{
		if (m_window == nullptr)
		{
			return;
		}
		if (m_cursorLocked)
		{
			glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			// Checked rather than assumed - not every platform/backend reports raw motion
			// support, and asking for it anyway is a GLFW error, not a silent fallback.
			if (glfwRawMouseMotionSupported())
			{
				glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
			}
		}
		else
		{
			glfwSetInputMode(m_window, GLFW_CURSOR, m_osCursorVisible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
			if (glfwRawMouseMotionSupported())
			{
				glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
			}
		}
	}

	void Input::UpdateCursorLock(bool windowFocused)
	{
		if (!m_cursorLockRequested)
		{
			m_cursorLockEscaped = false;
		}
		else if (m_cursorLocked && IsKeyPressed(Key::Escape))
		{
			// The editor's own safety net: a captured cursor with no way out would strand
			// the user unable to reach the inspector, the toolbar or the window controls,
			// regardless of what the game's own script does with Escape. NOT consumed (see
			// this method's own doc comment in Input.hpp): a script's own Escape-driven UI
			// must see this exact press in this exact frame, or it waits for a second one
			// while the game silently re-requests the lock it was just released from.
			m_cursorLockEscaped = true;
		}
		SetCursorLocked(m_cursorLockRequested && windowFocused && !m_cursorLockEscaped);

		// Input ownership (see GameOwnsInput's own doc comment for why this is independent
		// of cursor lock): released by the identical Escape press, but NOT gated on
		// m_cursorLocked - a menu-driven game that never locks the cursor at all still
		// needs an escape hatch back to the editor's own panels, and gating this on lock
		// state would leave that whole class of game with no way back in at all.
		if (m_gameOwnsInput && windowFocused && IsKeyPressed(Key::Escape))
		{
			m_gameOwnsInput = false;
		}
	}

	// -- Gamepads -------------------------------------------------------------

	void Input::DeriveStickFlicks(GamepadState& pad, float deadzone)
	{
		for (std::size_t stick = 0; stick < 2; ++stick)
		{
			const glm::vec2 v = StickVector(pad.axes[stick * 2], pad.axes[stick * 2 + 1], deadzone);
			const float component[4] = {v.y, -v.y, -v.x, v.x}; // Up, Down, Left, Right
			for (std::size_t dir = 0; dir < 4; ++dir)
			{
				// Hysteresis: engage above kFlickEnter, then stay engaged until the stick
				// falls back below kFlickExit, so a stick resting on the line cannot chatter.
				const bool wasHeld = pad.stickHeld[stick][dir];
				const bool held = component[dir] > (wasHeld ? kFlickExit : kFlickEnter);
				pad.stickFlicked[stick][dir] = held && !wasHeld;
				pad.stickHeld[stick][dir] = held;
			}
		}
	}

	void Input::UpdateGamepads()
	{
		AE_PROFILE_ZONE();
		for (int i = 0; i < kMaxGamepads; ++i)
		{
			GamepadState& pad = m_gamepads[i];
			pad.prev = pad.curr;

			// One call answers both "is a pad in this slot" and "does GLFW have a mapping
			// for it", so there is no second presence query that can fall out of step with
			// this one. An unmapped stick reports as absent, which is the honest answer.
			GLFWgamepadstate state{};
			const bool present = glfwGetGamepadState(i, &state) == GLFW_TRUE;

			if (present)
			{
				for (int b = 0; b < kMaxGamepadButtons; ++b)
				{
					pad.curr[b] = state.buttons[b] == GLFW_PRESS;
				}
				for (int a = 0; a < kMaxGamepadAxes; ++a)
				{
					pad.axes[a] = state.axes[a];
				}
			}
			else
			{
				// Reset to the resting pose rather than leaving the last poll behind: an
				// unplugged pad must not keep reporting the buttons it held on the way out.
				// Note this is NOT a Released edge - every accessor resolves through Pad(),
				// which stops resolving the moment `connected` drops, so a vanished pad reads
				// uniformly "nothing pressed" rather than emitting one last event. A game that
				// needs to react to a controller dying mid-hold should watch IsGamepadConnected;
				// a phantom button event would be a worse thing to build on.
				pad.curr.fill(false);
				pad.axes = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
			}

			// Synthetic state layers on top, exactly like synthetic keys and mouse buttons.
			for (int b = 0; b < kMaxGamepadButtons; ++b)
			{
				pad.curr[b] = pad.curr[b] || pad.synthetic[b];
			}
			for (int a = 0; a < kMaxGamepadAxes; ++a)
			{
				if (pad.hasSyntheticAxis[a])
				{
					pad.axes[a] = pad.syntheticAxes[a];
				}
			}

			// Same lifetime as m_consumedKeys: one frame, dropped by the owner of the state
			// rather than by whoever consumed it, so a consumer that dies mid-frame cannot
			// leave a button muted forever.
			pad.consumed.fill(false);

			// Stick-as-direction edges, derived once here so every reader this frame agrees.
			DeriveStickFlicks(pad, m_stickDeadzone);

			const bool connected = present || pad.syntheticConnected;
			if (connected != pad.connected)
			{
				pad.connected = connected;
				if (connected)
				{
					const char* name = present ? glfwGetGamepadName(i) : nullptr;
					pad.name = name != nullptr ? name : "Synthetic Gamepad";
					AE_INFO(LogCategory::Input, "Gamepad {} connected: {}", i, pad.name);
				}
				else
				{
					AE_INFO(LogCategory::Input, "Gamepad {} disconnected ({})", i, pad.name);
					pad.name.clear();
				}
			}
		}
	}

	int Input::ResolveGamepad(int pad) const
	{
		if (pad == kAnyGamepad)
		{
			for (int i = 0; i < kMaxGamepads; ++i)
			{
				if (m_gamepads[i].connected)
				{
					return i;
				}
			}
			return -1;
		}
		return (pad >= 0 && pad < kMaxGamepads && m_gamepads[pad].connected) ? pad : -1;
	}

	const Input::GamepadState* Input::Pad(int pad) const
	{
		const int slot = ResolveGamepad(pad);
		return slot >= 0 ? &m_gamepads[static_cast<std::size_t>(slot)] : nullptr;
	}

	bool Input::IsGamepadConnected(int pad) const
	{
		return ResolveGamepad(pad) >= 0;
	}

	std::string_view Input::GetGamepadName(int pad) const
	{
		const GamepadState* p = Pad(pad);
		return p != nullptr ? std::string_view(p->name) : std::string_view{};
	}

	bool Input::IsGamepadButtonDown(GamepadButton button, int pad) const
	{
		const GamepadState* p = Pad(pad);
		const int b = static_cast<int>(button);
		if (p == nullptr || b < 0 || b >= kMaxGamepadButtons)
		{
			return false;
		}
		return p->curr[static_cast<std::size_t>(b)] && !p->consumed[static_cast<std::size_t>(b)];
	}

	bool Input::IsGamepadButtonPressed(GamepadButton button, int pad) const
	{
		const GamepadState* p = Pad(pad);
		const int b = static_cast<int>(button);
		if (p == nullptr || b < 0 || b >= kMaxGamepadButtons)
		{
			return false;
		}
		return p->curr[static_cast<std::size_t>(b)] && !p->prev[static_cast<std::size_t>(b)] && !p->consumed[static_cast<std::size_t>(b)];
	}

	bool Input::IsGamepadButtonReleased(GamepadButton button, int pad) const
	{
		const GamepadState* p = Pad(pad);
		const int b = static_cast<int>(button);
		if (p == nullptr || b < 0 || b >= kMaxGamepadButtons)
		{
			return false;
		}
		return !p->curr[static_cast<std::size_t>(b)] && p->prev[static_cast<std::size_t>(b)];
	}

	glm::vec2 Input::GetGamepadStick(GamepadStick stick, int pad) const
	{
		const GamepadState* p = Pad(pad);
		if (p == nullptr)
		{
			return {0.0f, 0.0f};
		}
		const std::size_t base = stick == GamepadStick::Left ? 0u : 2u;
		return StickVector(p->axes[base], p->axes[base + 1u], m_stickDeadzone);
	}

	float Input::GetGamepadTrigger(GamepadTrigger trigger, int pad) const
	{
		const GamepadState* p = Pad(pad);
		if (p == nullptr)
		{
			return 0.0f;
		}
		const std::size_t axis = trigger == GamepadTrigger::Left ? 4u : 5u;
		return TriggerToUnit(p->axes[axis], m_triggerDeadzone);
	}

	float Input::GetGamepadAxisRaw(GamepadAxis axis, int pad) const
	{
		const GamepadState* p = Pad(pad);
		const int a = static_cast<int>(axis);
		if (p == nullptr || a < 0 || a >= kMaxGamepadAxes)
		{
			return 0.0f;
		}
		return p->axes[static_cast<std::size_t>(a)];
	}

	void Input::SetGamepadDeadzones(float stick, float trigger)
	{
		// Upper bound of 0.9 rather than 1: the rescale divides by (1 - deadzone), so a
		// deadzone of exactly 1 is a division by zero and anything near it turns the last
		// sliver of stick travel into the whole output range.
		m_stickDeadzone = std::clamp(stick, 0.0f, 0.9f);
		m_triggerDeadzone = std::clamp(trigger, 0.0f, 0.9f);
	}

	bool Input::IsGamepadStickFlicked(GamepadStick stick, GamepadDirection dir, int pad) const
	{
		const GamepadState* p = Pad(pad);
		if (p == nullptr)
		{
			return false;
		}
		return p->stickFlicked[stick == GamepadStick::Left ? 0u : 1u][static_cast<std::size_t>(dir)];
	}

	void Input::ConsumeGamepadButton(GamepadButton button, int pad)
	{
		const int slot = ResolveGamepad(pad);
		const int b = static_cast<int>(button);
		if (slot < 0 || b < 0 || b >= kMaxGamepadButtons)
		{
			return;
		}
		m_gamepads[static_cast<std::size_t>(slot)].consumed[static_cast<std::size_t>(b)] = true;
	}

	bool Input::IsGamepadButtonConsumed(GamepadButton button, int pad) const
	{
		const GamepadState* p = Pad(pad);
		const int b = static_cast<int>(button);
		return p != nullptr && b >= 0 && b < kMaxGamepadButtons && p->consumed[static_cast<std::size_t>(b)];
	}

	void Input::SetSyntheticGamepadConnected(int pad, bool connected)
	{
		if (pad < 0 || pad >= kMaxGamepads)
		{
			return;
		}
		GamepadState& p = m_gamepads[static_cast<std::size_t>(pad)];
		p.syntheticConnected = connected;
		// Seed live state ONLY when there is no window, for the same reason SetSyntheticKey
		// does: a windowless unit test never calls Update(), which is what normally derives
		// `connected`. With a window this write would race Update() and is left to it.
		if (m_window == nullptr)
		{
			p.connected = connected;
			if (connected && p.name.empty())
			{
				p.name = "Synthetic Gamepad";
			}
		}
	}

	void Input::SetSyntheticGamepadButton(int pad, GamepadButton button, bool down)
	{
		const int b = static_cast<int>(button);
		if (pad < 0 || pad >= kMaxGamepads || b < 0 || b >= kMaxGamepadButtons)
		{
			return;
		}
		GamepadState& p = m_gamepads[static_cast<std::size_t>(pad)];
		p.synthetic[static_cast<std::size_t>(b)] = down;
		// Windowless only - see SetSyntheticKey for why seeding curr with a window
		// attached silently destroys the down-edge that IsGamepadButtonPressed reports.
		if (m_window == nullptr)
		{
			p.prev[static_cast<std::size_t>(b)] = p.curr[static_cast<std::size_t>(b)];
			p.curr[static_cast<std::size_t>(b)] = down;
			// A fresh frame for this button, so last frame's consumption expires with it -
			// otherwise a windowless caller could consume once and mute the button forever.
			p.consumed[static_cast<std::size_t>(b)] = false;
		}
	}

	void Input::SetSyntheticGamepadAxis(int pad, GamepadAxis axis, float value)
	{
		const int a = static_cast<int>(axis);
		if (pad < 0 || pad >= kMaxGamepads || a < 0 || a >= kMaxGamepadAxes)
		{
			return;
		}
		GamepadState& p = m_gamepads[static_cast<std::size_t>(pad)];
		const float clamped = std::clamp(value, -1.0f, 1.0f);
		p.syntheticAxes[static_cast<std::size_t>(a)] = clamped;
		p.hasSyntheticAxis[static_cast<std::size_t>(a)] = true;
		if (m_window == nullptr)
		{
			p.axes[static_cast<std::size_t>(a)] = clamped;
			// Windowless there is no Update() to derive the latch, and a caller setting an
			// axis IS the frame. Without this the flick edge would be unreachable from a test.
			DeriveStickFlicks(p, m_stickDeadzone);
		}
	}

	void Input::ClearSyntheticGamepads()
	{
		for (GamepadState& p: m_gamepads)
		{
			p.synthetic.fill(false);
			p.hasSyntheticAxis.fill(false);
			p.syntheticConnected = false;
			// Symmetric with the setters: a windowless caller has no Update() to re-derive
			// this, so without the reset a cleared pad stays connected with its buttons held.
			if (m_window == nullptr)
			{
				p.connected = false;
				p.curr.fill(false);
				p.prev.fill(false);
				p.axes = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
				p.consumed.fill(false);
				p.stickHeld = {};
				p.stickFlicked = {};
				p.name.clear();
			}
		}
	}

} // namespace aether
