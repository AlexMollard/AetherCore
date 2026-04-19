#pragma once

#include "AppLayer.hpp"
#include "CameraManager.hpp"

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
    };
} // namespace aether::app
