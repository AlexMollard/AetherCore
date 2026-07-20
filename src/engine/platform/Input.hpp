#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
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

		[[nodiscard]] glm::vec2 GetScrollDelta() const;

		// layout, dead keys, and IME - far more reliable than manual key->char mapping.
		[[nodiscard]] const std::string& GetTypedChars() const;

		void SetMouseCaptured(bool captured)
		{
			m_mouseCaptured = captured;
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
			}
		}

		void ClearSyntheticKeys()
		{
			m_syntheticKeys.fill(false);
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

		std::array<bool, kMaxKeys> m_currKeys{};
		std::array<bool, kMaxKeys> m_prevKeys{};
		std::array<bool, kMaxKeys> m_syntheticKeys{};
		std::array<bool, kMaxMouseButtons> m_currMouseButtons{};
		std::array<bool, kMaxMouseButtons> m_prevMouseButtons{};

		glm::vec2 m_mousePos{};
		glm::vec2 m_prevMousePos{};

		glm::vec2 m_scrollDelta{};
		glm::vec2 m_pendingScroll{};

		std::string m_typedChars;
		std::string m_pendingChars;

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
