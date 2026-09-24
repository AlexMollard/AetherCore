#pragma once

namespace aether::app
{
	struct LayerContext;
}

namespace aether::editor
{
	// What the light gizmos draw. These are editor debug visualisation, so the toggles live
	// with the other debug overlays in Dev Tools rather than in a window of their own - the
	// master "Debug overlay" switch they depend on was already there, which made a separate
	// "Lighting" window a second place to configure one thing.
	struct LightGizmoSettings
	{
		bool enabled = true;
		bool pointVolumes = false;
		bool spotCones = true;
		bool sunDirection = true;
		bool shadowMarkers = true;
		float scale = 1.0f;
	};

	// Queues this frame's light gizmos. Self-suppresses while the game is playing (unless
	// editor gizmos in play are enabled) and when the debug overlay is off.
	void DrawLightGizmos(app::LayerContext& context, const LightGizmoSettings& settings);
} // namespace aether::editor
