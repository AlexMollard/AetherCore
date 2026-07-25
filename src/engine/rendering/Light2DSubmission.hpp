#pragma once

#include <vector>

#include "rendering/RenderFramePacket.hpp"
#include "rendering/Renderer.hpp"

namespace aether
{
	// Game-thread collector for 2D lights and shadow occluders submitted by SCRIPTS each frame, with no
	// ECS entity behind them - for effects that are transient and data-driven (a drawn ink stroke, a
	// projectile trail, a spell). PrepareFrame drains these into the render packet and clears them, so a
	// script that stops submitting simply stops lighting.
	//
	// Registered as a service so the app-side interop and the engine-side extraction share one instance.
	// The engine stays game-agnostic: it only knows "a light here" and "a capsule occludes here"; what
	// they mean lives in the project's scripts.
	struct Light2DSubmissionRegistry
	{
		std::vector<Renderer::PointLight> lights; // appended to packet.pointLights
		std::vector<Occluder2D> occluders;        // appended to packet.render2D.occluders

		void Clear()
		{
			lights.clear();
			occluders.clear();
		}
	};
} // namespace aether
