#include "FishingLayer.hpp"

#include <cstdio>

#include "Logger.hpp"
#include "OverlayStyle.hpp"
#include "systems/FishingGameSystem.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"

namespace aether::app
{
    namespace
    {
        using namespace overlay;

        constexpr glm::vec2 kAnchor{0.0f, 0.0f};
        constexpr float kPanelL = 12.0f;
        constexpr float kPanelR = 460.0f;
        constexpr float kPanelTop = 12.0f;
        constexpr float kPanelBot = 320.0f;
        constexpr float kInnerL = kPanelL + kPad;
        constexpr float kInnerR = kPanelR - kPad;
        constexpr float kColKey = kInnerL;
        constexpr float kColVal = kInnerL + 110.0f;
    }

    const char* FishingLayer::GetActiveCameraName(aether::CameraHandle activeCamera) const
    {
        if (!activeCamera.IsValid())
            return "None";

        return "Fishing";
    }

    void FishingLayer::OnAttach(LayerContext& context)
    {
        INFO(LogCategory::App, "Fishing layer attached.");

        auto gameSystem = std::make_unique<FishingGameSystem>();
        gameSystem->Init(context.engine, *context.assets, *context.cameras, *context.input);
        m_gameSystem = gameSystem.get();
        context.world->RegisterSystem(std::move(gameSystem));
    }

    void FishingLayer::OnDetach(LayerContext& context)
    {
        context.world->UnregisterSystem("FishingGameSystem");
        m_gameSystem = nullptr;
        (void)context;
    }

    void FishingLayer::OnUpdate(LayerContext& context)
    {
        (void)context;
    }

    void FishingLayer::OnGui(LayerContext& context)
    {
        if (context.ui == nullptr)
            return;

        aether::UIRenderer& ui = *context.ui;
        std::array<char, 128> buf{};

        DrawPanel(ui, kAnchor, kPanelL, kPanelR, kPanelTop, kPanelBot);
        ui.DrawText("FISHING DEMO",
                    aether::UiPoint{.anchor = kAnchor, .offsetPx = {kInnerL, kPanelTop + 26.0f}},
                    18.0f,
                    kColorTitle);

        DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kPanelTop + 54.0f);

        const float sceneY = kPanelTop + 66.0f;
        DrawSectionHeader(ui, "GAME", kAnchor, kPanelL, kInnerL, sceneY);
        DrawSeparator(ui, kAnchor, kInnerL, kInnerR, sceneY + 13.0f);

        float rowY = sceneY + 34.0f;
        const char* cameraName = GetActiveCameraName(context.cameras->GetMainCamera());
        DrawKV(ui, "Camera", cameraName, kAnchor, kColKey, kColVal, rowY);
        rowY += kRowH;

        if (m_gameSystem)
        {
            const auto fishCount = m_gameSystem->GetFishCount();
            std::snprintf(buf.data(), buf.size(), "%zu", fishCount);
            DrawKV(ui, "Fish", buf.data(), kAnchor, kColKey, kColVal, rowY);
            rowY += kRowH;

            std::snprintf(buf.data(), buf.size(), "%zu", m_gameSystem->GetScore());
            DrawKV(ui, "Score", buf.data(), kAnchor, kColKey, kColVal, rowY);
            rowY += kRowH;

            DrawKV(ui, "Bobber", m_gameSystem->GetBobberStateName(), kAnchor, kColKey, kColVal, rowY);
            rowY += kRowH;
        }

        const float controlsY = rowY + kRowH;
        DrawSectionHeader(ui, "CONTROLS", kAnchor, kPanelL, kInnerL, controlsY);
        DrawSeparator(ui, kAnchor, kInnerL, kInnerR, controlsY + 13.0f);

        float controlRow = controlsY + 34.0f;
        DrawKV(ui, "LMB", "Cast to water", kAnchor, kColKey, kColVal, controlRow);
        controlRow += kRowH;
        DrawKV(ui, "Space", "Hook / Reel", kAnchor, kColKey, kColVal, controlRow);
        controlRow += kRowH;
        DrawKV(ui, "RMB", "Rotate camera", kAnchor, kColKey, kColVal, controlRow);
        controlRow += kRowH;
        DrawKV(ui, "WASD", "Move camera", kAnchor, kColKey, kColVal, controlRow);
    }
} // namespace aether::app
