#pragma once

#include "layers/AppLayer.hpp"

namespace aether::app
{
	bool StartPlaySession(LayerContext& context);
	bool StopPlaySession(LayerContext& context);
	bool TogglePlaySession(LayerContext& context);
} // namespace aether::app
