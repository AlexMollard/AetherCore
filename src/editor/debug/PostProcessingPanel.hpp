#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::editor
{

	class PostProcessingPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Post Processing";
		}

		void OnImGui(app::LayerContext& context) override;
	};

} // namespace aether::editor
