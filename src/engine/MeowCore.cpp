#include "MeowCore.hpp"

#include "Logger.hpp"

namespace meow
{
	MeowCore::MeowCore(const Config& config)
		: m_window(config.appName, config.width, config.height),
		m_vulkanContext(m_window, config.appName)
	{
		INFO(LogCategory::Engine, "Engine core initialized.");
	}

	bool MeowCore::ShouldClose() const
	{
		return m_window.ShouldClose();
	}

	void MeowCore::PumpEvents() const
	{
		m_window.PollEvents();
	}

	void MeowCore::BeginFrame()
	{
		// Frame hooks will route through here as rendering systems are expanded.
	}

	void MeowCore::EndFrame()
	{
	}

	Window& MeowCore::GetWindow()
	{
		return m_window;
	}

	const Window& MeowCore::GetWindow() const
	{
		return m_window;
	}

	VulkanContext& MeowCore::GetVulkanContext()
	{
		return m_vulkanContext;
	}

	const VulkanContext& MeowCore::GetVulkanContext() const
	{
		return m_vulkanContext;
	}
}