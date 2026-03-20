#include "LayerStack.hpp"

namespace meow::app
{
	void LayerStack::Push(std::unique_ptr<AppLayer> layer)
	{
		m_layers.push_back(std::move(layer));
	}

	void LayerStack::AttachAll(LayerContext& context)
	{
		for (const auto& layer : m_layers)
		{
			layer->OnAttach(context);
		}
	}

	void LayerStack::DetachAll(LayerContext& context)
	{
		for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it)
		{
			(*it)->OnDetach(context);
		}
	}

	void LayerStack::UpdateAll(LayerContext& context)
	{
		for (const auto& layer : m_layers)
		{
			layer->OnUpdate(context);
		}
	}

	void LayerStack::GuiAll(LayerContext& context)
	{
		for (const auto& layer : m_layers)
		{
			layer->OnGui(context);
		}
	}
}