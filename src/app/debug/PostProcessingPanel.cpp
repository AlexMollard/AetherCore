#include "debug/PostProcessingPanel.hpp"

#include <algorithm>
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
	namespace
	{
		constexpr const char* kCullModeSettingKey = "debug.scene_cullmode";
	} // namespace

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

			int cullMode = static_cast<int>(renderer.GetCullMode());
			const char* cullModeNames[] = {"None", "Front", "Back", "Front + Back"};
			if (ImGui::Combo("Cull mode", &cullMode, cullModeNames, static_cast<int>(std::size(cullModeNames))))
			{
				renderer.SetCullMode(static_cast<aether::gpu::CullMode>(cullMode));
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

	void PostProcessingPanel::LoadSettings(TomlConfig& config, LayerContext& context)
	{
		constexpr int kMinCullMode = static_cast<int>(aether::gpu::CullMode::None);
		constexpr int kMaxCullMode = static_cast<int>(aether::gpu::CullMode::FrontAndBack);
		int cullMode = static_cast<int>(config.GetFloat(kCullModeSettingKey, static_cast<float>(static_cast<int>(aether::gpu::CullMode::Back))));
		cullMode = std::clamp(cullMode, kMinCullMode, kMaxCullMode);
		context.Get<Renderer>().SetCullMode(static_cast<aether::gpu::CullMode>(cullMode));
	}

	void PostProcessingPanel::SaveSettings(TomlConfig& config, LayerContext& context) const
	{
		config.Set(kCullModeSettingKey, static_cast<float>(static_cast<int>(context.Get<Renderer>().GetCullMode())));
	}
} // namespace aether::app
