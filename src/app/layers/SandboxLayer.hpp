#pragma once

#include <string>

#include "AppLayer.hpp"
#include "camera/CameraManager.hpp"
#include "scene/Entity.hpp"

namespace aether::app
{
	class SandboxGameSystem;

	class SandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		[[nodiscard]] const char* GetActiveCameraName(aether::CameraHandle activeCamera) const;

		SandboxGameSystem* m_gameSystem = nullptr;

		// Cached display values updated in OnUpdate
		std::size_t m_foxCount = 0;
		std::size_t m_primCount = 0;
		unsigned m_animCount = 0;
		std::string m_animName;
		aether::CameraHandle m_activeCamera;
	};
} // namespace aether::app
