#pragma once

#include "AppLayer.hpp"

namespace aether::app
{
	// Minimal loading overlay. The script toggles visibility via
	// set_loading_visible(true/false) and does its own loading.
	class LoadingLayer final : public AppLayer
	{
	public:
		void OnGui(LayerContext& context) override;

		void SetVisible(bool visible)
		{
			m_visible = visible;
		}

		[[nodiscard]] bool IsVisible() const
		{
			return m_visible;
		}

	private:
		bool m_visible = false;
	};
} // namespace aether::app
