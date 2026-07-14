#include "camera/CameraSubsystem.hpp"

#include "gpu/GpuDevice.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void CameraSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& gpu = services.Get<GpuDevice>();
		auto& vk = services.Get<VulkanContext>();
		// descriptor-set-layout helper, and the VulkanContext for direct
		m_lightingManager.Initialize(gpu, vk);
	}

	void CameraSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_lightingManager.Shutdown();
	}
} // namespace aether
