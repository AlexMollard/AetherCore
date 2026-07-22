#pragma once

#include <string>
#include <unordered_map>

#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	// Game-thread registry + submission queue for project-registered custom render passes. A project
	// registers a pass once (name -> shader + stage), then submits its data buffer + params each frame;
	// PrepareFrame snapshots the frame's submissions into the render packet (RenderCustomPassData) and
	// clears them. Both the script update and the extraction run on the game/producer thread in
	// sequence, so no locking is needed.
	//
	// Registered as a service so the app-side interop and the engine-side extraction share one instance.
	// The engine stays entirely game-agnostic: it only knows "run this project shader over a fullscreen
	// pass with this float4 buffer bound" - what the pass means lives in the project's shader + scripts.
	struct CustomPassRegistry
	{
		struct Registration
		{
			std::string shader;                                // project shader stem
			CustomPassStage stage = CustomPassStage::OverScene2D;
		};

		std::unordered_map<std::string, Registration> registered; // pass name -> registration
		RenderCustomPassData frame;                               // submissions this frame (cleared after extract)
	};
} // namespace aether
