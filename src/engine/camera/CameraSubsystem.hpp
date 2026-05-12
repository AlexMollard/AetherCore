#pragma once

#include "camera/CameraManager.hpp"
#include "camera/LightingManager.hpp"

class ServiceContainer;

namespace aether
{
	// Owns the camera manager and lighting manager.
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
