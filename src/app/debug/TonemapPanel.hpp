#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::editor
{

	class TonemapPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Tonemap";
		}

		void OnImGui(app::LayerContext& context) override;
	};

} // namespace aether::editor
