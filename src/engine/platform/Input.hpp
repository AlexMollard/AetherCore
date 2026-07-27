#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

struct GLFWwindow;

namespace aether
{
	enum class Key : int
	{
		Space = 32,
		Apostrophe = 39,
		Comma = 44,
		Minus = 45,
		Period = 46,
		Slash = 47,
		Num0 = 48,
		Num1,
		Num2,
		Num3,
		Num4,
		Num5,
		Num6,
		Num7,
		Num8,
		Num9,
		Semicolon = 59,
		Equal = 61,
		A = 65,
		B,
		C,
		D,
		E,
		F,
		G,
		H,
		I,
		J,
		K,
		L,
		M,
		N,
		O,
		P,
		Q,
		R,
		S,
		T,
		U,
		V,
		W,
		X,
		Y,
		Z,
		LeftBracket = 91,
		Backslash = 92,
		RightBracket = 93,
		GraveAccent = 96,
		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2,
		F3,
		F4,
		F5,
		F6,
		F7,
		F8,
		F9,
		F10,
		F11,
		F12,
		Kp0 = 320,
		Kp1,
		Kp2,
		Kp3,
		Kp4,
		Kp5,
		Kp6,
		Kp7,
		Kp8,
		Kp9,
		KpDecimal = 330,
		KpDivide = 331,
		KpMultiply = 332,
		KpSubtract = 333,
		KpAdd = 334,
		KpEnter = 335,
		KpEqual = 336,
		LeftShift = 340,
		LeftCtrl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightCtrl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348,
	};

