#pragma once

#include "layers/AppLayer.hpp"

namespace aether::app
{
	// background thread and returns immediately with the session in the Compiling
	bool StartPlaySession(LayerContext& context);

	bool StopPlaySession(LayerContext& context);

	bool TogglePlaySession(LayerContext& context);

	// in any other state. Must be called once per frame from the main thread.
	void UpdatePlaySession(LayerContext& context);
} // namespace aether::app
