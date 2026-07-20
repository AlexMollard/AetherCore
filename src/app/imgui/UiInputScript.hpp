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
		void QueueClick(float x, float y, int button, bool doubleClick);
		void QueueHover(float x, float y);
		void QueueKey(int imguiKey);
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
