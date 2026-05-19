#include "LayerStack.hpp"

#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	void LayerStack::Push(std::unique_ptr<AppLayer> layer)
	{
		m_layers.push_back(std::move(layer));
	}

	void LayerStack::AttachAll(LayerContext& context)
	{
		AE_INFO(LogCategory::App, "Attaching layers.");
		for (const auto& layer: m_layers)
		{
			layer->OnAttach(context);
		}
	}

	void LayerStack::DetachAll(LayerContext& context)
	{
		AE_INFO(LogCategory::App, "Detaching layers.");
		for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it)
		{
			(*it)->OnDetach(context);
		}
	}

	void LayerStack::UpdateAll(LayerContext& context)
	{
		for (const auto& layer: m_layers)
		{
			AE_PROFILE_ZONE_N("Layer::Update");
			layer->OnUpdate(context);
		}
	}

	void LayerStack::GuiAll(LayerContext& context)
	{
		for (const auto& layer: m_layers)
		{
			AE_PROFILE_ZONE_N("Layer::Gui");
			layer->OnGui(context);
		}
	}
} // namespace aether::app
