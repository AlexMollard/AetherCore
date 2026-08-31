#pragma once

#include "layers/AppLayer.hpp"

namespace aether::app
{
	// background thread and returns immediately with the session in the Compiling
	bool StartPlaySession(LayerContext& context);

	bool StopPlaySession(LayerContext& context);

	bool TogglePlaySession(LayerContext& context);

	// Freeze / unfreeze the running simulation. Only meaningful while Playing;
	// returns false (no-op) otherwise. TogglePausePlaySession flips the state.
	bool PausePlaySession(LayerContext& context);

	bool ResumePlaySession(LayerContext& context);

	bool TogglePausePlaySession(LayerContext& context);

	// Advance the simulation by exactly one frame. Auto-pauses first if the
	// session is playing but not yet paused, so a single call always freezes
	// after stepping. Returns false when not playing.
	// Advance the paused simulation by `frames` frames. A count, not a flag, so a capture
	// can be taken at a known frame - see PlayState::RequestStep.
	bool StepPlaySession(LayerContext& context, int frames = 1);

	// in any other state. Must be called once per frame from the main thread.
	void UpdatePlaySession(LayerContext& context);
} // namespace aether::app
