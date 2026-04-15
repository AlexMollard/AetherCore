#pragma once

#include <memory>

#include "AetherCore.hpp"
#include "AppLayer.hpp"
#include "LayerStack.hpp"
#include "UIRenderer.hpp"

namespace aether::app
{
	class Application
	{
	public:
		explicit Application(const aether::AetherCore::Config& engineConfig = {});
		~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		void PushLayer(std::unique_ptr<AppLayer> layer);
		int Run();

		[[nodiscard]] aether::AetherCore& GetEngine();
		[[nodiscard]] const aether::AetherCore& GetEngine() const;

	private:
		aether::AetherCore m_engine;
		aether::UIRenderer m_uiRenderer;
		LayerStack m_layers;
		bool m_layersAttached = false;
		std::uint64_t m_frameIndex = 0;
	};
} // namespace aether::app
