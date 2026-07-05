#include "ViewportPanel.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>

#include "AetherCore.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "camera/CameraManager.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/DebugPanel.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/ScenePicker.hpp"
#include "material/EffectParamBuffer.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/SceneSerializer.hpp"
#include "scripting/SceneContext.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsSystem.hpp"
#include "platform/Input.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/Ray.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	namespace
	{
		scene::ApplySceneDeps MakeSceneDeps(LayerContext& context)
		{
			auto* sceneCtx = context.TryGet<scripting::SceneContext>();
			auto* assets = context.TryGet<AssetManager>();
			scene::ApplySceneDeps deps{};
			deps.assets = assets;
			deps.primitives = context.TryGet<PrimitiveMeshes>();
			deps.effectManager = sceneCtx ? sceneCtx->effects : nullptr;
			deps.effectParams = context.TryGet<EffectParamBuffer>();
			deps.pipelines = assets ? &assets->GetPipelineCache() : nullptr;
			deps.sceneContext = sceneCtx;
			deps.physics = context.TryGet<PhysicsSystem>();
			deps.renderer = context.TryGet<Renderer>();
			return deps;
		}
	} // namespace

	void ViewportPanel::OnUpdate(LayerContext& context)
	{
		// Editor camera policy: while Editing the viewport is driven by a
		// free-fly editor camera (RMB-fly chord); the game's camera - whatever
		// the script made main - is remembered and restored on Play.
		auto* playState = context.TryGet<PlayState>();
		auto* cameras = context.TryGet<CameraManager>();
		if (playState == nullptr || cameras == nullptr)
		{
			return;
		}

		const bool editing = !playState->IsPlaying();
		const CameraHandle main = cameras->GetMainCamera();

		// F5 script reload re-runs set_main_camera while Editing: the game took
		// the view back. Drop our claim so the block below re-seeds and reswaps.
		if (editing && m_editorCamActive && main.IsValid() && main.id != m_editorCamId)
		{
			m_editorCamActive = false;
		}

		if (editing && !m_editorCamActive)
		{
			if (main.IsValid() && main.id != m_editorCamId)
			{
				m_gameCamId = main.id;
			}
			if (m_editorCamId == 0)
			{
				CameraDesc desc;
				desc.mode = CameraMode::Free;
				desc.moveSpeed = 15.0f; // the sandbox plaza is ~100 units across
				m_editorCamId = cameras->Create(desc).id;
			}
			if (Camera* editorCam = cameras->TryGet(CameraHandle{m_editorCamId}))
			{
				// Seed the editor pose from whatever the player was looking at so
				// the swap is seamless.
				if (const Camera* from = cameras->TryGet(CameraHandle{m_gameCamId}); from != nullptr && main.IsValid() && main.id != m_editorCamId)
				{
					const glm::mat4 inv = glm::inverse(from->GetViewMatrix());
					const glm::vec3 eye = glm::vec3(inv[3]);
					const glm::vec3 fwd = glm::normalize(-glm::vec3(inv[2]));
					const float pitch = glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f)));
					const float yaw = glm::degrees(std::atan2(-fwd.x, -fwd.z));
					editorCam->SetPosition(eye);
					editorCam->SetYawPitch(yaw, pitch);
				}
				cameras->SetMainCamera(CameraHandle{m_editorCamId});
				m_editorCamActive = true;
			}
		}
		else if (!editing && m_editorCamActive)
		{
			if (m_gameCamId != 0 && cameras->TryGet(CameraHandle{m_gameCamId}) != nullptr)
			{
				cameras->SetMainCamera(CameraHandle{m_gameCamId});
			}
			m_editorCamActive = false;
		}
	}

	void ViewportPanel::DrawPlayControls(LayerContext& context)
	{
		auto* playState = context.TryGet<PlayState>();
		if (playState == nullptr)
		{
			return;
		}
		auto* assets = context.TryGet<AssetManager>();
		World& world = context.Get<World>();

		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();

		if (playState->IsPlaying())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.38f, 1.0f));
			const bool stop = ImGui::SmallButton(ICON_FA_STOP " Stop");
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("Stop and restore the scene captured at Play");
			if (stop)
			{
				playState->SetMode(PlayState::Mode::Editing);
				if (playState->stopSnapshot)
				{
					scene::ReplaceScene(*playState->stopSnapshot, world, MakeSceneDeps(context));
					playState->stopSnapshot.reset();
					context.Get<SceneSelection>().Clear();
				}
			}
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.45f, 1.0f));
			const bool play = ImGui::SmallButton(ICON_FA_PLAY " Play");
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("Snapshot the scene and simulate");
			if (play && assets != nullptr)
			{
				playState->stopSnapshot = scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
				playState->SetMode(PlayState::Mode::Playing);
			}
		}
	}

	bool ViewportPanel::DrawTransformGizmo(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		// W/E/R switch ops while the mouse is over the viewport - but not while
		// the right button is down, which is the free-camera's WASD-fly chord.
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			if (ImGui::IsKeyPressed(ImGuiKey_W))
			{
				m_gizmoOp = 0;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_E))
			{
				m_gizmoOp = 1;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_R))
			{
				m_gizmoOp = 2;
			}
		}

		auto& selection = context.Get<SceneSelection>();
		World& world = context.Get<World>();
		const Entity primary = selection.Primary();
		if (!primary.IsValid() || !world.GetRegistry().valid(World::ToEntt(primary)))
		{
			return false;
		}
		auto* tc = world.TryGet<TransformComponent>(primary);
		if (tc == nullptr)
		{
			return false;
		}
		const Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
		if (camera == nullptr)
		{
			return false;
		}

		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
		ImGuizmo::SetRect(imageMin.x, imageMin.y, imageSize.x, imageSize.y);

		const glm::mat4 view = camera->GetViewMatrix();
		glm::mat4 proj = camera->GetProjectionMatrix(renderAspect);
		proj[1][1] *= -1.0f; // undo the Vulkan Y-flip: ImGuizmo assumes GL clip conventions

		const ImGuizmo::OPERATION op = m_gizmoOp == 0 ? ImGuizmo::TRANSLATE : m_gizmoOp == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
		// ImGuizmo scales in local space only; WORLD+SCALE misbehaves.
		const ImGuizmo::MODE mode = (op == ImGuizmo::SCALE || m_gizmoLocal) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

		// Ctrl-hold snapping: half-metre steps, 15 degrees, 0.1 scale.
		float snapValues[3] = {0.5f, 0.5f, 0.5f};
		if (op == ImGuizmo::ROTATE)
		{
			snapValues[0] = 15.0f;
		}
		else if (op == ImGuizmo::SCALE)
		{
			snapValues[0] = snapValues[1] = snapValues[2] = 0.1f;
		}
		const float* snap = ImGui::GetIO().KeyCtrl ? snapValues : nullptr;

		glm::mat4 model = tc->localToWorld;
		if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op, mode, &model[0][0], nullptr, snap))
		{
			ApplyWorldTransform(context, world, primary, model);
		}
		return true;
	}

	void ViewportPanel::HandleViewportPicking(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		// The gizmo owns the mouse while hovered or dragging.
		if (ImGuizmo::IsOver() || ImGuizmo::IsUsingAny())
		{
			return;
		}
		// A click is a release that never dragged: camera orbits/looks move past
		// the threshold and never select.
		if (!ImGui::IsItemHovered() || !ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 4.0f))
		{
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		const glm::vec2 uv{(io.MousePos.x - imageMin.x) / imageSize.x, (io.MousePos.y - imageMin.y) / imageSize.y};
		if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
		{
			return;
		}

		const Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
		if (camera == nullptr)
		{
			return;
		}

		// Identical matrices to the renderer (projection at render-target
		// aspect), so letterboxed/stretched display modes pick what is shown.
		const glm::mat4 viewProj = camera->GetProjectionMatrix(renderAspect) * camera->GetViewMatrix();
		const Ray ray = BuildCameraRay(glm::inverse(viewProj), uv, camera->GetPosition());

		auto& selection = context.Get<SceneSelection>();
		const PickHit hit = PickEntity(context.Get<World>(), context.TryGet<PhysicsSystem>(), ray, camera->GetFarPlane());

		if (io.KeyCtrl)
		{
			if (hit.IsValid())
			{
				selection.ToggleSelection(hit.entity);
			}
		}
		else if (hit.IsValid())
		{
			selection.Select(hit.entity);
		}
		else
		{
			selection.Clear();
		}
	}

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

		// Image + gizmo submit before any input item so Manipulate sees this
		// frame's mouse first.
		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		const ImVec2 imageMax = ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y);
		ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(m_sceneViewportTextureId)), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
		const bool gizmoDrawn = DrawTransformGizmo(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);

		// ImGuizmo's CanActivate() consults ImGui::IsAnyItemHovered(), which
		// includes the PREVIOUS frame's hovered item - so merely submitting the
		// gizmo before the viewport's InvisibleButton is not enough: the button
		// hovered last frame still vetoes this frame's grab, forever (handles
		// highlight, drags never start). While a handle is hot the button is
		// therefore NOT SUBMITTED AT ALL; the hovered-id drains for a frame and
		// the grab activates. RMB is carved out (the gizmo only activates from
		// an LMB press) so flying across a handle never stalls the camera.
		// IsOver/IsUsing are only trusted on frames the gizmo actually drew -
		// they go stale when the selection clears.
		// Reporting the viewport as input-inactive while hot also flips the
		// engine's per-frame mouse capture on (Tick recomputes captured =
		// WantsInputCapture && !viewportActive BEFORE cameras update), so no
		// camera mode fights the drag.
		const bool gizmoHot = gizmoDrawn && (ImGuizmo::IsUsingAny() || (ImGuizmo::IsOver() && !ImGui::IsMouseDown(ImGuiMouseButton_Right)));
		context.Get<Input>().SetMouseViewportTransform(glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, glm::vec2{static_cast<float>(extent.width), static_cast<float>(extent.height)});
		if (gizmoHot)
		{
			context.Get<Input>().SetMouseViewportInputActive(false);
		}
		else
		{
			ImGui::InvisibleButton("SceneViewportInput", imageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			context.Get<Input>().SetMouseViewportInputActive(ImGui::IsItemHovered() || ImGui::IsItemActive());
			HandleViewportPicking(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
		}
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

		// Gizmo controls
		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
		const auto opButton = [&](const char* icon, int op, const char* tooltip)
		{
			const bool active = m_gizmoOp == op;
			if (active)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.2f, 1.0f));
			}
			if (ImGui::SmallButton(icon))
			{
				m_gizmoOp = op;
			}
			if (active)
			{
				ImGui::PopStyleColor();
			}
			ImGui::SetItemTooltip("%s", tooltip);
			ImGui::SameLine();
		};
		opButton(ICON_FA_UP_DOWN_LEFT_RIGHT, 0, "Translate (W)");
		opButton(ICON_FA_ROTATE, 1, "Rotate (E)");
		opButton(ICON_FA_EXPAND, 2, "Scale (R)");
		if (ImGui::SmallButton(m_gizmoLocal ? "Local" : "World"))
		{
			m_gizmoLocal = !m_gizmoLocal;
		}
		ImGui::SetItemTooltip("Gizmo orientation (Ctrl-drag snaps)");

		DrawPlayControls(context);

		ImGui::End();
	}
} // namespace aether::app