	enum class MouseButton : int
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		B4 = 3,
		B5 = 4,
		B6 = 5,
		B7 = 6,
		B8 = 7,
	};

	class Input
	{
	public:
		Input() = default;
		~Input();

		Input(const Input&) = delete;
		Input& operator=(const Input&) = delete;

		void Init(GLFWwindow* window);

		void Update();

		[[nodiscard]] bool IsKeyDown(Key key) const;

		[[nodiscard]] bool IsKeyPressed(Key key) const;

		[[nodiscard]] bool IsKeyReleased(Key key) const;

		[[nodiscard]] bool IsMouseButtonDown(MouseButton btn) const;
		[[nodiscard]] bool IsMouseButtonPressed(MouseButton btn) const;
		[[nodiscard]] bool IsMouseButtonReleased(MouseButton btn) const;

		[[nodiscard]] glm::vec2 GetMousePos() const;

		// The pixel-space size that GetMousePos() is expressed in: the game render
		// target when a viewport transform is active (editor play), otherwise the
		// window size (shipped game). Scripts need this to unproject the cursor.
		[[nodiscard]] glm::vec2 GetMouseTargetSize() const;

		[[nodiscard]] glm::vec2 GetMouseDelta() const;

		void SetMouseViewportTransform(glm::vec2 viewportMin, glm::vec2 viewportSize, glm::vec2 targetSize);
		void ClearMouseViewportTransform();

		void SetMouseViewportInputActive(bool active)
		{
			m_mouseViewportInputActive = active;
		}

		[[nodiscard]] bool IsMouseViewportInputActive() const
		{
			return m_mouseViewportInputActive;
		}

		// True when mouse coordinates are being remapped into a game viewport, i.e. the game is being
		// hosted inside a tool rather than owning the window. A shipped game never sets one.
		[[nodiscard]] bool HasMouseViewportTransform() const
		{
			return m_mouseViewportTransformActive;
		}

		[[nodiscard]] glm::vec2 GetScrollDelta() const;

		// layout, dead keys, and IME - far more reliable than manual key->char mapping.
		[[nodiscard]] const std::string& GetTypedChars() const;

		// OS clipboard. With no window (unit tests) both fall back to an internal string, so
		// cut/copy/paste logic is exercisable headless.
		[[nodiscard]] std::string GetClipboardText() const;
		void SetClipboardText(std::string_view text);

		void SetMouseCaptured(bool captured)
		{
			m_mouseCaptured = captured;
		}

		// Hide the OS pointer so a game can draw its own. HIDDEN, not DISABLED: the pointer keeps its
		// real screen position and reports normally, it simply is not painted - so mouse input, window
		// chrome and alt-tab all behave exactly as before. DISABLED would lock the pointer to the window
		// and switch to raw virtual motion, which is what an FPS wants and what a game drawing a pointer
		// at a true screen position very much does not.
		void SetOsCursorVisible(bool visible);

		[[nodiscard]] bool IsOsCursorVisible() const
		{
			return m_osCursorVisible;
		}

		[[nodiscard]] bool IsMouseCaptured() const
		{
			return m_mouseCaptured;
		}

		// Synthetic key injection (headless playtesting via the control server). A
		// synthetic key is OR'd into the real GLFW state each Update(), so held keys
		// drive IsKeyDown and the down-edge still fires IsKeyPressed exactly once.
		void SetSyntheticKey(int key, bool down)
		{
			if (key >= 0 && key < kMaxKeys)
			{
				m_syntheticKeys[key] = down;
				// Seed the live state ONLY when there is no window. A windowless unit test
				// never calls Update(), which is what normally derives m_currKeys, so without
				// this it would observe nothing.
				//
				// With a window this write is actively harmful and must not happen: Update()
				// owns both arrays and begins with `m_prevKeys = m_currKeys`. Seeding
				// m_currKeys here means the next Update() copies the already-pressed state
				// into m_prevKeys, so curr and prev are both true and the down-edge is gone -
				// IsKeyPressed never fires for injected keys, while IsKeyDown still works.
				// That silently breaks every headless playtest that waits on a key press.
				if (m_window == nullptr)
				{
					m_currKeys[key] = m_currKeys[key] || down;
				}
			}
		}

		void ClearSyntheticKeys()
		{
			m_syntheticKeys.fill(false);
			// Symmetric with SetSyntheticKey: a windowless caller has no Update() to
			// re-derive m_currKeys, so without this a cleared key would stay down forever.
			if (m_window == nullptr)
			{
				m_currKeys.fill(false);
			}
		}

		// Synthetic TEXT injection, the character-level counterpart to SetSyntheticKey: the
		// string is appended to the next frame's GetTypedChars(), so a headless test types
		// through exactly the path a real keyboard does. Consumed and cleared each Update().
		void SetSyntheticChars(std::string_view chars)
		{
			m_pendingChars.append(chars);
			// Seed the live buffer too, the way SetSyntheticKey seeds m_currKeys. Only when there is
			// no window: Update() rebuilds m_typedChars from m_pendingChars every frame, so seeding a
			// windowed Input would deliver the text twice. Windowless unit tests never call Update(),
			// and without this they could never observe a typed character at all.
			if (m_window == nullptr)
			{
				m_typedChars.append(chars);
			}
		}

		void ClearSyntheticChars()
		{
			m_pendingChars.clear();
			if (m_window == nullptr)
			{
				m_typedChars.clear();
			}
		}

		// Synthetic MOUSE injection, same contract as keys: buttons OR into the real
		// state each Update() so IsMouseButtonDown and the Pressed/Released edges all
		// fire. A synthetic cursor position, once set, overrides the real one until
		// cleared - together these let a headless test drive click-and-drag tools
		// (drawing, painting) exactly like a player would.
		void SetSyntheticMouseButton(int button, bool down)
		{
			if (button >= 0 && button < kMaxMouseButtons)
			{
				m_syntheticMouseButtons[button] = down;
			}
		}

		// Position is in the same pixel space GetMousePos() reports (see GetMouseTargetSize).
		void SetSyntheticMousePos(glm::vec2 pos)
		{
			m_syntheticMousePos = pos;
			m_hasSyntheticMousePos = true;
		}

		void ClearSyntheticMouse()
		{
			m_syntheticMouseButtons.fill(false);
			m_hasSyntheticMousePos = false;
		}

		// Timed synthetic-input playback for auto-testing. A sequence is a list of
		// events (seconds-from-start, key, down/up); keyCode < 0 means "release all".
		// Driven off a wall clock in Update(), so it survives variable framerate and
		// needs no per-frame dt. See engine.play_input_sequence.
		struct InputSequenceEvent
		{
			float time = 0.0f;
			int keyCode = 0; // < 0 == clear all synthetic keys
			bool down = false;
		};

		void PlayInputSequence(std::vector<InputSequenceEvent> events);
		void StopInputSequence();

		[[nodiscard]] bool IsInputSequenceActive() const
		{
			return m_inputSequenceActive;
		}

	private:
		void TickInputSequence();

		static void OnScroll(GLFWwindow* window, double xOffset, double yOffset);
		static void OnChar(GLFWwindow* window, unsigned int codepoint);

		static constexpr int kMaxKeys = 349;
		static constexpr int kMaxMouseButtons = 8;

		GLFWwindow* m_window = nullptr;
		bool m_osCursorVisible = true;

		std::array<bool, kMaxKeys> m_currKeys{};
		std::array<bool, kMaxKeys> m_prevKeys{};
		std::array<bool, kMaxKeys> m_syntheticKeys{};
		std::array<bool, kMaxMouseButtons> m_syntheticMouseButtons{};
		glm::vec2 m_syntheticMousePos{0.0f};
		bool m_hasSyntheticMousePos = false;
		std::array<bool, kMaxMouseButtons> m_currMouseButtons{};
		std::array<bool, kMaxMouseButtons> m_prevMouseButtons{};

		glm::vec2 m_mousePos{};
		glm::vec2 m_prevMousePos{};

		glm::vec2 m_scrollDelta{};
		glm::vec2 m_pendingScroll{};

		std::string m_typedChars;
		std::string m_pendingChars;
		std::string m_clipboardFallback;

		bool m_firstUpdate = true;
		bool m_mouseCaptured = false;
		bool m_mouseViewportInputActive = false;

		std::vector<InputSequenceEvent> m_inputSequence;
		std::size_t m_inputSequenceNext = 0;
		bool m_inputSequenceActive = false;
		std::chrono::steady_clock::time_point m_inputSequenceStart{};

		bool m_mouseViewportTransformActive = false;
		glm::vec2 m_mouseViewportMin{};
		glm::vec2 m_mouseViewportSize{};
		glm::vec2 m_mouseViewportTargetSize{};

		[[nodiscard]] glm::vec2 TransformMousePos(glm::vec2 windowMousePos) const;
	};
} // namespace aether
