#pragma once

#include <memory>

#include "AppLayer.hpp"
#include "LayerStack.hpp"
#include "MeowCore.hpp"

namespace meow::app
{
	class Application
	{
	public:
		explicit Application(const meow::MeowCore::Config& engineConfig = {});
		~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		void PushLayer(std::unique_ptr<AppLayer> layer);
		int Run();

		[[nodiscard]] meow::MeowCore& GetEngine();
		[[nodiscard]] const meow::MeowCore& GetEngine() const;

	private:
		meow::MeowCore m_engine;
		LayerStack m_layers;
		bool m_layersAttached = false;
		std::uint64_t m_frameIndex = 0;
	};
}