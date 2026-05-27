#include "camera/CameraSubsystem.hpp"

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void CameraSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = services.Get<VulkanContext>();
		// LightingManager needs just the Vulkan context to set up descriptor
		// layouts. The Renderer pointer is linked later via LinkRenderer()
		// after the RenderingSubsystem is initialized.
		m_lightingManager.Initialize(vk);
	}

	void CameraSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_lightingManager.Shutdown();
	}
} // namespace aether
