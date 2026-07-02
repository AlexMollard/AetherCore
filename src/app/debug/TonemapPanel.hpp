#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::app
{

class TonemapPanel final : public DebugPanel
{
public:
	std::string_view GetName() const override
	{
		return "Tonemap";
	}

	void OnImGui(LayerContext& context) override;
};

} // namespace aether::app
