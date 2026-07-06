#include "LightingPanel.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		glm::vec4 LightDebugColor(glm::vec3 color, float intensity, bool castsShadow)
		{
			const float maxChannel = std::max({color.x, color.y, color.z, 0.001f});
			color /= maxChannel;
			const float gain = std::clamp(intensity * 0.35f, 0.35f, 1.0f);
			const glm::vec3 tinted = glm::max(color * gain, castsShadow ? glm::vec3(0.25f, 0.22f, 0.08f) : glm::vec3(0.08f));
			return {std::clamp(tinted.x, 0.0f, 1.0f), std::clamp(tinted.y, 0.0f, 1.0f), std::clamp(tinted.z, 0.0f, 1.0f), 1.0f};
		}

		void MakeBasis(glm::vec3 direction, glm::vec3& right, glm::vec3& up)
		{
			if (glm::length(direction) <= 0.0001f)
			{
				direction = {0.0f, -1.0f, 0.0f};
			}
			direction = glm::normalize(direction);
			const glm::vec3 reference = std::abs(direction.y) > 0.95f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
			right = glm::normalize(glm::cross(reference, direction));
			up = glm::normalize(glm::cross(direction, right));
		}

		void AddDebugCircle(std::vector<DebugVertex>& out, glm::vec3 center, glm::vec3 normal, float radius, glm::vec4 color, int segments = 32)
		{
			if (radius <= 0.0f)
			{
				return;
			}

			glm::vec3 right{};
			glm::vec3 up{};
			MakeBasis(normal, right, up);

			constexpr float kTwoPi = 6.28318530718f;
			for (int i = 0; i < segments; ++i)
			{
				const float a0 = static_cast<float>(i) * kTwoPi / static_cast<float>(segments);
				const float a1 = static_cast<float>(i + 1) * kTwoPi / static_cast<float>(segments);
				const glm::vec3 p0 = center + (right * std::cos(a0) + up * std::sin(a0)) * radius;
				const glm::vec3 p1 = center + (right * std::cos(a1) + up * std::sin(a1)) * radius;
				AddDebugLine(out, p0, p1, color);
			}
		}

		struct LightGizmoOptions
		{
			bool pointVolumes = true;
			bool spotCones = true;
			bool sunDirection = true;
			bool shadowMarkers = true;
			float scale = 1.0f;
		};

		void AddLightGizmos(std::vector<DebugVertex>& out, const Renderer& renderer, const CameraManager& cameras, const LightGizmoOptions& options)
		{
			for (const Renderer::PointLight& light: renderer.GetPointLights())
			{
				const glm::vec4 color = LightDebugColor(light.color, light.intensity, light.castsShadow);
				const float centerSize = 0.25f * options.scale;
				if (options.pointVolumes)
				{
					AddDebugSphere(out, light.position, std::max(light.radius * options.scale, 0.05f), color, 24);
				}
				AddDebugLine(out, light.position - glm::vec3{centerSize, 0.0f, 0.0f}, light.position + glm::vec3{centerSize, 0.0f, 0.0f}, color);
				AddDebugLine(out, light.position - glm::vec3{0.0f, centerSize, 0.0f}, light.position + glm::vec3{0.0f, centerSize, 0.0f}, color);
				AddDebugLine(out, light.position - glm::vec3{0.0f, 0.0f, centerSize}, light.position + glm::vec3{0.0f, 0.0f, centerSize}, color);
				if (options.shadowMarkers && light.castsShadow)
				{
					AddDebugAabb(out, light.position - glm::vec3{0.18f}, light.position + glm::vec3{0.18f}, colors::Yellow);
				}
			}

			for (const Renderer::SpotLight& light: renderer.GetSpotLights())
			{
				glm::vec3 dir = glm::length(light.direction) > 0.0001f ? glm::normalize(light.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
				const float radius = std::max(light.radius * options.scale, 0.05f);
				const glm::vec4 color = LightDebugColor(light.color, light.intensity, light.castsShadow);
				const glm::vec3 coneCenter = light.position + dir * radius;
				const float outerRadius = std::tan(light.outerAngleRad) * radius;
				const float innerRadius = std::tan(light.innerAngleRad) * radius;

				AddDebugSphere(out, light.position, 0.16f * options.scale, color, 12);
				if (options.spotCones)
				{
					AddDebugLine(out, light.position, coneCenter, color);
					AddDebugCircle(out, coneCenter, dir, outerRadius, color);
					AddDebugCircle(out, coneCenter, dir, innerRadius, {color.x, color.y, color.z, 0.55f}, 24);

					glm::vec3 right{};
					glm::vec3 up{};
					MakeBasis(dir, right, up);
					AddDebugLine(out, light.position, coneCenter + right * outerRadius, color);
					AddDebugLine(out, light.position, coneCenter - right * outerRadius, color);
					AddDebugLine(out, light.position, coneCenter + up * outerRadius, color);
					AddDebugLine(out, light.position, coneCenter - up * outerRadius, color);
				}
				if (options.shadowMarkers && light.castsShadow)
				{
					AddDebugAabb(out, light.position - glm::vec3{0.15f}, light.position + glm::vec3{0.15f}, colors::Yellow);
				}
			}

			if (options.sunDirection)
			{
				if (const Camera* cam = cameras.TryGetMainCamera())
				{
					const glm::vec3 sunDir = glm::length(renderer.GetDirectionalLightDirection()) > 0.0001f ? glm::normalize(renderer.GetDirectionalLightDirection()) : glm::vec3{0.0f, -1.0f, 0.0f};
					const glm::vec3 anchor = cam->GetPosition() + cam->GetForward() * 4.0f + glm::vec3{0.0f, 1.5f, 0.0f};
					const glm::vec4 sunColor = LightDebugColor(renderer.GetSunColor(), renderer.GetDirectionalLightIntensity(), true);
					AddDebugLine(out, anchor - sunDir * (0.9f * options.scale), anchor + sunDir * (0.9f * options.scale), sunColor);
					AddDebugSphere(out, anchor + sunDir * (0.9f * options.scale), 0.18f * options.scale, sunColor, 12);
					AddDebugCircle(out, anchor, sunDir, 0.35f * options.scale, sunColor, 24);
				}
			}
		}
	} // namespace

	void LightingPanel::OnUpdate(LayerContext& context)
	{
		// The light-gizmo toggle lives on this panel's checkbox; the editor keeps
		// only scene-manipulation keybinds.
		if (m_lightGizmos && aether::IsDebugRenderingEnabled())
		{
			if (auto engine = context.TryGet<aether::AetherCore>())
			{
				AddLightGizmos(engine->GetPendingDebugVertices(),
				        context.Get<Renderer>(),
				        context.Get<CameraManager>(),
				        LightGizmoOptions{
				                .pointVolumes = m_lightGizmoPointVolumes,
				                .spotCones = m_lightGizmoSpotCones,
				                .sunDirection = m_lightGizmoSunDirection,
				                .shadowMarkers = m_lightGizmoShadowMarkers,
				                .scale = m_lightGizmoScale,
				        });
			}
		}
	}

	void LightingPanel::OnImGui(LayerContext& context)
	{
		ImGui::Begin("Lighting", VisiblePtr());
		{
			Renderer& renderer = context.Get<Renderer>();
			ImGui::Text("Point lights: %zu", renderer.GetPointLights().size());
			ImGui::Text("Spot lights: %zu", renderer.GetSpotLights().size());
			ImGui::Text("Sun intensity: %.2f", renderer.GetDirectionalLightIntensity());

			ImGui::SeparatorText("Gizmos");
			ImGui::Checkbox("Light gizmos", &m_lightGizmos);
			ImGui::Checkbox("Point light volumes", &m_lightGizmoPointVolumes);
			ImGui::Checkbox("Spot cones", &m_lightGizmoSpotCones);
			ImGui::Checkbox("Sun direction", &m_lightGizmoSunDirection);
			ImGui::Checkbox("Shadow markers", &m_lightGizmoShadowMarkers);
			ImGui::SliderFloat("Gizmo scale", &m_lightGizmoScale, 0.25f, 2.0f, "%.2f");
		}
		ImGui::End();
	}

	void LightingPanel::LoadSettings(TomlConfig& config, LayerContext& context)
	{
		(void) context;
		m_lightGizmos = config.GetBool("debug.lightgizmos", m_lightGizmos);
		m_lightGizmoPointVolumes = config.GetBool("debug.lightgizmopointvolumes", m_lightGizmoPointVolumes);
		m_lightGizmoSpotCones = config.GetBool("debug.lightgizmospotcones", m_lightGizmoSpotCones);
		m_lightGizmoSunDirection = config.GetBool("debug.lightgizmosundirection", m_lightGizmoSunDirection);
		m_lightGizmoShadowMarkers = config.GetBool("debug.lightgizmoshadowmarkers", m_lightGizmoShadowMarkers);
		m_lightGizmoScale = config.GetFloat("debug.lightgizmoscale", m_lightGizmoScale);
	}

	void LightingPanel::SaveSettings(TomlConfig& config, LayerContext& context) const
	{
		(void) context;
		config.Set("debug.lightgizmos", m_lightGizmos);
		config.Set("debug.lightgizmopointvolumes", m_lightGizmoPointVolumes);
		config.Set("debug.lightgizmospotcones", m_lightGizmoSpotCones);
		config.Set("debug.lightgizmosundirection", m_lightGizmoSunDirection);
		config.Set("debug.lightgizmoshadowmarkers", m_lightGizmoShadowMarkers);
		config.Set("debug.lightgizmoscale", m_lightGizmoScale);
	}
} // namespace aether::app
