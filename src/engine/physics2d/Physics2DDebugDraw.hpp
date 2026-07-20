#pragma once

#include <vector>

#include "physics/PhysicsDebugRenderer.hpp" // DebugVertex + enable toggles

namespace aether
{
	class World;

	// Game-thread extraction of 2D collider wireframes into the immediate-mode
	// debug line channel (RenderFramePacket::debugVertices). Gated by the same
	// physics-debug-shapes toggle the 3D shapes use; the render thread only ever
	// sees the packet data. Colors: triggers green, static blue, kinematic cyan,
	// dynamic yellow (grey when sleeping).
	void ExtractPhysics2DDebugLines(const World& world, std::vector<DebugVertex>& out);
} // namespace aether
