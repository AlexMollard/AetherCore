#include <optional>
#include "debug/PostProcessingPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "rendering/Renderer.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr const char* kCullModeSettingKey = "debug.scene_cull_override_index";
	}

	void PostProcessingPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Post Processing", VisiblePtr());
		chrome::PanelHeader("POST PROCESSING");
		{
			Renderer& renderer = context.Get<Renderer>();

			// Entry 0 is no override at all, which is the default: a two-sided material has to
			// be able to turn its own culling off, and it cannot if this forces one on everything.
			const std::optional<gpu::CullMode> current = renderer.GetCullMode();
			int cullMode = current ? static_cast<int>(*current) + 1 : 0;
			const char* const cullModeNames[] = {"Per material", "None", "Front", "Back", "Front + Back"};
			if (ImGui::Combo("Cull mode", &cullMode, cullModeNames, static_cast<int>(std::size(cullModeNames))))
			{
				renderer.SetCullMode(cullMode == 0 ? std::nullopt
				                                  : std::optional<gpu::CullMode>(static_cast<aether::gpu::CullMode>(cullMode - 1)));
			}

			const gpu::Extent2D ext = context.Get<Swapchain>().GetExtent();
			ImGui::Text("Resolution: %u x %u", ext.width, ext.height);
		}
		ImGui::End();
	}

	void PostProcessingPanel::LoadSettings(TomlConfig& config, app::LayerContext& context)
	{
		// Stored as the combo index: 0 is "no override", 1..4 are the cull modes. The default
		// is 0 - a persisted Back here is what silently disabled every two-sided material.
		constexpr int kMaxIndex = static_cast<int>(aether::gpu::CullMode::FrontAndBack) + 1;
		int cullMode = static_cast<int>(config.GetFloat(kCullModeSettingKey, 0.0f));
		cullMode = std::clamp(cullMode, 0, kMaxIndex);
		context.Get<Renderer>().SetCullMode(cullMode == 0 ? std::nullopt
		                                                  : std::optional<gpu::CullMode>(static_cast<aether::gpu::CullMode>(cullMode - 1)));
	}

	void PostProcessingPanel::SaveSettings(TomlConfig& config, app::LayerContext& context) const
	{
		const std::optional<gpu::CullMode> mode = context.Get<Renderer>().GetCullMode();
		config.Set(kCullModeSettingKey, static_cast<float>(mode ? static_cast<int>(*mode) + 1 : 0));
	}
} // namespace aether::editor
