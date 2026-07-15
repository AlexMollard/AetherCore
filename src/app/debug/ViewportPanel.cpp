#include "ViewportPanel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>

#include "AetherCore.hpp"
#include "PlaySession.hpp"
#include "PlayState.hpp"
#include "camera/CameraManager.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/DebugPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/ScenePicker.hpp"
#include "debug/UndoStack.hpp"
#include "assets/AssetDatabase.hpp"
#include "material/EffectParamBuffer.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scripting/SceneContext.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsSystem.hpp"
#include "platform/Input.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/CameraSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/Ray.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::editor
{
	void ViewportPanel::OnUpdate(app::LayerContext& context)
	{
		auto* playState = context.TryGet<app::PlayState>();
		auto* cameras = context.TryGet<CameraManager>();
		if (playState == nullptr || cameras == nullptr)
		{
			return;
		}

		const bool editing = !playState->IsPlaying();
		const CameraHandle main = cameras->GetMainCamera();
		const bool scene2D = context.Get<World>().GetSceneKind() == SceneKind::Scene2D;

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
			const bool justCreated = (m_editorCamId == 0);
			if (justCreated)
			{
				CameraDesc desc;
				desc.mode = scene2D ? CameraMode::Manual : CameraMode::Free;
				desc.projection = scene2D ? CameraProjection::Orthographic : CameraProjection::Perspective;
				desc.position = scene2D ? glm::vec3{0.0f, 0.0f, 10.0f} : desc.position;
				desc.moveSpeed = 15.0f;
				m_editorCamId = cameras->Create(desc).id;
			}
			if (Camera* editorCam = cameras->TryGet(CameraHandle{m_editorCamId}))
			{
				if (scene2D)
				{
					editorCam->SetMode(CameraMode::Manual);
					editorCam->SetYawPitch(0.0f, 0.0f);
					editorCam->SetOrthographic(10.0f, 0.1f, 1000.0f);
				}
				else
				{
					editorCam->SetMode(CameraMode::Free);
					editorCam->SetPerspective(60.0f, 0.1f, 1000.0f);
				}
				if (justCreated)
				{
					CameraHandle seedFrom{};
					if (auto* cameraSystem = context.TryGet<CameraSystem>())
					{
						seedFrom = cameraSystem->GetMainCameraBacking();
					}
					if (!seedFrom.IsValid())
					{
						seedFrom = CameraHandle{m_gameCamId};
					}
					if (const Camera* from = cameras->TryGet(seedFrom); from != nullptr && seedFrom.id != m_editorCamId)
					{
						const glm::mat4 inv = glm::inverse(from->GetViewMatrix());
						const glm::vec3 eye = glm::vec3(inv[3]);
						if (scene2D)
						{
							// A 2D scene must always open square to the XY plane. Carry the
							// game camera's framing across, but never inherit a small 3D yaw,
							// pitch, or Z offset that makes sprites appear perspective-skewed.
							editorCam->SetPosition({eye.x, eye.y, 10.0f});
							editorCam->SetYawPitch(0.0f, 0.0f);
							if (from->GetProjection() == CameraProjection::Orthographic)
							{
								editorCam->SetOrthographic(from->GetOrthographicHeight(), 0.1f, 1000.0f);
							}
						}
						else
						{
							const glm::vec3 fwd = glm::normalize(-glm::vec3(inv[2]));
							const float pitch = glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f)));
							const float yaw = glm::degrees(std::atan2(-fwd.x, -fwd.z));
							editorCam->SetPosition(eye);
							editorCam->SetYawPitch(yaw, pitch);
						}
					}
				}
				cameras->SetMainCamera(CameraHandle{m_editorCamId});
				m_editorCamActive = true;
				m_editor2DMode = scene2D;
			}
		}
		else if (!editing && m_editorCamActive)
		{
			if (m_lookThroughEntityId != 0)
			{
				RestoreEditorCameraAfterLookThrough(context);
			}
			CameraHandle entityMain{};
			if (auto* cameraSystem = context.TryGet<CameraSystem>())
			{
				entityMain = cameraSystem->GetMainCameraBacking();
			}
			if (main.IsValid() && main.id != m_editorCamId)
			{
				m_gameCamId = main.id;
			}
			else if (entityMain.IsValid())
			{
				m_gameCamId = entityMain.id;
				cameras->SetMainCamera(entityMain);
			}
			else if (m_gameCamId != 0 && cameras->TryGet(CameraHandle{m_gameCamId}) != nullptr)
			{
				cameras->SetMainCamera(CameraHandle{m_gameCamId});
			}
			m_editorCamActive = false;
			m_lookThroughEntityId = 0;
		}

		// Live "look through" preview: while Editing, lock the editor camera to the
		if (editing && m_editorCamActive && m_editorCamId != 0)
		{
			Camera* editorCam = cameras->TryGet(CameraHandle{m_editorCamId});
			World& world = context.Get<World>();
			if (editorCam != nullptr && m_lookThroughEntityId == 0 && m_editor2DMode != scene2D)
			{
				m_editor2DMode = scene2D;
				if (scene2D)
				{
					const glm::vec3 position = editorCam->GetPosition();
					editorCam->SetMode(CameraMode::Manual);
					editorCam->SetPosition({position.x, position.y, 10.0f});
					editorCam->SetYawPitch(0.0f, 0.0f);
					editorCam->SetOrthographic(10.0f, 0.1f, 1000.0f);
				}
				else
				{
					editorCam->SetMode(CameraMode::Free);
					editorCam->SetPerspective(60.0f, 0.1f, 1000.0f);
				}
			}
			const Entity target{m_lookThroughEntityId};
			const auto* targetCam = (m_lookThroughEntityId != 0 && world.GetRegistry().valid(World::ToEntt(target))) ? world.TryGet<CameraComponent>(target) : nullptr;
			const auto* targetTc = targetCam != nullptr ? world.TryGet<TransformComponent>(target) : nullptr;
			if (editorCam != nullptr && targetCam != nullptr && targetTc != nullptr)
			{
				const glm::vec3 position = glm::vec3(targetTc->localToWorld[3]);
				glm::vec3 forward = -glm::vec3(targetTc->localToWorld[2]);
				const float len = glm::length(forward);
				forward = len > 1e-6f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f);
				const float pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
				const float yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
				editorCam->SetMode(CameraMode::Manual);
				editorCam->SetPosition(position);
				editorCam->SetYawPitch(yaw, pitch);
				if (targetCam->projection == CameraProjection::Orthographic)
				{
					editorCam->SetOrthographic(targetCam->orthographicHeight, targetCam->nearPlane, targetCam->farPlane);
				}
				else
				{
					editorCam->SetPerspective(targetCam->fovDegrees, targetCam->nearPlane, targetCam->farPlane);
				}
			}
			else
			{
				if (m_lookThroughEntityId != 0 && editorCam != nullptr)
				{
					editorCam->SetMode(scene2D ? CameraMode::Manual : CameraMode::Free);
					if (scene2D)
					{
						editorCam->SetYawPitch(0.0f, 0.0f);
						editorCam->SetOrthographic(10.0f, 0.1f, 1000.0f);
					}
					else
					{
						editorCam->SetPerspective(60.0f, 0.1f, 1000.0f);
					}
				}
				m_lookThroughEntityId = 0;
			}
		}
	}

	void ViewportPanel::RestoreEditorCameraAfterLookThrough(app::LayerContext& context)
	{
		auto* cameras = context.TryGet<CameraManager>();
		Camera* editorCam = cameras != nullptr ? cameras->TryGet(CameraHandle{m_editorCamId}) : nullptr;
		if (editorCam != nullptr)
		{
			const bool scene2D = context.Get<World>().GetSceneKind() == SceneKind::Scene2D;
			if (scene2D)
			{
				editorCam->SetMode(CameraMode::Manual);
				editorCam->SetPosition(m_hasSaved2DEditorCamera ? m_saved2DEditorPosition : glm::vec3{0.0f, 0.0f, 10.0f});
				editorCam->SetYawPitch(0.0f, 0.0f);
				editorCam->SetOrthographic(m_hasSaved2DEditorCamera ? m_saved2DEditorHeight : 10.0f, 0.1f, 1000.0f);
			}
			else
			{
				editorCam->SetMode(CameraMode::Free);
				editorCam->SetPerspective(60.0f, 0.1f, 1000.0f);
			}
		}
		m_hasSaved2DEditorCamera = false;
		m_lookThroughEntityId = 0;
	}

	void ViewportPanel::DrawPlayControls(app::LayerContext& context)
	{
		auto* playState = context.TryGet<app::PlayState>();
		if (playState == nullptr)
		{
			return;
		}

		const bool playing = playState->IsPlaying();
		const bool compiling = playState->IsCompiling();
		const ImVec2 size(0.0f, ImGui::GetFrameHeight());
		bool clicked = false;
		if (playing)
		{
			clicked = chrome::PrimaryButton(ICON_FA_STOP " Stop", size);
		}
		else if (compiling)
		{
			clicked = chrome::OutlineButton(ICON_FA_GEAR " Compiling", size);
		}
		else
		{
			clicked = chrome::GhostButton(ICON_FA_PLAY " Play", size, chrome::kAccentHi);
		}
		ImGui::SetItemTooltip("%s", playing ? "Stop and restore the Play snapshot in-place" : (compiling ? "Building C# scripts on a worker thread - click to cancel" : "Snapshot the scene and simulate"));
		if (!clicked)
		{
			return;
		}
		TogglePlaySession(context);
	}

	bool ViewportPanel::DrawTransformGizmo(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
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

		ImGuizmo::SetOrthographic(camera->GetProjection() == CameraProjection::Orthographic);
		ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
		ImGuizmo::SetRect(imageMin.x, imageMin.y, imageSize.x, imageSize.y);

		{
			auto& gizmoStyle = ImGuizmo::GetStyle();
			const auto axis = [](const glm::vec4& c, const float a)
			{
				return ImVec4(c.r, c.g, c.b, a);
			};
			gizmoStyle.Colors[ImGuizmo::DIRECTION_X] = axis(colors::AxisX, 1.0f);
			gizmoStyle.Colors[ImGuizmo::DIRECTION_Y] = axis(colors::AxisY, 1.0f);
			gizmoStyle.Colors[ImGuizmo::DIRECTION_Z] = axis(colors::AxisZ, 1.0f);
			gizmoStyle.Colors[ImGuizmo::PLANE_X] = axis(colors::AxisX, 0.42f);
			gizmoStyle.Colors[ImGuizmo::PLANE_Y] = axis(colors::AxisY, 0.42f);
			gizmoStyle.Colors[ImGuizmo::PLANE_Z] = axis(colors::AxisZ, 0.42f);
			gizmoStyle.Colors[ImGuizmo::SELECTION] = axis(colors::Primary, 0.9f);
		}

		const glm::mat4 view = camera->GetViewMatrix();
		glm::mat4 proj = camera->GetProjectionMatrix(renderAspect);
		proj[1][1] *= -1.0f;

		bool constrain2D = world.GetSceneKind() == SceneKind::Scene2D;
		for (const Entity entity: selection.All())
		{
			constrain2D |= world.Has<SpriteRendererComponent>(entity);
		}
		const ImGuizmo::OPERATION op = constrain2D
		        ? (m_gizmoOp == 0 ? static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y)
		                          : m_gizmoOp == 1 ? ImGuizmo::ROTATE_Z : static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y))
		        : (m_gizmoOp == 0 ? ImGuizmo::TRANSLATE : m_gizmoOp == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
		const ImGuizmo::MODE mode = (m_gizmoOp == 2 || m_gizmoLocal) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

		float snapValues[3] = {0.5f, 0.5f, 0.5f};
		if (m_gizmoOp == 1)
		{
			snapValues[0] = 15.0f;
		}
		else if (m_gizmoOp == 2)
		{
			snapValues[0] = snapValues[1] = snapValues[2] = 0.1f;
		}
		const float* snap = ImGui::GetIO().KeyCtrl ? snapValues : nullptr;

		glm::mat4 model = tc->localToWorld;
		if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op, mode, &model[0][0], nullptr, snap))
		{
			if (constrain2D)
			{
				glm::vec3 position{}, rotation{}, scale{};
				DecomposeTRS(model, position, rotation, scale);
				rotation.x = 0.0f;
				rotation.y = 0.0f;
				scale.z = 1.0f;
				model = ComposeTransform(position, rotation, scale);
			}
			const glm::mat4 worldDelta = model * glm::inverse(tc->localToWorld);
			ApplyWorldTransform(context, world, primary, model);
			if (selection.All().size() > 1)
			{
				for (const Entity other: selection.All())
				{
					if (other == primary || HasSelectedAncestor(world, other, selection))
					{
						continue;
					}
					if (const auto* otc = world.TryGet<TransformComponent>(other))
					{
						ApplyWorldTransform(context, world, other, worldDelta * otc->localToWorld);
					}
				}
			}
		}
		return true;
	}

	void ViewportPanel::Draw2DGrid(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		World& world = context.Get<World>();
		const Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
		if (!m_viewportShow2DGrid || world.GetSceneKind() != SceneKind::Scene2D || camera == nullptr || camera->GetProjection() != CameraProjection::Orthographic)
		{
			return;
		}

		const glm::vec3 position = camera->GetPosition();
		const float height = camera->GetOrthographicHeight();
		const float width = height * renderAspect;
		const float rawStep = height / 20.0f;
		const float magnitude = std::pow(10.0f, std::floor(std::log10(glm::max(rawStep, 0.0001f))));
		const float normalized = rawStep / magnitude;
		const float step = (normalized < 2.0f ? 1.0f : normalized < 5.0f ? 2.0f : 5.0f) * magnitude;
		const float minX = position.x - width * 0.5f;
		const float maxX = position.x + width * 0.5f;
		const float minY = position.y - height * 0.5f;
		const float maxY = position.y + height * 0.5f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const auto toScreen = [&](glm::vec2 point)
		{
			return ImVec2{
			        imageMin.x + (point.x - minX) / width * imageSize.x,
			        imageMin.y + (maxY - point.y) / height * imageSize.y,
			};
		};
		const ImU32 gridColor = chrome::U32(chrome::WithAlpha(chrome::kMuted, 0.16f));
		const ImU32 axisXColor = chrome::U32(chrome::WithAlpha(chrome::C(colors::AxisX), 0.60f));
		const ImU32 axisYColor = chrome::U32(chrome::WithAlpha(chrome::C(colors::AxisY), 0.60f));
		const int firstX = static_cast<int>(std::floor(minX / step));
		const int lastX = static_cast<int>(std::ceil(maxX / step));
		for (int i = firstX; i <= lastX; ++i)
		{
			const float x = static_cast<float>(i) * step;
			drawList->AddLine(toScreen({x, minY}), toScreen({x, maxY}), i == 0 ? axisYColor : gridColor, i == 0 ? 1.5f : 1.0f);
		}
		const int firstY = static_cast<int>(std::floor(minY / step));
		const int lastY = static_cast<int>(std::ceil(maxY / step));
		for (int i = firstY; i <= lastY; ++i)
		{
			const float y = static_cast<float>(i) * step;
			drawList->AddLine(toScreen({minX, y}), toScreen({maxX, y}), i == 0 ? axisXColor : gridColor, i == 0 ? 1.5f : 1.0f);
		}
	}

	void ViewportPanel::Handle2DNavigation(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		World& world = context.Get<World>();
		Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
		if (!m_editorCamActive || world.GetSceneKind() != SceneKind::Scene2D || camera == nullptr || camera->GetProjection() != CameraProjection::Orthographic || !ImGui::IsItemHovered())
		{
			return;
		}

		ImGuiIO& io = ImGui::GetIO();
		const float oldHeight = camera->GetOrthographicHeight();
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))
		{
			glm::vec3 position = camera->GetPosition();
			position.x -= io.MouseDelta.x / imageSize.x * oldHeight * renderAspect;
			position.y += io.MouseDelta.y / imageSize.y * oldHeight;
			camera->SetPosition(position);
		}

		if (io.MouseWheel != 0.0f)
		{
			const glm::vec2 uv{
			        glm::clamp((io.MousePos.x - imageMin.x) / imageSize.x, 0.0f, 1.0f),
			        glm::clamp((io.MousePos.y - imageMin.y) / imageSize.y, 0.0f, 1.0f),
			};
			glm::vec3 position = camera->GetPosition();
			const glm::vec2 before{position.x + (uv.x - 0.5f) * oldHeight * renderAspect, position.y + (0.5f - uv.y) * oldHeight};
			const float newHeight = glm::clamp(oldHeight * std::exp(-io.MouseWheel * 0.15f), 0.01f, 100000.0f);
			const glm::vec2 after{position.x + (uv.x - 0.5f) * newHeight * renderAspect, position.y + (0.5f - uv.y) * newHeight};
			position += glm::vec3{before - after, 0.0f};
			camera->SetPosition(position);
			camera->SetOrthographic(newHeight, camera->GetNearPlane(), camera->GetFarPlane());
		}
	}

	void ViewportPanel::HandleViewportPicking(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		if (ImGuizmo::IsOver() || ImGuizmo::IsUsingAny())
		{
			return;
		}
		// Alt+LMB is the orbit chord, never a selection click.
		if (ImGui::GetIO().KeyAlt)
		{
			return;
		}
		// A click is a release that never dragged: camera orbits/looks move past
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

		const glm::mat4 viewProj = camera->GetProjectionMatrix(renderAspect) * camera->GetViewMatrix();
		const Ray ray = BuildCameraRay(
		        glm::inverse(viewProj), uv, camera->GetPosition(), camera->GetProjection() == CameraProjection::Orthographic);

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

	void ViewportPanel::DrawCameraGizmos(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		if (auto* playState = context.TryGet<app::PlayState>(); playState != nullptr && playState->IsPlaying())
		{
			return;
		}
		const Camera* viewCam = context.Get<CameraManager>().TryGetMainCamera();
		if (viewCam == nullptr)
		{
			return;
		}
		World& world = context.Get<World>();
		const bool scene2D = world.GetSceneKind() == SceneKind::Scene2D;
		auto& reg = world.GetRegistry();
		if (reg.view<CameraComponent, TransformComponent>().size_hint() == 0)
		{
			return;
		}
		auto* selection = context.TryGet<SceneSelection>();

		const glm::mat4 view = viewCam->GetViewMatrix();
		glm::mat4 proj = viewCam->GetProjectionMatrix(renderAspect);
		proj[1][1] *= -1.0f;
		const glm::mat4 viewProj = proj * view;

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ImVec2(imageMin.x, imageMin.y), ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y), true);

		reg.view<CameraComponent, TransformComponent>().each(
		        [&](entt::entity handle, const CameraComponent& cam, const TransformComponent& tc)
		        {
			        const Entity e = World::FromEntt(handle);
			        const bool isMain = reg.all_of<MainCameraComponent>(handle);
			        const bool isSelected = selection != nullptr && selection->Contains(e);

			        const glm::vec3 pos = glm::vec3(tc.localToWorld[3]);
			        const glm::vec3 right = glm::normalize(glm::vec3(tc.localToWorld[0]));
			        const glm::vec3 up = glm::normalize(glm::vec3(tc.localToWorld[1]));
			        const glm::vec3 fwd = -glm::normalize(glm::vec3(tc.localToWorld[2]));

			        const float nearD = std::max(0.02f, cam.nearPlane);
			        const float farD = std::clamp(cam.farPlane, nearD + 0.5f, nearD + 9.0f);
			        const bool orthographic = cam.projection == CameraProjection::Orthographic;
			        const float tanHalf = std::tan(glm::radians(cam.fovDegrees) * 0.5f);

			        auto planeCorners = [&](float dist, glm::vec3 out[4])
			        {
				        const float h = orthographic ? std::max(0.001f, cam.orthographicHeight) * 0.5f : tanHalf * dist;
				        const float w = h * renderAspect;
				        const glm::vec3 c = pos + fwd * dist;
				        out[0] = c - right * w + up * h;
				        out[1] = c + right * w + up * h;
				        out[2] = c + right * w - up * h;
				        out[3] = c - right * w - up * h;
			        };
			        glm::vec3 nearC[4];
			        glm::vec3 farC[4];
			        planeCorners(nearD, nearC);
			        planeCorners(farD, farC);

			        auto project = [&](const glm::vec3& wp, ImVec2& out) -> bool
			        {
				        const glm::vec4 clip = viewProj * glm::vec4(wp, 1.0f);
				        if (clip.w <= 1e-4f)
				        {
					        return false;
				        }
				        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				        out = ImVec2(imageMin.x + (ndc.x * 0.5f + 0.5f) * imageSize.x, imageMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * imageSize.y);
				        return true;
			        };

			        const ImVec4 baseColor = isSelected ? chrome::kText : isMain ? chrome::kAccent : chrome::kMuted;
			        const ImU32 color = chrome::U32(chrome::WithAlpha(baseColor, isSelected ? 0.92f : isMain ? 0.86f : 0.74f));
			        const float thickness = (isSelected || isMain) ? 2.0f : 1.25f;

			        auto line = [&](const glm::vec3& a, const glm::vec3& b)
			        {
				        ImVec2 sa, sb;
				        if (project(a, sa) && project(b, sb))
				        {
					        drawList->AddLine(sa, sb, color, thickness);
				        }
			        };
			        if (scene2D && orthographic)
			        {
				        // A 2D camera is a framing rectangle, not a near/far volume.
				        // Draw it on the XY content plane so it cannot read as a cube.
				        const float h = std::max(0.001f, cam.orthographicHeight) * 0.5f;
				        const float w = h * renderAspect;
				        const glm::vec3 center{pos.x, pos.y, 0.0f};
				        const glm::vec3 frame[4]{
				                center + glm::vec3{-w, h, 0.0f},
				                center + glm::vec3{w, h, 0.0f},
				                center + glm::vec3{w, -h, 0.0f},
				                center + glm::vec3{-w, -h, 0.0f},
				        };
				        for (int i = 0; i < 4; ++i)
				        {
					        line(frame[i], frame[(i + 1) % 4]);
				        }
				        return;
			        }
			        for (int i = 0; i < 4; ++i)
			        {
				        const int j = (i + 1) % 4;
				        line(nearC[i], nearC[j]);
				        line(farC[i], farC[j]);
				        line(nearC[i], farC[i]);
			        }
			        if (!orthographic)
			        {
				        line(pos, nearC[0]);
				        line(pos, nearC[1]);
				        line(pos, nearC[2]);
				        line(pos, nearC[3]);
			        }
		        });

		drawList->PopClipRect();
	}

	void ViewportPanel::DrawSpriteOutlines(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect)
	{
		const auto* selection = context.TryGet<SceneSelection>();
		const Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
		if (selection == nullptr || camera == nullptr || selection->All().empty())
		{
			return;
		}
		glm::mat4 projection = camera->GetProjectionMatrix(renderAspect);
		projection[1][1] *= -1.0f;
		const glm::mat4 viewProjection = projection * camera->GetViewMatrix();
		World& world = context.Get<World>();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		for (const Entity entity: selection->All())
		{
			const auto* sprite = world.TryGet<SpriteRendererComponent>(entity);
			const auto* transform = world.TryGet<TransformComponent>(entity);
			if (sprite == nullptr || transform == nullptr)
			{
				continue;
			}
			const glm::vec2 size = sprite->pixelSize / std::max(sprite->pixelsPerUnit, 0.001f);
			const glm::vec2 local[4] = {
			        -sprite->pivot * size,
			        (glm::vec2(1.0f, 0.0f) - sprite->pivot) * size,
			        (glm::vec2(1.0f) - sprite->pivot) * size,
			        (glm::vec2(0.0f, 1.0f) - sprite->pivot) * size,
			};
			ImVec2 screen[4];
			bool valid = true;
			for (std::uint32_t i = 0; i < 4; ++i)
			{
				const glm::vec4 clip = viewProjection * transform->localToWorld * glm::vec4(local[i], 0.0f, 1.0f);
				if (clip.w <= 1e-5f)
				{
					valid = false;
					break;
				}
				const glm::vec2 ndc = glm::vec2(clip) / clip.w;
				screen[i] = ImVec2(imageMin.x + (ndc.x * 0.5f + 0.5f) * imageSize.x, imageMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * imageSize.y);
			}
			if (valid)
			{
				drawList->AddPolyline(screen, 4, chrome::U32(chrome::kAccentHi), ImDrawFlags_Closed, 2.0f);
			}
		}
	}

	void ViewportPanel::DrawCameraPreviewControls(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize)
	{
		auto* playState = context.TryGet<app::PlayState>();
		const bool editing = playState == nullptr || !playState->IsPlaying();
		auto& selection = context.Get<SceneSelection>();
		World& world = context.Get<World>();

		auto& rendering = context.Get<aether::RenderingSubsystem>();
		CameraPreviewService& preview = rendering.GetCameraPreview();

		const Entity primary = selection.Primary();
		const bool selectedIsCamera = editing && primary.IsValid() && world.GetRegistry().valid(World::ToEntt(primary)) && world.Has<CameraComponent>(primary);
		const bool previewing = m_lookThroughEntityId != 0;

		bool enabled = false;
		if (selectedIsCamera)
		{
			if (const auto* cc = world.TryGet<CameraComponent>(primary))
			{
				if (const Camera* backing = context.Get<CameraManager>().TryGet(CameraHandle{cc->backingCamera}))
				{
					constexpr float aspect = static_cast<float>(CameraPreviewService::kWidth) / static_cast<float>(CameraPreviewService::kHeight);
					preview.SetRequest(true,
					        backing->GetViewMatrix(),
					        backing->GetProjectionMatrix(aspect),
					        backing->GetPosition(),
					        backing->GetNearPlane(),
					        world.GetSceneKind() != SceneKind::Scene2D);
					enabled = true;
				}
			}
		}
		if (!enabled)
		{
			preview.SetRequest(false, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f));
		}

		if (!selectedIsCamera && !previewing)
		{
			return;
		}

		const float thumbW = 240.0f;
		const float thumbH = thumbW * static_cast<float>(CameraPreviewService::kHeight) / static_cast<float>(CameraPreviewService::kWidth);
		const ImVec2 pillPad(8.0f, 8.0f);
		const ImVec2 pillSpacing(6.0f, 6.0f);
		const float pillW = thumbW + pillPad.x * 2.0f;
		const float pillH = pillPad.y * 2.0f + 15.0f + pillSpacing.y + thumbH + pillSpacing.y + ImGui::GetFrameHeight();
		ImGui::SetCursorScreenPos(ImVec2(imageMin.x + imageSize.x - pillW - 12.0f, imageMin.y + imageSize.y - pillH - 12.0f));

		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::WithAlpha(chrome::kPanel, 0.95f));
		ImGui::PushStyleColor(ImGuiCol_Border, chrome::kStroke);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pillPad);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, pillSpacing);
		ImGui::BeginChild("##vpCameraPreview", ImVec2(pillW, pillH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		chrome::SectionTag(previewing ? "CAMERA VIEW · LIVE" : "CAMERA VIEW");

		if (enabled)
		{
			const gpu::ImageView pv = preview.GetColorView();
			if (pv != m_cameraPreviewImageView)
			{
				if (auto* imgui = context.TryGet<aether::ImguiSubsystem>())
				{
					if (m_cameraPreviewTextureId != 0)
					{
						imgui->UnregisterTexture(static_cast<ImTextureID>(m_cameraPreviewTextureId));
					}
					const ImTextureID id = imgui->RegisterTexture(pv, gpu::ImageLayout::ShaderReadOnly);
					m_cameraPreviewTextureId = (id != ImTextureID_Invalid) ? static_cast<std::uint64_t>(id) : 0;
					m_cameraPreviewImageView = pv;
				}
			}
		}
		const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
		if (enabled && m_cameraPreviewTextureId != 0)
		{
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_cameraPreviewTextureId)), ImVec2(thumbW, thumbH));
		}
		else
		{
			ImGui::Dummy(ImVec2(thumbW, thumbH));
		}
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 thumbMax(thumbMin.x + thumbW, thumbMin.y + thumbH);
			drawList->AddRect(thumbMin, thumbMax, chrome::U32(chrome::kStroke), 0.0f, 0, 1.0f);
			chrome::CornerBrackets(drawList, ImVec2(thumbMin.x + 5.0f, thumbMin.y + 5.0f), ImVec2(thumbMax.x - 5.0f, thumbMax.y - 5.0f), 14.0f, 2.0f, previewing ? chrome::kAccentHi : chrome::WithAlpha(chrome::kAccent, 0.7f));
		}

		const char* label = previewing ? ICON_FA_VIDEO " Exit camera view" : ICON_FA_VIDEO " Look through";
		const bool toggle = previewing ? chrome::PrimaryButton(label, ImVec2(thumbW, 0.0f)) : chrome::OutlineButton(label, ImVec2(thumbW, 0.0f));
		if (toggle)
		{
			if (previewing)
			{
				RestoreEditorCameraAfterLookThrough(context);
			}
			else
			{
				if (world.GetSceneKind() == SceneKind::Scene2D)
				{
					if (auto* cameras = context.TryGet<CameraManager>())
					{
						if (const Camera* editorCam = cameras->TryGet(CameraHandle{m_editorCamId}))
						{
							m_saved2DEditorPosition = editorCam->GetPosition();
							m_saved2DEditorHeight = editorCam->GetOrthographicHeight();
							m_hasSaved2DEditorCamera = true;
						}
					}
				}
				m_lookThroughEntityId = primary.id;
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
	}

	void ViewportPanel::OnAttach(app::LayerContext& context)
	{
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, true);
	}

	void ViewportPanel::OnDetach(app::LayerContext& context)
	{
		ReleaseSceneViewportTexture(context);
		context.Get<Input>().ClearMouseViewportTransform();
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, false);
	}

	void ViewportPanel::OnRenderTargetsInvalidated(app::LayerContext& context)
	{
		ReleaseSceneViewportTexture(context);
	}

	void ViewportPanel::ReleaseSceneViewportTexture(app::LayerContext& context)
	{
		if (m_sceneViewportTextureId != 0)
		{
			if (auto* imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_sceneViewportTextureId));
			}
		}

		m_sceneViewportTextureId = 0;
		m_sceneViewportImageView = nullptr;
	}

	void ViewportPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin(GetName().data(), VisiblePtr(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		auto& rendering = context.Get<aether::RenderingSubsystem>();
		auto& post = rendering.GetPostProcessStack();
		const gpu::ImageView imageView = post.GetFinalColorImageView();

		if (imageView != m_sceneViewportImageView)
		{
			ReleaseSceneViewportTexture(context);
			if (auto* imgui = context.TryGet<aether::ImguiSubsystem>())
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

		const ImVec2 contentAvailable(std::max(1.0f, ImGui::GetContentRegionAvail().x), std::max(1.0f, ImGui::GetContentRegionAvail().y));
		const ImVec2 toolbarMin = ImGui::GetCursorScreenPos();
		const float btnH = ImGui::GetFrameHeight();
		const ImVec2 pillPad(4.0f, 4.0f);
		const float pillH = btnH + pillPad.y * 2.0f;
		const float toolbarH = pillH + 8.0f;
		const ImVec2 available(contentAvailable.x, std::max(1.0f, contentAvailable.y - toolbarH));
		const ImVec2 imageAreaMin(toolbarMin.x, toolbarMin.y + toolbarH);
		const ImVec2 imageAreaMax(imageAreaMin.x + available.x, imageAreaMin.y + available.y);

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
			case 1:
				if (imageSize.x < imageSize.y * targetAspect)
				{
					imageSize.x = imageSize.y * targetAspect;
				}
				else
				{
					imageSize.y = imageSize.x / targetAspect;
				}
				break;
			case 2:
				imageSize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
				break;
			case 3:
			{
				const float sx = extent.width > 0 ? std::floor(available.x / static_cast<float>(extent.width)) : 1.0f;
				const float sy = extent.height > 0 ? std::floor(available.y / static_cast<float>(extent.height)) : 1.0f;
				const float scale = std::max(1.0f, std::min(sx, sy));
				imageSize = ImVec2(static_cast<float>(extent.width) * scale, static_cast<float>(extent.height) * scale);
				break;
			}
			default:
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
		ImGui::SetCursorPos(ImVec2(imageCursorStart.x + (available.x - imageSize.x) * 0.5f, imageCursorStart.y + toolbarH + (available.y - imageSize.y) * 0.5f));

		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		const ImVec2 imageMax = ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y);
		const auto* viewportPlayState = context.TryGet<app::PlayState>();
		const bool viewportEditing = viewportPlayState == nullptr || !viewportPlayState->IsPlaying();
		const bool editorViewportInteractive = viewportEditing && m_lookThroughEntityId == 0;
		ImGui::PushClipRect(imageAreaMin, imageAreaMax, true);
		ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(m_sceneViewportTextureId)), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
		bool gizmoDrawn = false;
		if (editorViewportInteractive)
		{
			Draw2DGrid(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
			DrawSpriteOutlines(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
			gizmoDrawn = DrawTransformGizmo(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
			DrawCameraGizmos(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
		}

		// Do not cover ImGuizmo with the input hitbox: it steals handle hover and prevents drags.
		const ImVec2 gizmoMouse = ImGui::GetIO().MousePos;
		const bool mouseOverImage =
		        gizmoMouse.x >= std::max(imageMin.x, imageAreaMin.x) && gizmoMouse.x < std::min(imageMax.x, imageAreaMax.x) && gizmoMouse.y >= std::max(imageMin.y, imageAreaMin.y) && gizmoMouse.y < std::min(imageMax.y, imageAreaMax.y);
		const bool gizmoActive = gizmoDrawn && mouseOverImage && ImGuizmo::IsUsingAny() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
		const bool gizmoHovered = gizmoDrawn && mouseOverImage && ImGuizmo::IsOver() && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
		const ImVec2 inputViewportOrigin = ImGui::GetWindowViewport() != nullptr ? ImGui::GetWindowViewport()->Pos : ImGui::GetMainViewport()->Pos;
		const glm::vec2 inputImageMin{imageMin.x - inputViewportOrigin.x, imageMin.y - inputViewportOrigin.y};
		context.Get<Input>().SetMouseViewportTransform(inputImageMin, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, glm::vec2{static_cast<float>(extent.width), static_cast<float>(extent.height)});
		if (editorViewportInteractive && !gizmoActive && !gizmoHovered)
		{
			ImGui::InvisibleButton("SceneViewportInput", imageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload); payload != nullptr && payload->DataSize == sizeof(dragdrop::FilePayload))
				{
					const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
					Camera* camera = context.Get<CameraManager>().TryGetMainCamera();
					World& world = context.Get<World>();
					if (file->kind == dragdrop::FileKind::Texture && camera != nullptr && camera->GetProjection() == CameraProjection::Orthographic)
					{
						if (auto* undo = context.TryGet<UndoStack>())
						{
							undo->Push(world, context.services);
						}
						const glm::vec2 mouse{ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y};
						const glm::vec2 uv = (mouse - glm::vec2{imageMin.x, imageMin.y}) / glm::vec2{imageSize.x, imageSize.y};
						const glm::vec3 cameraPosition = camera->GetPosition();
						const float height = camera->GetOrthographicHeight();
						const glm::vec3 position{cameraPosition.x + (uv.x - 0.5f) * height * renderAspect, cameraPosition.y + (0.5f - uv.y) * height, 0.0f};
						const Entity entity = world.Create();
						world.Emplace<NameComponent>(entity, NameComponent{.name = file->displayName[0] != '\0' ? file->displayName : "Sprite"});
						TransformComponent transform{};
						transform.localToWorld[3] = glm::vec4(position, 1.0f);
						world.Emplace<TransformComponent>(entity, transform);
						world.Emplace<SpriteRendererComponent>(entity, SpriteRendererComponent{.texturePath = file->path});
						if (auto* database = context.TryGet<AssetDatabase>())
						{
							database->Register(MakeTextureSource(file->path));
						}
						context.Get<SceneSelection>().Select(entity);
					}
				}
				ImGui::EndDragDropTarget();
			}
			Handle2DNavigation(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
			HandleViewportPicking(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, renderAspect);
		}

		if (editorViewportInteractive && m_editorCamActive && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			auto& selection = context.Get<SceneSelection>();
			World& world = context.Get<World>();
			const Entity primary = selection.Primary();
			if (primary.IsValid() && world.GetRegistry().valid(World::ToEntt(primary)))
			{
				if (const auto* tc = world.TryGet<TransformComponent>(primary))
				{
					if (Camera* cam = context.Get<CameraManager>().TryGet(CameraHandle{m_editorCamId}))
					{
						const glm::vec3 target = glm::vec3(tc->localToWorld[3]);
						const float sx = glm::length(glm::vec3(tc->localToWorld[0]));
						const float sy = glm::length(glm::vec3(tc->localToWorld[1]));
						const float sz = glm::length(glm::vec3(tc->localToWorld[2]));
						if (world.GetSceneKind() == SceneKind::Scene2D)
						{
							const glm::vec3 current = cam->GetPosition();
							cam->SetPosition({target.x, target.y, current.z});
							cam->SetOrthographic(glm::max(2.0f, 2.5f * glm::max(sx, sy)), cam->GetNearPlane(), cam->GetFarPlane());
						}
						else
						{
							const float dist = glm::max(4.0f, 2.5f * glm::max(sx, glm::max(sy, sz)));
							cam->FocusOn(target, dist);
						}
					}
				}
			}
		}
		if (editorViewportInteractive && (m_viewportShowStats || m_viewportShowMouse))
		{
			struct StatRow
			{
				const char* label;
				std::string value;
			};

			std::array<StatRow, 3> rows;
			std::size_t rowCount = 0;
			if (m_viewportShowStats)
			{
				rows[rowCount++] = {"RENDER", std::format("{} x {}", extent.width, extent.height)};
				rows[rowCount++] = {"VIEW", std::format("{:.0f} x {:.0f}", imageSize.x, imageSize.y)};
			}
			if (m_viewportShowMouse)
			{
				const glm::vec2 mouse = context.Get<Input>().GetMousePos();
				if (mouse.x > -999999.0f)
				{
					rows[rowCount++] = {"MOUSE", std::format("{:.0f}, {:.0f}", mouse.x, mouse.y)};
				}
			}
			if (rowCount > 0)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				constexpr float kLabelSize = 12.0f;
				constexpr float kValueSize = 13.0f;
				constexpr float kRowH = 17.0f;
				const ImVec2 pad(10.0f, 8.0f);
				float labelW = 0.0f;
				float valueW = 0.0f;
				for (std::size_t i = 0; i < rowCount; ++i)
				{
					labelW = std::max(labelW, chrome::MeasureSized(kLabelSize, rows[i].label).x);
					valueW = std::max(valueW, chrome::MeasureSized(kValueSize, rows[i].value.c_str()).x);
				}
				const ImVec2 rectMin(imageMin.x + 10.0f, imageMin.y + 10.0f);
				const ImVec2 rectMax(rectMin.x + pad.x * 2.0f + labelW + 14.0f + valueW, rectMin.y + pad.y * 2.0f + static_cast<float>(rowCount) * kRowH - 3.0f);
				drawList->AddRectFilled(rectMin, rectMax, chrome::U32(chrome::WithAlpha(chrome::kPanel, 0.88f)), 4.0f);
				drawList->AddRect(rectMin, rectMax, chrome::U32(chrome::kStroke), 4.0f, 0, 1.0f);
				// Amber tick on the leading edge - the section-label motif.
				drawList->AddRectFilled(ImVec2(rectMin.x, rectMin.y + 6.0f), ImVec2(rectMin.x + 3.0f, rectMax.y - 6.0f), chrome::U32(chrome::WithAlpha(chrome::kAccent, 0.9f)));
				for (std::size_t i = 0; i < rowCount; ++i)
				{
					const float y = rectMin.y + pad.y + static_cast<float>(i) * kRowH;
					chrome::TextSized(drawList, kLabelSize, ImVec2(rectMin.x + pad.x, y + 1.0f), chrome::kFaint, rows[i].label);
					chrome::TextSized(drawList, kValueSize, ImVec2(rectMin.x + pad.x + labelW + 14.0f, y), chrome::kText, rows[i].value.c_str());
				}
			}
		}
		ImGui::PopClipRect();

		// ── Match Panel render resolution (functional; recomputed every frame so the
		// scene target tracks the panel size + DPI) ────────────────────────────────
		SceneViewportSettings viewportSettings = rendering.GetSceneViewportSettings();
		bool viewportSettingsChanged = false;
		if (viewportSettings.resolutionMode == SceneViewportResolutionMode::MatchPanel)
		{
			const float dpi = ImGui::GetWindowDpiScale();
			const auto physW = static_cast<std::uint32_t>(std::clamp(available.x * dpi, 64.0f, 8192.0f));
			const auto physH = static_cast<std::uint32_t>(std::clamp(available.y * dpi, 64.0f, 8192.0f));
			if (physW != viewportSettings.customExtent.width || physH != viewportSettings.customExtent.height)
			{
				viewportSettings.customExtent.width = physW;
				viewportSettings.customExtent.height = physH;
				viewportSettingsChanged = true;
			}
		}

		// ── Viewport toolbar: tools (left) · Play (center) · settings (right) ──────
		// The toolbar owns a dedicated row above the scene image so editor controls
		// never cover game UI. Each group remains an isolated child for clean hover.
		auto* toolbarPlayState = context.TryGet<app::PlayState>();
		const bool toolbarPlaying = toolbarPlayState != nullptr && toolbarPlayState->IsPlaying();
		const bool toolbarCompiling = toolbarPlayState != nullptr && toolbarPlayState->IsCompiling();
		const float pillTop = toolbarMin.y + 4.0f;
		bool toolbarControlActive = false;

		// Live-session hairline across the top edge of the scene image: solid amber
		// while playing, dimmed while the async script build is still running.
		if (toolbarPlaying || toolbarCompiling)
		{
			chrome::AccentHairline(ImGui::GetWindowDrawList(), imageMin, imageSize.x, toolbarPlaying ? 0.85f : 0.35f);
		}

		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::WithAlpha(chrome::kPanel, 0.92f));
		ImGui::PushStyleColor(ImGuiCol_Border, chrome::kStroke);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pillPad);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

		// Left pill: gizmo tools + orientation. The active op is a filled amber
		// segment; idle ops are ghosts.
		ImGui::SetCursorScreenPos(ImVec2(toolbarMin.x, pillTop));
		ImGui::BeginChild(
		        "##vpTools", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			const auto tool = [&](const char* icon, int op, const char* tooltip)
			{
				// Wide enough for the FA advance so the glyph centers instead of
				// left-clamping (icon advance can exceed a btnH square's inner width).
				const float iconW = std::max(btnH, ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f);
				const bool active = m_gizmoOp == op;
				const bool pressed = active ? chrome::ActiveToolButton(icon, ImVec2(iconW, btnH)) : chrome::GhostButton(icon, ImVec2(iconW, btnH));
				if (pressed)
				{
					m_gizmoOp = op;
				}
				toolbarControlActive = toolbarControlActive || ImGui::IsItemActive();
				ImGui::SetItemTooltip("%s", tooltip);
			};
			tool(ICON_FA_UP_DOWN_LEFT_RIGHT, 0, "Translate (W)");
			ImGui::SameLine();
			tool(ICON_FA_ROTATE, 1, "Rotate (E)");
			ImGui::SameLine();
			tool(ICON_FA_EXPAND, 2, "Scale (R)");
			ImGui::SameLine(0.0f, 10.0f);
			// Space toggle: amber text signals the non-default Local space.
			if (chrome::GhostButton(m_gizmoLocal ? "Local" : "World", ImVec2(0.0f, btnH), m_gizmoLocal ? chrome::kAccentHi : chrome::kMuted))
			{
				m_gizmoLocal = !m_gizmoLocal;
			}
			toolbarControlActive = toolbarControlActive || ImGui::IsItemActive();
			ImGui::SetItemTooltip("Gizmo space (Ctrl-drag snaps)");
		}
		ImGui::EndChild();

		// Center pill: Play/Stop, horizontally centered in the toolbar row.
		const char* playSizeLabel = toolbarPlaying ? ICON_FA_STOP " Stop" : (toolbarCompiling ? ICON_FA_GEAR " Compiling" : ICON_FA_PLAY " Play");
		const float playBtnW = ImGui::CalcTextSize(playSizeLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float playPillW = playBtnW + pillPad.x * 2.0f;
		ImGui::SetCursorScreenPos(ImVec2(toolbarMin.x + (contentAvailable.x - playPillW) * 0.5f, pillTop));
		ImGui::BeginChild("##vpPlay", ImVec2(playPillW, pillH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			DrawPlayControls(context);
			toolbarControlActive = toolbarControlActive || ImGui::IsItemActive();
		}
		ImGui::EndChild();

		// Right pill: view settings gear, anchored to the right edge. Sized for the
		// FA advance so the glyph centers (see the tool buttons above).
		const float gearBtnW = std::max(btnH, ImGui::CalcTextSize(ICON_FA_GEAR).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		const float gearPillW = gearBtnW + pillPad.x * 2.0f;
		ImGui::SetCursorScreenPos(ImVec2(toolbarMin.x + contentAvailable.x - gearPillW, pillTop));
		ImGui::BeginChild("##vpGear", ImVec2(gearPillW, pillH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			if (chrome::GhostIconButton(ICON_FA_GEAR, "##viewportGear", ImVec2(gearBtnW, btnH)))
			{
				ImGui::OpenPopup("##vpSettings");
			}
			toolbarControlActive = toolbarControlActive || ImGui::IsItemActive();
			ImGui::SetItemTooltip("View settings");
			ImGui::PushStyleColor(ImGuiCol_PopupBg, chrome::WithAlpha(chrome::kPanelHi, 0.98f));
			ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
			if (ImGui::BeginPopup("##vpSettings"))
			{
				toolbarControlActive = true;
				chrome::SectionTag("VIEW SETTINGS");
				ImGui::Spacing();
				const char* const resolutionModes[] = {"Native", "720p", "1080p", "1440p", "Custom", "Match Panel"};
				int resolutionMode = static_cast<int>(viewportSettings.resolutionMode);
				ImGui::SetNextItemWidth(150.0f);
				if (ImGui::Combo("Resolution", &resolutionMode, resolutionModes, static_cast<int>(std::size(resolutionModes))))
				{
					viewportSettings.resolutionMode = static_cast<SceneViewportResolutionMode>(resolutionMode);
					viewportSettingsChanged = true;
				}
				if (viewportSettings.resolutionMode == SceneViewportResolutionMode::Custom)
				{
					int customExtent[2] = {static_cast<int>(viewportSettings.customExtent.width), static_cast<int>(viewportSettings.customExtent.height)};
					ImGui::SetNextItemWidth(150.0f);
					if (ImGui::InputInt2("Custom size", customExtent))
					{
						viewportSettings.customExtent.width = static_cast<std::uint32_t>(std::clamp(customExtent[0], 64, 8192));
						viewportSettings.customExtent.height = static_cast<std::uint32_t>(std::clamp(customExtent[1], 64, 8192));
						viewportSettingsChanged = true;
					}
				}
				const char* const displayModes[] = {"Fit", "Fill", "Actual", "Integer"};
				ImGui::SetNextItemWidth(150.0f);
				ImGui::Combo("Display", &m_viewportDisplayMode, displayModes, static_cast<int>(std::size(displayModes)));
				const char* const aspectModes[] = {"Render", "Free", "16:9", "16:10", "4:3", "1:1"};
				ImGui::SetNextItemWidth(150.0f);
				ImGui::Combo("Aspect", &m_viewportAspectMode, aspectModes, static_cast<int>(std::size(aspectModes)));
				ImGui::Spacing();
				chrome::SectionTag("OVERLAYS");
				ImGui::Spacing();
				if (context.Get<World>().GetSceneKind() == SceneKind::Scene2D)
				{
					ImGui::Checkbox("2D grid", &m_viewportShow2DGrid);
				}
				ImGui::Checkbox("Stats overlay", &m_viewportShowStats);
				ImGui::Checkbox("Mouse overlay", &m_viewportShowMouse);
				ImGui::EndPopup();
			}
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();

		ImGui::PopStyleVar(4);
		ImGui::PopStyleColor(2);

		// Look-through preview pill (bottom-center), shown for camera selections.
		if (viewportEditing)
		{
			DrawCameraPreviewControls(context, glm::vec2{imageMin.x, imageMin.y}, glm::vec2{imageSize.x, imageSize.y});
		}
		else
		{
			rendering.GetCameraPreview().SetRequest(false, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f));
		}
		toolbarControlActive = toolbarControlActive || ImGui::IsItemActive();

		// Tick() consumes this value on the next frame when translating ImGui capture
		// into camera capture. Toolbar hover is intentionally ignored: the camera
		// should only lose the mouse while a control/popup/gizmo is actually active.
		context.Get<Input>().SetMouseViewportInputActive(editorViewportInteractive && mouseOverImage && !gizmoActive && !toolbarControlActive);

		if (viewportSettingsChanged)
		{
			ReleaseSceneViewportTexture(context);
			rendering.SetSceneViewportSettings(context.services, viewportSettings);
		}

		ImGui::End();
	}
} // namespace aether::editor
