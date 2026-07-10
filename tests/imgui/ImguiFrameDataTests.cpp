#include <doctest/doctest.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "imgui/ImguiFrameData.hpp"

using namespace aether;

// The render thread consumes a cloned snapshot of ImGui's draw data. Those
// clones are pooled and kept warm in a process-lifetime static pool, so they
// outlive ImGui::DestroyContext(). If a clone registers itself in the live
// context's ImDrawListSharedData::DrawLists registry (which every ImDrawList
// built from that shared data does), ~ImDrawListSharedData aborts on
// `IM_ASSERT(DrawLists.Size == 0)` when the context is destroyed at shutdown.
//
// So a clone must be a self-contained data carrier that never attaches to the
// live context.
TEST_CASE("ImguiFrameData clones do not attach to the live context draw-list registry")
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	{
		ImDrawListSharedData& shared = ImGui::GetCurrentContext()->DrawListSharedData;

		// A source draw list as ImGui would hand us: this one legitimately
		// registers with the context.
		ImDrawList source(&shared);

		ImDrawData data;
		data.Clear();
		data.Valid = true;
		data.CmdListsCount = 1;
		data.CmdLists.resize(1);
		data.CmdLists[0] = &source;

		ImguiFrameData frame;
		const int registeredBeforeCapture = shared.DrawLists.Size;
		frame.Capture(&data);

		// Cloning the source into the frame's pool must add nothing to the
		// context registry - the clone is independent of the live context.
		CHECK(shared.DrawLists.Size == registeredBeforeCapture);
	}

	// Everything the test registered is scoped out; destroying the context must
	// not trip the leaked-draw-list assertion.
	ImGui::DestroyContext();
}
