#pragma once

#include <string_view>

namespace aether::app
{
	// Called before runtime initialization; takes ownership only of a valid readiness socket.
	[[nodiscard]] bool InitializeEditorReadiness(std::string_view descriptor);
	void SignalEditorReady();
} // namespace aether::app
