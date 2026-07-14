#pragma once

#include "camera/CameraManager.hpp"
#include "rendering/LightingManager.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	class CameraSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown();

		[[nodiscard]] CameraManager& GetCameraManager()
		{
			return m_cameraManager;
		}

		[[nodiscard]] LightingManager& GetLightingManager()
		{
			return m_lightingManager;
		}

	private:
		CameraManager m_cameraManager;
		LightingManager m_lightingManager;
	};
} // namespace aether
