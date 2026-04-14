#pragma once

#include <glm/glm.hpp>

#include "AppLayer.hpp"
#include "UIRenderer.hpp"

namespace aether::app
{
	class UILayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

		[[nodiscard]] aether::UIRenderer& GetUIRenderer() { return m_uiRenderer; }

	private:
		aether::UIRenderer m_uiRenderer;
	};
}

