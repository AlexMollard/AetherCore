#include "LayerStack.hpp"

#include <ranges>

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
		for (auto& m_layer: std::views::reverse(m_layers))
		{
			m_layer->OnDetach(context);
		}
	}

	void LayerStack::UpdateAll(LayerContext& context)
	{
		for (const auto& layer: m_layers)
		{
			AE_PROFILE_ZONE();
			layer->OnUpdate(context);
		}
	}

	void LayerStack::ImGuiAll(LayerContext& context)
	{
		for (const auto& layer: m_layers)
		{
			AE_PROFILE_ZONE();
			layer->OnImGui(context);
		}
	}
} // namespace aether::app
