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

	// Standard-gamepad layout: GLFW's remapped view of any controller found in the SDL
	// controller database it bundles (944 entries), so an Xbox pad, a DualShock and a
	// no-name clone all report through these same names. Values match
	// GLFW_GAMEPAD_BUTTON_* and are cast straight across.
	enum class GamepadButton : int
	{
		A = 0,
		B = 1,
		X = 2,
		Y = 3,
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,
		Start = 7,
		Guide = 8,
		LeftThumb = 9,
		RightThumb = 10,
		DpadUp = 11,
		DpadRight = 12,
		DpadDown = 13,
		DpadLeft = 14,
	};

	// Values match GLFW_GAMEPAD_AXIS_*. Prefer GetGamepadStick and GetGamepadTrigger over
	// reading these raw - both fix a convention mismatch that stays invisible until a
	// player picks up a controller. See those two functions.
	enum class GamepadAxis : int
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5,
	};

	enum class GamepadStick : int
	{
		Left = 0,
		Right = 1,
	};

	enum class GamepadTrigger : int
	{
		Left = 0,
		Right = 1,
	};

	// A stick pushed far enough to count as a direction, for anything that wants a stick to
	// behave like a d-pad (menus, choice lists). See Input::IsGamepadStickFlicked.
	enum class GamepadDirection : int
	{
		Up = 0,
		Down = 1,
		Left = 2,
		Right = 3,
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

		// ── Key consumption ──────────────────────────────────────────────────
		// "This key has already been dealt with; nobody downstream gets it."
		//
		// A frame's keyboard is read by several independent consumers in a fixed order
		// - the UI pass, then scripts, then the editor's own shortcuts - and there is
		// no other way for the first of them to say that a key MEANT something. Without
		// it, Escape closing a text field is also Escape leaving the level, and both
		// happen: neither reader can see the other, and the only workarounds available
		// to a game are flags written by one script and read by another, which makes the
		// answer depend on script order and therefore on nothing the game controls.
		//
		// The rule is deliberately positional rather than a priority table: whoever
		// handles a key FIRST owns it, and everything after that frame's consume sees
		// the key as not pressed and not held. Consumers that ran EARLIER are
		// unaffected - a consume is not retroactive - so this can only ever remove an
		// ambiguity, never introduce one.
		//
		// Cleared at the top of every Update(), so a consumption lasts exactly one
		// frame and nothing has to remember to give a key back.
		void ConsumeKey(Key key);

		[[nodiscard]] bool IsKeyConsumed(Key key) const;

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

		// -- Gamepads ---------------------------------------------------------
		// Polled in Update() alongside the keyboard, through GLFW's *gamepad* view rather
		// than raw joystick axes: GLFW ships the SDL controller database, so a recognised
		// pad reports one fixed button/axis layout whatever the vendor. A pad GLFW cannot
		// map is reported as not connected rather than as a scrambled pile of axes, which
		// is the honest answer - nothing useful can be done with an unmapped stick.
		//
		// `pad` is a slot in [0, kMaxGamepads). kAnyGamepad reads whichever slot is
		// connected first, and is the default everywhere including the script API: GLFW
		// slots are not compacted, so a single controller routinely enumerates as slot 1
		// or 2 with a virtual device, a wheel or a dormant receiver holding slot 0.
		// Hardcoding slot 0 is the classic gamepad bug that works on the developer's
		// machine and does nothing at all on someone else's.
		static constexpr int kAnyGamepad = -1;

		[[nodiscard]] bool IsGamepadConnected(int pad = kAnyGamepad) const;

		// The concrete slot `pad` names, or -1 when nothing is connected there.
		[[nodiscard]] int ResolveGamepad(int pad) const;

		// Human-readable pad name from the mapping database ("Xbox Controller"), for a
		// bindings screen or a "controller connected" toast. Empty when disconnected.
		[[nodiscard]] std::string_view GetGamepadName(int pad = kAnyGamepad) const;

		[[nodiscard]] bool IsGamepadButtonDown(GamepadButton button, int pad = kAnyGamepad) const;
		[[nodiscard]] bool IsGamepadButtonPressed(GamepadButton button, int pad = kAnyGamepad) const;
		[[nodiscard]] bool IsGamepadButtonReleased(GamepadButton button, int pad = kAnyGamepad) const;

		// Stick position with a radial, rescaled deadzone, y flipped so +1 is up.
		//
		// RADIAL, not per-axis: a per-axis deadzone carves a SQUARE dead region out of a
		// round stick, so a fully diagonal push reads (1, 1) - magnitude 1.41, and the
		// character moves 41% faster diagonally - while a stick just off-axis snaps to a
		// pure cardinal and diagonal aiming becomes impossible. Both are the same bug.
		//
		// RESCALED: the magnitude ramps from 0 at the deadzone edge instead of jumping
		// straight to the deadzone value, so there is no visible lurch as the stick
		// leaves the dead region.
		//
		// Y FLIPPED: GLFW follows SDL, where pushing a stick up reports -1, but this
		// engine's world is y-up - a positive velocity moves something up the screen.
		// Passing GLFW's sign through would make every gamepad-driven character walk in
		// the opposite direction to the stick.
		[[nodiscard]] glm::vec2 GetGamepadStick(GamepadStick stick, int pad = kAnyGamepad) const;

		// 0 released .. 1 fully pressed, past a small deadzone.
		//
		// GLFW reports triggers on the same -1..+1 scale as sticks, so an untouched
		// trigger reads -1 and a pass-through reads as "half pressed, backwards". That
		// holds both for pads whose mapping drives the trigger from a real axis and for
		// the ~320 database entries that map it to a plain button, which GLFW converts
		// with `button * 2 - 1` - so one conversion is correct for both.
		[[nodiscard]] float GetGamepadTrigger(GamepadTrigger trigger, int pad = kAnyGamepad) const;

		// Untouched GLFW value in [-1, 1]: no deadzone, no y flip, no trigger remap, for a
		// game that wants its own response curve. Offered because deadzone taste genuinely
		// varies, and withholding the raw value only means it gets reimplemented worse.
		[[nodiscard]] float GetGamepadAxisRaw(GamepadAxis axis, int pad = kAnyGamepad) const;

		// Defaults follow XInput's documented thresholds - 7849/32767 for a thumbstick and
		// 30/255 for a trigger - which is what the hardware is physically built around.
		// Clamped to leave headroom above the deadzone; a deadzone of 1 has no meaning.
		void SetGamepadDeadzones(float stick, float trigger);

		[[nodiscard]] float GetStickDeadzone() const
		{
			return m_stickDeadzone;
		}

		[[nodiscard]] float GetTriggerDeadzone() const
		{
			return m_triggerDeadzone;
		}

		// A stick pushed past the threshold THIS frame, having been inside it last frame -
		// a stick behaving like a d-pad.
		//
		// Menus and choice lists need one step per push, and a stick has no press edge of
		// its own: it is a position, so "is it pushed up" is true every frame it is held and
		// a menu written against it scrolls at one entry per frame. Latched here rather than
		// by each caller because every caller needs the identical thing, and the per-caller
		// version has to invent its own idea of "this frame" - which is how it ends up
		// firing twice when something reads it twice.
		//
		// Release uses a lower threshold than engagement, so a stick resting near the line
		// does not chatter between the two. Both are measured against the DEADZONED magnitude
		// - the same value GetGamepadStick returns - rather than the raw axis, so raising the
		// deadzone raises the physical lean a flick needs, which is what a player adjusting it
		// for a worn stick means.
		[[nodiscard]] bool IsGamepadStickFlicked(GamepadStick stick, GamepadDirection dir, int pad = kAnyGamepad) const;

		// Gamepad-button counterpart of ConsumeKey, and needed for exactly the same reason:
		// the button that activates a menu item must not also reach the game. Without it,
		// the A that picks "Resume" is read a moment later by the player controller and the
		// character jumps as the menu closes.
		//
		// Cleared at the top of every frame's poll, so a consumption lasts one frame and
		// nothing has to remember to give the button back.
		void ConsumeGamepadButton(GamepadButton button, int pad = kAnyGamepad);
		[[nodiscard]] bool IsGamepadButtonConsumed(GamepadButton button, int pad = kAnyGamepad) const;

		// Synthetic gamepad injection, same contract as synthetic keys and mouse: an
		// injected pad presents as connected, injected buttons OR into the real state, and
		// an injected axis overrides the real one until cleared. This is what lets a
		// headless playtest drive controller paths with no controller attached - and it is
		// the only way the deadzone and trigger conversions above are testable at all.
		void SetSyntheticGamepadConnected(int pad, bool connected);
		void SetSyntheticGamepadButton(int pad, GamepadButton button, bool down);
		void SetSyntheticGamepadAxis(int pad, GamepadAxis axis, float value);
		void ClearSyntheticGamepads();

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
		// Four rather than GLFW's sixteen: four is what XInput exposes, what console local
		// co-op assumes, and what a couch seats. Raising it is a one-line change.
		static constexpr int kMaxGamepads = 4;
		static constexpr int kMaxGamepadButtons = 15;
		static constexpr int kMaxGamepadAxes = 6;

		struct GamepadState
		{
			std::array<bool, kMaxGamepadButtons> curr{};
			std::array<bool, kMaxGamepadButtons> prev{};
			// Seeded to GLFW's RESTING values rather than zero: triggers idle at -1 there,
			// so a zero-filled array reports both triggers as half pressed on any pad that
			// is connected but not yet polled - which a synthetic pad always is.
			std::array<float, kMaxGamepadAxes> axes{0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
			std::array<bool, kMaxGamepadButtons> synthetic{};
			std::array<float, kMaxGamepadAxes> syntheticAxes{0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
			std::array<bool, kMaxGamepadAxes> hasSyntheticAxis{};
			std::array<bool, kMaxGamepadButtons> consumed{};
			// [stick][direction] - whether the stick was past the threshold last frame, and
			// whether it crossed on this one. Derived during the poll rather than on read, so
			// reading a flick twice in one frame gives the same answer both times.
			std::array<std::array<bool, 4>, 2> stickHeld{};
			std::array<std::array<bool, 4>, 2> stickFlicked{};
			std::string name;
			bool connected = false;
			bool syntheticConnected = false;
		};

		void UpdateGamepads();

		// Shared by the real poll and by synthetic injection: a windowless test never calls
		// Update(), so without this the flick latch would only ever exist on a real frame and
		// nothing about it would be testable.
		static void DeriveStickFlicks(GamepadState& pad, float deadzone);

		// Resolves kAnyGamepad and range-checks the slot in one place, so every public
		// accessor is a null check rather than its own copy of the bounds logic.
		[[nodiscard]] const GamepadState* Pad(int pad) const;

		GLFWwindow* m_window = nullptr;
		bool m_osCursorVisible = true;

		std::array<bool, kMaxKeys> m_currKeys{};
		std::array<bool, kMaxKeys> m_prevKeys{};
		std::array<bool, kMaxKeys> m_consumedKeys{};
		std::array<bool, kMaxKeys> m_syntheticKeys{};
		std::array<bool, kMaxMouseButtons> m_syntheticMouseButtons{};
		glm::vec2 m_syntheticMousePos{0.0f};
		bool m_hasSyntheticMousePos = false;
		std::array<bool, kMaxMouseButtons> m_currMouseButtons{};
		std::array<bool, kMaxMouseButtons> m_prevMouseButtons{};

		std::array<GamepadState, kMaxGamepads> m_gamepads{};
		float m_stickDeadzone = 0.24f;
		float m_triggerDeadzone = 0.12f;

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
