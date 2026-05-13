#pragma once

#include "AppLayer.hpp"

namespace aether::app
{

	// Full-screen loading overlay drawn with the engine UI system.
	// Displays progress while LoadingManager has pending tasks,
	// then becomes a no-op once loading is complete.
	class LoadingLayer final : public AppLayer
	{
	public:
		void OnGui(LayerContext& context) override;
	};

} // namespace aether::app
