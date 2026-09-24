#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aether::app
{
	enum class SynKind : std::uint8_t
	{
		MousePos,
		MouseButton,
		Key,
		Char,
	};

	struct SynEvent
	{
		SynKind kind = SynKind::MousePos;
		float x = 0.0f;
		float y = 0.0f;
		int button = 0;      // 0 = left, 1 = right
		bool down = false;
		int key = 0;         // ImGuiKey value, kept as int so this stays ImGui-free
		unsigned int ch = 0; // UTF-32 code point
	};

	// Frame-stepped synthetic input. One queued action plays over several Step()
	// calls (one per frame); a held hover position is re-emitted every Step until
	// changed or cleared. Pure - no ImGui globals - so it is unit-testable.
	class UiInputScript
	{
	public:
		// `ctrl`/`shift`/`alt` are held for the whole click, which is how a multi-select or a
		// range-select is actually performed - without them those interactions cannot be
		// driven at all, in this panel or any other.
		void QueueClick(float x, float y, int button, bool doubleClick, bool ctrl = false, bool shift = false, bool alt = false);
		// Press at one point, move to another over several frames, release there.
		//
		// A drag cannot be built out of two clicks: ImGui only reports one once the mouse has
		// MOVED while held, so the intermediate positions are the whole mechanism. This is
		// what makes a node editor's links, and any drag-and-drop, reachable from a test.
		// `holdFrames` keeps the button down at the destination for that many extra frames
		// before releasing, so dwell-triggered behaviour - a tree that spring-loads open under a
		// hovering drag - can be driven. 0 keeps the default brief hold.
		void QueueDrag(float fromX, float fromY, float toX, float toY, int button, int holdFrames = 0);
		void QueueHover(float x, float y);
		void QueueKey(int imguiKey);
		// A key pressed WITH modifiers held, which is what an editor shortcut actually is.
		// The modifiers go down a frame early and come up a frame late, because ImGui only
		// sees a chord when they are already held on the frame the key itself goes down.
		void QueueKeyChord(int imguiKey, bool ctrl, bool shift, bool alt);
		void QueueText(std::string utf8);
		void ClearHover();

		// Events for one frame; advances the action cursor. Empty when idle and no
		// hover is held.
		[[nodiscard]] std::vector<SynEvent> Step();
		[[nodiscard]] bool Busy() const;

	private:
		std::vector<SynEvent> m_steps;             // flattened events
		std::vector<std::size_t> m_frameBoundaries; // [begin0,end0, begin1,end1, ...]
		std::size_t m_cursor = 0;                   // index into m_frameBoundaries (step *2)
		std::optional<std::pair<float, float>> m_hover;

		void PushFrame(std::vector<SynEvent> frame);
	};
} // namespace aether::app
