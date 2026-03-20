#pragma once

#include "AppLayer.hpp"

namespace meow::app
{
	class SandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
	};
}