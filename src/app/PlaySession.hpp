#pragma once

#include "layers/AppLayer.hpp"

namespace aether::app
{
	// Enter Play. If the C# scripts need rebuilding this kicks the build off on a
	// background thread and returns immediately with the session in the Compiling
	// state; UpdatePlaySession completes the transition to Playing once the build
	// finishes. The editor never blocks. Returns true if a play/compile was
	// initiated, false if already playing/compiling or state is unavailable.
	bool StartPlaySession(LayerContext& context);

	// Exit Play (restoring the pre-play snapshot), or cancel a pending Compile.
	bool StopPlaySession(LayerContext& context);

	bool TogglePlaySession(LayerContext& context);

	// Per-frame pump for the async play transition: while Compiling, polls the
	// background build and, on success, loads the scripts + snapshots the scene +
	// enters Playing; on failure, reports the error and returns to Editing. A no-op
	// in any other state. Must be called once per frame from the main thread.
	void UpdatePlaySession(LayerContext& context);
} // namespace aether::app
