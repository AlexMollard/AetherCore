#include "debug/PostProcessingPanel.hpp"

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	void PostProcessingPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Post Processing");
		{
			Renderer& renderer = context.Get<Renderer>();
			auto& rendering = context.Get<aether::RenderingSubsystem>();

			int tonemapMode = static_cast<int>(renderer.GetTonemapMode());
			const char* tonemapNames[] = {"Reinhard", "ACES Filmic", "Uncharted2"};
			if (ImGui::Combo("Tonemap", &tonemapMode, tonemapNames, static_cast<int>(std::size(tonemapNames))))
			{
				renderer.SetTonemapMode(static_cast<aether::TonemapMode>(tonemapMode));
			}

			bool fxaa = renderer.IsFxaaEnabled();
			if (ImGui::Checkbox("FXAA", &fxaa))
			{
				renderer.SetFxaaEnabled(fxaa);
			}

			float exposure = rendering.GetPostProcessStack().GetExposure();
			if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 4.0f, "%.2f"))
			{
				rendering.GetPostProcessStack().SetExposure(exposure);
			}

			const gpu::Extent2D ext = context.Get<Swapchain>().GetExtent();
			ImGui::Text("Resolution: %u x %u", ext.width, ext.height);
		}
		ImGui::End();
	}

	void PostProcessingPanel::OnUpdate(LayerContext& context)
	{
		const Input& input = context.Get<Input>();

		if (input.IsKeyPressed(aether::Key::F))
		{
			const bool enabled = !context.Get<Renderer>().IsFxaaEnabled();
			context.Get<Renderer>().SetFxaaEnabled(enabled);
			AE_INFO(aether::LogCategory::App, "FXAA: {}", enabled ? "on" : "off");
		}

		if (input.IsKeyPressed(aether::Key::T))
		{
			const auto next = static_cast<aether::TonemapMode>((static_cast<int>(context.Get<Renderer>().GetTonemapMode()) + 1) % 3);
			context.Get<Renderer>().SetTonemapMode(next);

			auto TonemapModeName = [](aether::TonemapMode mode) -> const char*
			{
				switch (mode)
				{
					case aether::TonemapMode::Reinhard:
						return "Reinhard";
					case aether::TonemapMode::AcesFilmic:
						return "ACES Filmic";
					case aether::TonemapMode::Uncharted2:
						return "Uncharted2";
					default:
						return "Unknown";
				}
			};

			AE_INFO(aether::LogCategory::App, "Tonemap: {}", TonemapModeName(next));
		}
	}

	void PostProcessingPanel::LoadSettings(TomlConfig& /*config*/, LayerContext& /*context*/)
	{
	}

	void PostProcessingPanel::SaveSettings(TomlConfig& /*config*/, LayerContext& /*context*/) const
	{
	}
} // namespace aether::app
