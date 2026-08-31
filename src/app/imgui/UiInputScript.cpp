#include "imgui/UiInputScript.hpp"

#include <imgui.h>

namespace aether::app
{
	namespace
	{
		// Decode UTF-8 into code points (minimal, assumes valid input).
		std::vector<unsigned int> DecodeUtf8(const std::string& s)
		{
			std::vector<unsigned int> out;
			for (std::size_t i = 0; i < s.size();)
			{
				const unsigned char c = static_cast<unsigned char>(s[i]);
				unsigned int cp = 0;
				int extra = 0;
				if (c < 0x80) { cp = c; extra = 0; }
				else if ((c >> 5) == 0x6) { cp = c & 0x1Fu; extra = 1; }
				else if ((c >> 4) == 0xE) { cp = c & 0x0Fu; extra = 2; }
				else if ((c >> 3) == 0x1E) { cp = c & 0x07u; extra = 3; }
				else { ++i; continue; }
				++i;
				for (int k = 0; k < extra && i < s.size(); ++k, ++i)
				{
					cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
				}
				out.push_back(cp);
			}
			return out;
		}

		const int kModCtrl = ImGuiMod_Ctrl; // documented modifier path (1.92)
		const int kKeyA = ImGuiKey_A;
	} // namespace

	void UiInputScript::PushFrame(std::vector<SynEvent> frame)
	{
		m_frameBoundaries.push_back(m_steps.size());
		for (const SynEvent& e: frame)
		{
			m_steps.push_back(e);
		}
		m_frameBoundaries.push_back(m_steps.size());
	}

	void UiInputScript::QueueHover(float x, float y)
	{
		m_hover = std::make_pair(x, y);
	}

	void UiInputScript::ClearHover()
	{
		m_hover.reset();
	}

	void UiInputScript::QueueClick(float x, float y, int button, bool doubleClick)
	{
		// The click's own MousePos frames establish hover; no persistent hold.
		const int cycles = doubleClick ? 2 : 1;
		PushFrame({SynEvent{SynKind::MousePos, x, y}});
		for (int c = 0; c < cycles; ++c)
		{
			// Re-assert the position on EVERY frame (down and up). Without it the
			// GLFW backend re-posts the real cursor that frame and the release
			// lands off-target, so release-triggered widgets never fire.
			PushFrame({SynEvent{SynKind::MousePos, x, y}, SynEvent{SynKind::MouseButton, x, y, button, true}});
			PushFrame({SynEvent{SynKind::MousePos, x, y}, SynEvent{SynKind::MouseButton, x, y, button, false}});
		}
	}

	void UiInputScript::QueueDrag(float fromX, float fromY, float toX, float toY, int button)
	{
		// Enough intermediate frames that ImGui's drag threshold is crossed well before the
		// release, and that a target under the cursor gets a frame to notice it is hovered.
		constexpr int kMoveFrames = 8;

		// Several position-only frames before the press. ImGui resolves which window is
		// hovered at the START of a frame, and a widget that only reacts to a press while
		// hovered - a node editor's pins - sees nothing if the press arrives on the same
		// frame the cursor first appears there.
		constexpr int kSettleFrames = 3;
		for (int i = 0; i < kSettleFrames; ++i)
		{
			PushFrame({SynEvent{SynKind::MousePos, fromX, fromY}});
		}
		PushFrame({SynEvent{SynKind::MousePos, fromX, fromY}, SynEvent{SynKind::MouseButton, fromX, fromY, button, true}});
		for (int i = 1; i <= kMoveFrames; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(kMoveFrames);
			const float x = fromX + (toX - fromX) * t;
			const float y = fromY + (toY - fromY) * t;
			// The button is re-asserted every frame for the same reason QueueClick re-asserts
			// the position: the backend re-posts real input each frame otherwise.
			PushFrame({SynEvent{SynKind::MousePos, x, y}, SynEvent{SynKind::MouseButton, x, y, button, true}});
		}
		// And hold at the destination before releasing, for the same reason at the other end:
		// the drop target has to be hovered on the frame the button comes up.
		for (int i = 0; i < kSettleFrames; ++i)
		{
			PushFrame({SynEvent{SynKind::MousePos, toX, toY}, SynEvent{SynKind::MouseButton, toX, toY, button, true}});
		}
		PushFrame({SynEvent{SynKind::MousePos, toX, toY}, SynEvent{SynKind::MouseButton, toX, toY, button, false}});
		PushFrame({SynEvent{SynKind::MousePos, toX, toY}});
	}

	void UiInputScript::QueueKey(int imguiKey)
	{
		PushFrame({SynEvent{SynKind::Key, 0.0f, 0.0f, 0, true, imguiKey}});
		PushFrame({SynEvent{SynKind::Key, 0.0f, 0.0f, 0, false, imguiKey}});
	}

	void UiInputScript::QueueText(std::string utf8)
	{
		// Select-all (Ctrl+A), hold a frame, release; then type. When an ImGui
		// InputText has a selection, typing the first character replaces it, so
		// no explicit Delete is needed. The modifier must be pressed BEFORE 'A'
		// and stay held while 'A' is pressed for the shortcut to register.
		PushFrame({SynEvent{SynKind::Key, 0.0f, 0.0f, 0, true, kModCtrl}});
		PushFrame({SynEvent{SynKind::Key, 0.0f, 0.0f, 0, true, kModCtrl}, SynEvent{SynKind::Key, 0.0f, 0.0f, 0, true, kKeyA}});
		PushFrame({SynEvent{SynKind::Key, 0.0f, 0.0f, 0, false, kKeyA}, SynEvent{SynKind::Key, 0.0f, 0.0f, 0, false, kModCtrl}});
		std::vector<SynEvent> chars;
		for (const unsigned int cp: DecodeUtf8(utf8))
		{
			chars.push_back(SynEvent{SynKind::Char, 0.0f, 0.0f, 0, false, 0, cp});
		}
		PushFrame(std::move(chars));
	}

	std::vector<SynEvent> UiInputScript::Step()
	{
		std::vector<SynEvent> out;
		// The held hover only re-asserts between actions. While an action plays,
		// its own MousePos events own the cursor - otherwise the hover would snap
		// the mouse away mid-click and break release-triggered widgets.
		if (m_hover.has_value() && !Busy())
		{
			out.push_back(SynEvent{SynKind::MousePos, m_hover->first, m_hover->second});
		}
		if (Busy())
		{
			const std::size_t begin = m_frameBoundaries[m_cursor];
			const std::size_t end = m_frameBoundaries[m_cursor + 1];
			for (std::size_t i = begin; i < end; ++i)
			{
				out.push_back(m_steps[i]);
			}
			m_cursor += 2;
			if (m_cursor >= m_frameBoundaries.size())
			{
				m_steps.clear();
				m_frameBoundaries.clear();
				m_cursor = 0;
			}
		}
		return out;
	}

	bool UiInputScript::Busy() const
	{
		return m_cursor < m_frameBoundaries.size();
	}
} // namespace aether::app
