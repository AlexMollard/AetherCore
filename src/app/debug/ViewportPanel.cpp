#include "ViewportPanel.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include "AetherCore.hpp"
#include "debug/DebugPanel.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	void ViewportPanel::OnAttach(LayerContext& context)
	{
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, true);
	}

	void ViewportPanel::OnDetach(LayerContext& context)
	{
		ReleaseSceneViewportTexture(context);
		context.Get<Input>().ClearMouseViewportTransform();
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, false);
	}

	void ViewportPanel::OnRenderTargetsInvalidated(LayerContext& context)
	{
		// The post-process final-color image (and its view) were just destroyed
		// and recreated. Drop our cached ImGui descriptor AND cached view so the
		// lazy re-register in OnImGui fires next frame even if the allocator
		// recycled the same VkImageView handle value for the new image.
		ReleaseSceneViewportTexture(context);
	}

	void ViewportPanel::ReleaseSceneViewportTexture(LayerContext& context)
	{
		if (m_sceneViewportTextureId != 0)
		{
			if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_sceneViewportTextureId));
			}
		}

		m_sceneViewportTextureId = 0;
		m_sceneViewportImageView = nullptr;
	}

	void ViewportPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin(GetName().data(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		auto& rendering = context.Get<aether::RenderingSubsystem>();
		auto& post = rendering.GetPostProcessStack();
		const gpu::ImageView imageView = post.GetFinalColorImageView();

		if (imageView != m_sceneViewportImageView)
		{
			ReleaseSceneViewportTexture(context);
			if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				const ImTextureID textureId = imgui->RegisterTexture(imageView, gpu::ImageLayout::ShaderReadOnly);
				if (textureId != ImTextureID_Invalid)
				{
					m_sceneViewportTextureId = static_cast<std::uint64_t>(textureId);
					m_sceneViewportImageView = imageView;
				}
			}
		}

		if (m_sceneViewportTextureId == 0)
		{
			context.Get<Input>().ClearMouseViewportTransform();
			ImGui::TextDisabled("Scene viewport texture unavailable");
			ImGui::End();
			return;
		}

		const ImGuiStyle& style = ImGui::GetStyle();
		const float settingsBarHeight = 1.0f + style.ItemSpacing.y * 2.0f + ImGui::GetFrameHeightWithSpacing();
		const ImVec2 available(ImGui::GetContentRegionAvail().x, std::max(1.0f, ImGui::GetContentRegionAvail().y - settingsBarHeight));

		gpu::Extent2D extent = post.GetExtent();
		if (extent.width == 0 || extent.height == 0)
		{
			extent = context.Get<Swapchain>().GetExtent();
		}

		const float renderAspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		float targetAspect = renderAspect;
		switch (m_viewportAspectMode)
		{
			case 1:
				targetAspect = available.y > 0.0f ? available.x / available.y : renderAspect;
				break;
			case 2:
				targetAspect = 16.0f / 9.0f;
				break;
			case 3:
				targetAspect = 16.0f / 10.0f;
				break;
			case 4:
				targetAspect = 4.0f / 3.0f;
				break;
			case 5:
				targetAspect = 1.0f;
				break;
			default:
				break;
		}

		ImVec2 imageSize = available;
		switch (m_viewportDisplayMode)
		{
			case 1: // Fill
				if (imageSize.x < imageSize.y * targetAspect)
				{
					imageSize.x = imageSize.y * targetAspect;
				}
				else
				{
					imageSize.y = imageSize.x / targetAspect;
				}
				break;
			case 2: // Actual
				imageSize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
				break;
			case 3: // Integer
			{
				const float sx = extent.width > 0 ? std::floor(available.x / static_cast<float>(extent.width)) : 1.0f;
				const float sy = extent.height > 0 ? std::floor(available.y / static_cast<float>(extent.height)) : 1.0f;
				const float scale = std::max(1.0f, std::min(sx, sy));
				imageSize = ImVec2(static_cast<float>(extent.width) * scale, static_cast<float>(extent.height) * scale);
				break;
			}
			default: // Fit
				if (m_viewportAspectMode != 1)
				{
					if (imageSize.x > imageSize.y * targetAspect)
					{
						imageSize.x = imageSize.y * targetAspect;
					}
					else
					{
						imageSize.y = imageSize.x / targetAspect;
					}
				}
				break;
		}

		if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
		{
			context.Get<Input>().SetMouseViewportInputActive(false);
			ImGui::End();
			return;
		}

		const ImVec2 imageCursorStart = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(imageCursorStart.x + (available.x - imageSize.x) * 0.5f, imageCursorStart.y + (available.y - imageSize.y) * 0.5f));

		ImGui::InvisibleButton("SceneViewportInput", imageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const ImVec2 imageMin = ImGui::GetItemRectMin();
		const ImVec2 imageMax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(m_sceneViewportTextureId)), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
		context.Get<Input>().SetMouseViewportInputActive(ImGui::IsItemHovered() || ImGui::IsItemActive());
		context.Get<Input>().SetMouseViewportTransform(glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, glm::vec2{static_cast<float>(extent.width), static_cast<float>(extent.height)});
		if (m_viewportShowStats || m_viewportShowMouse)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 pad(8.0f, 6.0f);
			std::string overlay;
			if (m_viewportShowStats)
			{
				overlay += std::format("Render: {} x {}\nView: {:.0f} x {:.0f}", extent.width, extent.height, imageSize.x, imageSize.y);
			}
			if (m_viewportShowMouse)
			{
				const glm::vec2 mouse = context.Get<Input>().GetMousePos();
				if (mouse.x > -999999.0f)
				{
					if (!overlay.empty())
					{
						overlay += "\n";
					}
					overlay += std::format("Mouse: {:.0f}, {:.0f}", mouse.x, mouse.y);
				}
			}
			if (!overlay.empty())
			{
				const ImVec2 textSize = ImGui::CalcTextSize(overlay.c_str());
				const ImVec2 rectMin(imageMin.x + 8.0f, imageMin.y + 8.0f);
				const ImVec2 rectMax(rectMin.x + textSize.x + pad.x * 2.0f, rectMin.y + textSize.y + pad.y * 2.0f);
				drawList->AddRectFilled(rectMin, rectMax, IM_COL32(22, 24, 28, 210), 4.0f);
				drawList->AddText(ImVec2(rectMin.x + pad.x, rectMin.y + pad.y), IM_COL32(235, 238, 242, 255), overlay.c_str());
			}
		}

		// Inline toolbar pinned below the image region
		ImGui::SetCursorPos(ImVec2(imageCursorStart.x, imageCursorStart.y + available.y));
		ImGui::Separator();

		SceneViewportSettings viewportSettings = rendering.GetSceneViewportSettings();
		bool viewportSettingsChanged = false;
		int resolutionMode = static_cast<int>(viewportSettings.resolutionMode);

		const char* resolutionModes[] = {"Native", "720p", "1080p", "1440p", "Custom"};
		ImGui::SetNextItemWidth(72.0f);
		if (ImGui::Combo("##res", &resolutionMode, resolutionModes, static_cast<int>(std::size(resolutionModes))))
		{
			viewportSettings.resolutionMode = static_cast<SceneViewportResolutionMode>(resolutionMode);
			viewportSettingsChanged = true;
		}
		ImGui::SetItemTooltip("Render resolution");

		if (static_cast<SceneViewportResolutionMode>(resolutionMode) == SceneViewportResolutionMode::Custom)
		{
			ImGui::SameLine(0.0f, 2.0f);
			const std::string customLabel = std::format("{}x{}##csz", viewportSettings.customExtent.width, viewportSettings.customExtent.height);
			if (ImGui::Button(customLabel.c_str()))
			{
				ImGui::OpenPopup("##customres");
			}

			if (ImGui::BeginPopup("##customres"))
			{
				int customExtent[2] = {static_cast<int>(viewportSettings.customExtent.width), static_cast<int>(viewportSettings.customExtent.height)};
				ImGui::SetNextItemWidth(150.0f);
				if (ImGui::InputInt2("Size", customExtent))
				{
					viewportSettings.customExtent.width = static_cast<std::uint32_t>(std::clamp(customExtent[0], 64, 8192));
					viewportSettings.customExtent.height = static_cast<std::uint32_t>(std::clamp(customExtent[1], 64, 8192));
					viewportSettingsChanged = true;
				}
				ImGui::EndPopup();
			}
		}

		if (viewportSettingsChanged)
		{
			ReleaseSceneViewportTexture(context);
			rendering.SetSceneViewportSettings(context.services, viewportSettings);
		}

		const char* displayModes[] = {"Fit", "Fill", "Actual", "Integer"};
		ImGui::SameLine();
		ImGui::SetNextItemWidth(72.0f);
		ImGui::Combo("##display", &m_viewportDisplayMode, displayModes, static_cast<int>(std::size(displayModes)));
		ImGui::SetItemTooltip("Display mode");

		const char* aspectModes[] = {"Render", "Free", "16:9", "16:10", "4:3", "1:1"};
		ImGui::SameLine();
		ImGui::SetNextItemWidth(66.0f);
		ImGui::Combo("##aspect", &m_viewportAspectMode, aspectModes, static_cast<int>(std::size(aspectModes)));
		ImGui::SetItemTooltip("Aspect ratio");

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
		ImGui::Checkbox("Stats", &m_viewportShowStats);
		ImGui::SameLine();
		ImGui::Checkbox("Mouse", &m_viewportShowMouse);

		ImGui::End();
	}
} // namespace aether::app
