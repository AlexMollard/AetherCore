#pragma once

#include <memory>
#include <vector>

#include "AppLayer.hpp"

namespace aether::app
{
	class LayerStack
	{
	public:
		void Push(std::unique_ptr<AppLayer> layer);
		void AttachAll(LayerContext& context);
		void DetachAll(LayerContext& context);
		void UpdateAll(LayerContext& context);
		void GuiAll(LayerContext& context);

	private:
		std::vector<std::unique_ptr<AppLayer>> m_layers;
	};
}