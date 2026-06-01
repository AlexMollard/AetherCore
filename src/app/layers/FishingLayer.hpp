#pragma once

#include "AppLayer.hpp"
#include "camera/CameraManager.hpp"

namespace aether::app
{
	class FishingGameSystem;

	class FishingLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		const char* GetActiveCameraName(aether::CameraHandle activeCamera) const;

		FishingGameSystem* m_gameSystem = nullptr;

		// Cached display values updated in OnUpdate
		std::size_t m_fishCount = 0;
		std::size_t m_score = 0;
		const char* m_bobberState = "";
		aether::CameraHandle m_activeCamera;
	};
} // namespace aether::app
