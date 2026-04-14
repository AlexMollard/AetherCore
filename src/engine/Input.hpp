#pragma once

#include <array>

#include <glm/glm.hpp>

struct GLFWwindow;

namespace aether
{
	// Key codes that mirror GLFW_KEY_* values exactly, enabling
	// zero-cost casting between Key and the raw int GLFW expects.
	enum class Key : int
	{
		Space        = 32,
		Apostrophe   = 39,
		Comma        = 44,
		Minus        = 45,
		Period       = 46,
		Slash        = 47,
		Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
		Semicolon    = 59,
		Equal        = 61,
		A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
		N,      O, P, Q, R, S, T, U, V, W, X, Y, Z,
		LeftBracket  = 91,
		Backslash    = 92,
		RightBracket = 93,
		GraveAccent  = 96,
		Escape       = 256,
		Enter        = 257,
		Tab          = 258,
		Backspace    = 259,
		Insert       = 260,
		Delete       = 261,
		Right        = 262,
		Left         = 263,
		Down         = 264,
		Up           = 265,
		PageUp       = 266,
		PageDown     = 267,
		Home         = 268,
		End          = 269,
		CapsLock     = 280,
		ScrollLock   = 281,
		NumLock      = 282,
		PrintScreen  = 283,
		Pause        = 284,
		F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
		Kp0 = 320, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
		KpDecimal  = 330,
		KpDivide   = 331,
		KpMultiply = 332,
		KpSubtract = 333,
		KpAdd      = 334,
		KpEnter    = 335,
		KpEqual    = 336,
		LeftShift  = 340,
		LeftCtrl   = 341,
		LeftAlt    = 342,
		LeftSuper  = 343,
		RightShift = 344,
		RightCtrl  = 345,
		RightAlt   = 346,
		RightSuper = 347,
		Menu       = 348,
	};

	enum class MouseButton : int
	{
		Left   = 0,
		Right  = 1,
		Middle = 2,
		B4 = 3, B5 = 4, B6 = 5, B7 = 6, B8 = 7,
	};

	// Input manager — updated once per frame by AetherCore::BeginFrame().
	// Provides edge-triggered pressed/released queries so callers do not
	// need to track previous-frame state themselves.
	class Input
	{
	public:
		Input() = default;
		~Input();

		Input(const Input&) = delete;
		Input& operator=(const Input&) = delete;

		// Called once during engine construction.
		void Init(GLFWwindow* window);

		// Called every frame by AetherCore::BeginFrame() before layers run.
		void Update();

		// ── Keyboard ─────────────────────────────────────────────────────────

		// True every frame the key is physically held down.
		[[nodiscard]] bool IsKeyDown(Key key) const;

		// True only on the first frame the key transitions from up to down.
		[[nodiscard]] bool IsKeyPressed(Key key) const;

		// True only on the first frame the key transitions from down to up.
		[[nodiscard]] bool IsKeyReleased(Key key) const;

		// ── Mouse buttons ─────────────────────────────────────────────────────

		[[nodiscard]] bool IsMouseButtonDown(MouseButton btn) const;
		[[nodiscard]] bool IsMouseButtonPressed(MouseButton btn) const;
		[[nodiscard]] bool IsMouseButtonReleased(MouseButton btn) const;

		// ── Mouse cursor ──────────────────────────────────────────────────────

		// Cursor position in screen-space pixels, origin at top-left.
		[[nodiscard]] glm::vec2 GetMousePos() const;

		// Frame-over-frame cursor displacement in pixels.
		[[nodiscard]] glm::vec2 GetMouseDelta() const;

		// ── Scroll wheel ──────────────────────────────────────────────────────

		// Scroll offset accumulated since the last frame (zeroed each Update).
		// x = horizontal, y = vertical.
		[[nodiscard]] glm::vec2 GetScrollDelta() const;

	private:
		static void OnScroll(GLFWwindow* window, double xOffset, double yOffset);

		// GLFW_KEY_LAST = 348  →  349 slots cover every defined key code.
		static constexpr int kMaxKeys         = 349;
		// GLFW_MOUSE_BUTTON_LAST = 7  →  8 buttons.
		static constexpr int kMaxMouseButtons = 8;

		GLFWwindow* m_window = nullptr;

		std::array<bool, kMaxKeys>         m_currKeys{};
		std::array<bool, kMaxKeys>         m_prevKeys{};
		std::array<bool, kMaxMouseButtons> m_currMouseButtons{};
		std::array<bool, kMaxMouseButtons> m_prevMouseButtons{};

		glm::vec2 m_mousePos{};
		glm::vec2 m_prevMousePos{};

		glm::vec2 m_scrollDelta{};    // exposed to callers this frame
		glm::vec2 m_pendingScroll{};  // accumulated from GLFW callback

		bool m_firstUpdate = true;
	};
}
