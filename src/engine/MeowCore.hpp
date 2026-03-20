#pragma once

#include "VulkanContext.hpp"
#include "Window.hpp"

namespace meow
{
	class MeowCore
	{
	public:
		struct Config
		{
			const char* appName = "MeowCore";
			int width = 1280;
			int height = 720;
		};

		explicit MeowCore(const Config& config = {});

		[[nodiscard]] bool ShouldClose() const;
		void PumpEvents() const;
		void BeginFrame();
		void EndFrame();

		[[nodiscard]] Window& GetWindow();
		[[nodiscard]] const Window& GetWindow() const;
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] const VulkanContext& GetVulkanContext() const;

	private:
		Window m_window;
		VulkanContext m_vulkanContext;
	};
}