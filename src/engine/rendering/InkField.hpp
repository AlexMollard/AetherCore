#pragma once

#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	// Game-thread accumulation buffer for the conjured-ink field. Scripts push their live
	// stroke segments here each frame through the Ink interop; PrepareFrame copies it into the
	// render packet (RenderInkFrameData) for the ink pass. Both the script update and the
	// extraction run on the game/producer thread in sequence, so no locking is needed.
	//
	// Registered as a service so the app-side interop and the engine-side extraction share one
	// instance without either reaching into the other's internals.
	struct InkField
	{
		RenderInkFrameData data;
	};
} // namespace aether
