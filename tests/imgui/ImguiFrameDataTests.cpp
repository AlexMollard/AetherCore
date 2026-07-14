#include <doctest/doctest.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "imgui/ImguiFrameData.hpp"

using namespace aether;

// The render thread consumes a cloned snapshot of ImGui's draw data. Those
TEST_CASE("ImguiFrameData clones do not attach to the live context draw-list registry")
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	{
		ImDrawListSharedData& shared = ImGui::GetCurrentContext()->DrawListSharedData;

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
		CHECK(shared.DrawLists.Size == registeredBeforeCapture);
	}

	// Everything the test registered is scoped out; destroying the context must
	ImGui::DestroyContext();
}
