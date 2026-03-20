#include "SandboxLayer.hpp"

#include "Logger.hpp"

namespace meow::app
{
	void SandboxLayer::OnAttach(LayerContext& context)
	{
		(void)context;
		INFO(LogCategory::App, "Sandbox layer attached.");
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		(void)context;
	}
}