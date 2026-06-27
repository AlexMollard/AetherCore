#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <regex>
#include <string_view>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/RenderGraphStorage.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	namespace
	{
		constexpr float kPanelW = 430.f;
		constexpr float kPanelH = 640.f;

		ImVec4 FpsColor(float fps) noexcept
		{
			if (fps >= 55.f)
			{
				return {0.40f, 0.72f, 0.46f, 1.f};
			}
			if (fps >= 30.f)
			{
				return {0.86f, 0.71f, 0.30f, 1.f};
			}
			return {0.80f, 0.33f, 0.30f, 1.f};
		}

		ImVec4 MsColor(float ms) noexcept
		{
			if (ms <= 16.667f)
			{
				return {0.40f, 0.72f, 0.46f, 1.f};
			}
			if (ms <= 25.f)
			{
				return {0.86f, 0.71f, 0.30f, 1.f};
			}
			return {0.80f, 0.33f, 0.30f, 1.f};
		}

		std::string ShortRenderPassName(std::string_view name)
		{
			const std::size_t sourceSuffix = name.find(" (");
			if (sourceSuffix != std::string_view::npos)
			{
				name = name.substr(0, sourceSuffix);
			}
			return std::string(name);
		}

		bool IsAlive(const World& world, Entity entity)
		{
			return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
		}

		std::string ComponentSummary(const World& world, Entity entity)
		{
			std::string out;
			auto add = [&](std::string_view name)
			{
				if (!out.empty())
				{
					out += ", ";
				}
				out += name;
			};

			if (world.Has<TransformComponent>(entity))
			{
				add("Transform");
			}
			if (world.Has<MeshComponent>(entity))
			{
				add("Mesh");
			}
			if (world.Has<MaterialComponent>(entity))
			{
				add("Material");
			}
			if (world.Has<PipelineComponent>(entity))
			{
				add("Pipeline");
			}
			if (world.Has<SkinnedMeshComponent>(entity))
			{
				add("Skinned");
			}
			if (world.Has<SpawnedEntitiesComponent>(entity))
			{
				add("Spawned");
			}
			if (world.Has<ParentEntityComponent>(entity))
			{
				add("Child");
			}
			if (world.Has<RigidBodyComponent>(entity))
			{
				add("RigidBody");
			}
			if (world.Has<PhysicsStateComponent>(entity))
			{
				add("Physics");
			}
			if (world.Has<PhysicsDebugShapeComponent>(entity))
			{
				add("DebugShape");
			}

			return out.empty() ? "Entity" : out;
		}

		std::string SceneEntityLabel(const World& world, Entity entity)
		{
			return std::format("#{}  {}", entity.id, ComponentSummary(world, entity));
		}

		std::vector<Entity> CollectSceneEntities(const World& world)
		{
			std::vector<Entity> entities;
			std::unordered_set<std::uint32_t> seen;
			auto addCandidate = [&](Entity entity)
			{
				if (IsAlive(world, entity) && seen.insert(entity.id).second)
				{
					entities.push_back(entity);
				}
			};

			for (const auto& [raw, component]: world.View<TransformComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<MeshComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<MaterialComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<PipelineComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<SkinnedMeshComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<SpawnedEntitiesComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<ParentEntityComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<RigidBodyComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<PhysicsStateComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}
			for (const auto& [raw, component]: world.View<PhysicsDebugShapeComponent>().each())
			{
				(void) component;
				addCandidate(World::FromEntt(raw));
			}

			std::ranges::sort(entities, [](Entity a, Entity b) { return a.id < b.id; });
			return entities;
		}

		void DrawMetricRow(const char* label, const char* value, ImVec4 color = {0.86f, 0.88f, 0.90f, 1.0f})
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextColored(color, "%s", value);
		}

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
					AddDebugAabb(out, light.position - glm::vec3{0.18f}, light.position + glm::vec3{0.18f}, {1.0f, 0.88f, 0.20f, 1.0f});
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
					AddDebugAabb(out, light.position - glm::vec3{0.15f}, light.position + glm::vec3{0.15f}, {1.0f, 0.88f, 0.20f, 1.0f});
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

	const char* DebugLayer::GetTonemapModeName(aether::TonemapMode mode)
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
	}

	void DebugLayer::ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine)
	{
		outPath.clear();
		outLine = 0;

		std::size_t searchPos = 0;
		while (searchPos < error.size())
		{
			const auto dasPos = error.find(".das:", searchPos);
			if (dasPos == std::string::npos)
			{
				break;
			}

			const std::size_t colonPos = dasPos + 4;
			if (colonPos >= error.size() || !std::isdigit(static_cast<unsigned char>(error[colonPos])))
			{
				searchPos = dasPos + 1;
				continue;
			}

			std::size_t start = dasPos;
			while (start > 0 && error[start - 1] != ' ' && error[start - 1] != '\n' && error[start - 1] != '\r')
			{
				--start;
			}

			outPath = error.substr(start, dasPos + 4 - start);

			std::size_t lineStart = colonPos + 1;
			std::size_t lineEnd = lineStart;
			while (lineEnd < error.size() && std::isdigit(static_cast<unsigned char>(error[lineEnd])))
			{
				++lineEnd;
			}

			if (lineEnd > lineStart)
			{
				try
				{
					outLine = std::stoi(error.substr(lineStart, lineEnd - lineStart));
				}
				catch (...)
				{
					outLine = 0;
				}
			}

			if (outLine > 0)
			{
				break;
			}

			searchPos = dasPos + 1;
		}
	}

	void DebugLayer::OpenInVSCode(const std::string& filePath, int line)
	{
		if (filePath.empty())
		{
			return;
		}

		std::string resolved = filePath;
		if (!std::filesystem::path(filePath).is_absolute())
		{
			std::error_code ec;
			auto candidate = std::filesystem::weakly_canonical(std::filesystem::current_path() / ".." / ".." / filePath, ec);
			if (!ec && std::filesystem::exists(candidate, ec))
			{
				resolved = candidate.string();
			}
		}

#ifdef _WIN32
		std::string cmd;
		if (line > 0)
		{
			cmd = "code -g \"" + resolved + ":" + std::to_string(line) + "\"";
		}
		else
		{
			cmd = "code \"" + resolved + "\"";
		}

		int ret = system(("where code >nul 2>&1 && " + cmd).c_str());
		if (ret == 0)
		{
			return;
		}

		ShellExecuteA(nullptr, "open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		std::string shellCmd = "code " + resolved;
		if (line > 0)
		{
			shellCmd += ":" + std::to_string(line);
		}
		system(shellCmd.c_str());
#endif
	}

	void DebugLayer::PollScriptErrors(LayerContext& context)
	{
		auto scripting = context.TryGet<scripting::ScriptingSubsystem>();
		if (!scripting)
		{
			return;
		}

		if (scripting->ConsumeErrorsCleared())
		{
			m_errorToasts.clear();
		}

		auto errors = scripting->PollPendingErrors();
		if (!errors.empty())
		{
			m_errorToasts.clear();
		}

		for (auto& err: errors)
		{
			std::vector<std::string> individualErrors;
			std::regex errorPattern(R"(error\[\d+\]:)");
			auto begin = std::sregex_iterator(err.begin(), err.end(), errorPattern);
			auto end = std::sregex_iterator();

			if (begin == end)
			{
				individualErrors.push_back(err);
			}
			else
			{
				std::size_t lastPos = 0;
				for (auto it = begin; it != end; ++it)
				{
					const std::smatch& match = *it;
					if (it == begin)
					{
						lastPos = match.position();
					}
					else
					{
						individualErrors.push_back(err.substr(lastPos, match.position() - lastPos));
						lastPos = match.position();
					}
				}
				individualErrors.push_back(err.substr(lastPos));
			}

			for (const auto& singleErr: individualErrors)
			{
				if (singleErr.empty())
				{
					continue;
				}

				ScriptErrorToast toast;
				toast.message = singleErr;

				std::size_t pos = 0;
				while (pos < singleErr.size())
				{
					auto lineEnd = singleErr.find('\n', pos);
					if (lineEnd == std::string::npos)
					{
						lineEnd = singleErr.size();
					}
					std::string line = singleErr.substr(pos, lineEnd - pos);

					std::size_t first = line.find_first_not_of(" \t");
					if (first != std::string::npos && !line.empty())
					{
						toast.summary = line.substr(first);
						break;
					}
					pos = lineEnd + 1;
				}

				ParseErrorLocation(singleErr, toast.filePath, toast.line);
				m_errorToasts.push_back(std::move(toast));
			}
		}
	}

	void DebugLayer::PushFrameSample(float frameMs)
	{
		m_frameSamples[m_frameSampleHead] = frameMs;
		m_frameSampleHead = (m_frameSampleHead + 1) % m_frameSamples.size();
		m_frameSampleCount = std::min(m_frameSampleCount + 1, m_frameSamples.size());
	}

	void DebugLayer::OnAttach(LayerContext&)
	{
		AE_PROFILE_ZONE();
	}

	void DebugLayer::OnDetach(LayerContext&)
	{
		AE_PROFILE_ZONE();
		m_errorToasts.clear();
		m_selectedSceneEntity = {};
		m_expandedSceneEntities.clear();
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		const Input& input = context.Get<Input>();

		if (input.IsKeyPressed(aether::Key::F1))
		{
			m_visible = !m_visible;
		}

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto scripting = context.TryGet<scripting::ScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

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
			AE_INFO(aether::LogCategory::App, "Tonemap: {}", GetTonemapModeName(next));
		}

		if (input.IsKeyPressed(aether::Key::F6))
		{
			const bool newState = !aether::IsDebugRenderingEnabled();
			aether::SetDebugRenderingEnabled(newState);
			AE_INFO(aether::LogCategory::App, "Debug renderer: {}", newState ? "on" : "off");
		}

		if (input.IsKeyPressed(aether::Key::F7))
		{
			m_debugTestShapes = !m_debugTestShapes;
			AE_INFO(aether::LogCategory::App, "Debug test shapes: {}", m_debugTestShapes ? "on" : "off");
		}

		if (input.IsKeyPressed(aether::Key::F8))
		{
			auto& rs = context.Get<aether::RenderingSubsystem>();
			const bool newState = !rs.IsForwardPassEnabled();
			rs.SetForwardPassEnabled(newState);
			AE_INFO(aether::LogCategory::App, "Forward render: {}", newState ? "on" : "off");
		}

		if (input.IsKeyPressed(aether::Key::F9))
		{
			m_lightGizmos = !m_lightGizmos;
			AE_INFO(aether::LogCategory::App, "Light gizmos: {}", m_lightGizmos ? "on" : "off");
		}

		if (m_debugTestShapes)
		{
			if (auto engine = context.TryGet<aether::AetherCore>())
			{
				auto& verts = engine->GetPendingDebugVertices();
				if (const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera())
				{
					const glm::vec3 boxCenter = cam->GetPosition() + cam->GetForward() * 2.0f;
					AddDebugAabb(verts, boxCenter - glm::vec3(0.5f), boxCenter + glm::vec3(0.5f), glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
					AddDebugAxes(verts, glm::translate(glm::mat4(1.0f), boxCenter), 0.75f);
				}

				AddDebugAabb(verts, glm::vec3(-2.5f), glm::vec3(2.5f), glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));
				AddDebugSphere(verts, glm::vec3(0.0f), 2.0f, glm::vec4(0.2f, 0.85f, 1.0f, 1.0f), 16);
				AddDebugLine(verts, glm::vec3(0.0f, -5.0f, 0.0f), glm::vec3(0.0f, 5.0f, 0.0f), glm::vec4(0.3f, 0.4f, 0.5f, 1.0f));
			}
		}

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

		PollScriptErrors(context);
		PushFrameSample(static_cast<float>(context.deltaTimeSeconds * 1000.0));
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (!m_errorToasts.empty())
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + viewport->WorkSize.y - 88.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - 32.0f, 68.0f), ImGuiCond_Always);
			ImGui::Begin("Script Errors", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			const auto& toast = m_errorToasts.front();
			ImGui::TextColored(ImVec4(1.0f, 0.32f, 0.30f, 1.0f), "Script Error%s", m_errorToasts.size() > 1 ? "s" : "");
			ImGui::SameLine();
			ImGui::TextUnformatted(toast.summary.c_str());
			if (!toast.filePath.empty())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Open"))
				{
					OpenInVSCode(toast.filePath, toast.line);
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Dismiss All"))
			{
				m_errorToasts.clear();
			}
			ImGui::End();
		}

		if (!m_visible)
		{
			return;
		}

		const gpu::Extent2D ext = context.Get<Swapchain>().GetExtent();
		ImGui::SetNextWindowPos(ImVec2(static_cast<float>(ext.width) - kPanelW - 16.0f, 16.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(kPanelW, kPanelH), ImGuiCond_FirstUseEver);

		const float curMs = m_frameSampleCount > 0 ? m_frameSamples[(m_frameSampleHead + m_frameSamples.size() - 1) % m_frameSamples.size()] : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		float totalMs = 0.0f;
		float minMs = curMs;
		float maxMs = curMs;
		std::array<float, kFrameSampleCount> orderedSamples{};
		for (std::size_t i = 0; i < m_frameSampleCount; ++i)
		{
			const std::size_t idx = (m_frameSampleHead + m_frameSamples.size() - m_frameSampleCount + i) % m_frameSamples.size();
			const float sample = m_frameSamples[idx];
			orderedSamples[i] = sample;
			totalMs += sample;
			minMs = std::min(minMs, sample);
			maxMs = std::max(maxMs, sample);
		}
		const float avgMs = m_frameSampleCount > 0 ? totalMs / static_cast<float>(m_frameSampleCount) : curMs;
		const float curFps = curMs > 0.0f ? 1000.0f / curMs : 0.0f;
		const float avgFps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;

		const std::string windowTitle = std::format("Debug  |  {:.0f} FPS  |  {:.2f} ms###DebugPanel", curFps, curMs);
		if (!ImGui::Begin(windowTitle.c_str(), &m_visible))
		{
			ImGui::End();
			return;
		}

		if (ImGui::BeginTabBar("DebugTabs"))
		{
			if (ImGui::BeginTabItem("Perf"))
			{
				if (ImGui::BeginTable("PerfStats", 2, ImGuiTableFlags_SizingStretchProp))
				{
					DrawMetricRow("Frame", std::format("#{}", context.frameIndex).c_str());
					DrawMetricRow("FPS", std::format("{:.1f}", curFps).c_str(), FpsColor(curFps));
					DrawMetricRow("Delta", std::format("{:.2f} ms", curMs).c_str(), MsColor(curMs));
					DrawMetricRow("Avg FPS", std::format("{:.1f}", avgFps).c_str(), FpsColor(avgFps));
					DrawMetricRow("Min", std::format("{:.2f} ms", minMs).c_str(), {0.40f, 0.72f, 0.46f, 1.f});
					DrawMetricRow("Max", std::format("{:.2f} ms", maxMs).c_str(), MsColor(maxMs));
					ImGui::EndTable();
				}
				ImGui::PlotLines("Frame Time", orderedSamples.data(), static_cast<int>(m_frameSampleCount), 0, "0 - 33 ms", 0.0f, 33.333f, ImVec2(-1.0f, 120.0f));
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Render"))
			{
				Renderer& renderer = context.Get<Renderer>();
				auto& rendering = context.Get<aether::RenderingSubsystem>();
				RenderQueue& renderQueue = context.Get<RenderQueue>();

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

				bool forward = rendering.IsForwardPassEnabled();
				if (ImGui::Checkbox("Forward pass", &forward))
				{
					rendering.SetForwardPassEnabled(forward);
				}

				bool forceVisible = renderQueue.IsDebugForceVisible();
				if (ImGui::Checkbox("Force visible", &forceVisible))
				{
					renderQueue.SetDebugForceVisible(forceVisible);
				}
				bool bypassIndirect = renderQueue.IsDebugBypassIndirect();
				if (ImGui::Checkbox("Bypass indirect", &bypassIndirect))
				{
					renderQueue.SetDebugBypassIndirect(bypassIndirect);
				}
				bool disableAnimation = renderQueue.IsDebugDisableAnimation();
				if (ImGui::Checkbox("Disable animation", &disableAnimation))
				{
					renderQueue.SetDebugDisableAnimation(disableAnimation);
				}

				ImGui::Separator();
				ImGui::Text("Resolution: %u x %u", ext.width, ext.height);
				ImGui::Text("Point lights: %zu", renderer.GetPointLights().size());
				ImGui::Text("Spot lights: %zu", renderer.GetSpotLights().size());
				ImGui::Text("Sun intensity: %.2f", renderer.GetDirectionalLightIntensity());

				if (auto rg = context.TryGet<aether::RenderGraph>())
				{
					const auto passes = rg->GetPasses();
					float passTotalMs = 0.0f;
					for (const auto& pass: passes)
					{
						passTotalMs += pass.lastCpuTimeMs;
					}

					ImGui::SeparatorText("Render Graph");
					const auto& stats = rg->GetFrameStats();
					if (ImGui::BeginTable("RenderGraphStats", 2, ImGuiTableFlags_SizingStretchProp))
					{
						DrawMetricRow("Passes", std::format("{}", stats.passCount).c_str());
						DrawMetricRow("Barriers", std::format("{}", stats.barrierCount).c_str());
						DrawMetricRow("Transient hits", std::format("{}", stats.transientCacheHit).c_str(), {0.40f, 0.72f, 0.46f, 1.f});
						DrawMetricRow("Transient misses", std::format("{}", stats.transientCacheMiss).c_str(), stats.transientCacheMiss == 0 ? ImVec4{0.40f, 0.72f, 0.46f, 1.f} : ImVec4{0.86f, 0.71f, 0.30f, 1.f});
						DrawMetricRow("Cache size", std::format("{}", stats.cacheSize).c_str());
						ImGui::EndTable();
					}

					if (ImGui::BeginTable("RenderPasses", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
					{
						ImGui::TableSetupColumn("Pass");
						ImGui::TableSetupColumn("CPU");
						ImGui::TableSetupColumn("Share");
						ImGui::TableHeadersRow();
						const std::size_t count = std::min<std::size_t>(passes.size(), kMaxRenderPassRows);
						for (std::size_t i = 0; i < count; ++i)
						{
							const float pct = passTotalMs > 0.0f ? passes[i].lastCpuTimeMs / passTotalMs : 0.0f;
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextUnformatted(ShortRenderPassName(passes[i].name).c_str());
							ImGui::TableSetColumnIndex(1);
							ImGui::TextColored(MsColor(passes[i].lastCpuTimeMs), "%.2f ms", passes[i].lastCpuTimeMs);
							ImGui::TableSetColumnIndex(2);
							ImGui::ProgressBar(pct, ImVec2(-1.0f, 0.0f), "");
						}
						ImGui::EndTable();
					}
					ImGui::Text("Total CPU: %.2f ms", passTotalMs);
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Debug"))
			{
				bool debugRenderer = aether::IsDebugRenderingEnabled();
				if (ImGui::Checkbox("Debug overlay", &debugRenderer))
				{
					aether::SetDebugRenderingEnabled(debugRenderer);
				}

				bool physicsShapes = aether::IsPhysicsDebugShapesEnabled();
				if (ImGui::Checkbox("Physics debug rendering", &physicsShapes))
				{
					aether::SetPhysicsDebugShapesEnabled(physicsShapes);
				}

				auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
				bool selfTest = physicsDebug.IsSelfTestEnabled();
				if (ImGui::Checkbox("Physics renderer self-test", &selfTest))
				{
					physicsDebug.SetSelfTestEnabled(selfTest);
				}

				ImGui::Checkbox("Test shapes", &m_debugTestShapes);
				ImGui::Checkbox("Light gizmos", &m_lightGizmos);
				ImGui::Checkbox("Point light volumes", &m_lightGizmoPointVolumes);
				ImGui::Checkbox("Spot cones", &m_lightGizmoSpotCones);
				ImGui::Checkbox("Sun direction", &m_lightGizmoSunDirection);
				ImGui::Checkbox("Shadow markers", &m_lightGizmoShadowMarkers);
				ImGui::SliderFloat("Light gizmo scale", &m_lightGizmoScale, 0.25f, 2.0f, "%.2f");

				if (ImGui::Button("Reload Script"))
				{
					if (auto scripting = context.TryGet<scripting::ScriptingSubsystem>())
					{
						scripting->RequestReload();
					}
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Camera"))
			{
				if (const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera())
				{
					const glm::vec3 pos = cam->GetPosition();
					const glm::vec3 fwd = cam->GetForward();
					if (ImGui::BeginTable("CameraStats", 2, ImGuiTableFlags_SizingStretchProp))
					{
						DrawMetricRow("Position", std::format("{:.2f}, {:.2f}, {:.2f}", pos.x, pos.y, pos.z).c_str());
						DrawMetricRow("Forward", std::format("{:.2f}, {:.2f}, {:.2f}", fwd.x, fwd.y, fwd.z).c_str());
						DrawMetricRow("FOV", std::format("{:.0f} deg", cam->GetFovDegrees()).c_str());
						DrawMetricRow("Near", std::format("{:.2f}", cam->GetNearPlane()).c_str());
						DrawMetricRow("Far", std::format("{:.0f}", cam->GetFarPlane()).c_str());
						ImGui::EndTable();
					}
				}
				else
				{
					ImGui::TextUnformatted("No active camera");
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Scene"))
			{
				World& world = context.Get<World>();
				const auto entities = CollectSceneEntities(world);
				if (!IsAlive(world, m_selectedSceneEntity))
				{
					m_selectedSceneEntity = {};
				}

				ImGui::Text("%zu entities", entities.size());
				ImGui::BeginChild("SceneList", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders);
				const std::size_t count = std::min<std::size_t>(entities.size(), kMaxSceneRows);
				for (std::size_t i = 0; i < count; ++i)
				{
					const Entity entity = entities[i];
					const bool selected = entity == m_selectedSceneEntity;
					if (ImGui::Selectable(SceneEntityLabel(world, entity).c_str(), selected))
					{
						m_selectedSceneEntity = entity;
					}
				}
				if (entities.size() > count)
				{
					ImGui::TextDisabled("Showing first %zu of %zu", count, entities.size());
				}
				ImGui::EndChild();

				ImGui::SeparatorText("Inspector");
				if (!IsAlive(world, m_selectedSceneEntity))
				{
					ImGui::TextDisabled("No selection");
				}
				else
				{
					ImGui::Text("#%u", m_selectedSceneEntity.id);
					ImGui::Text("Components: %s", ComponentSummary(world, m_selectedSceneEntity).c_str());
					if (const auto transform = world.TryGet<TransformComponent>(m_selectedSceneEntity))
					{
						const glm::vec3 pos = glm::vec3(transform->localToWorld[3]);
						ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
					}
					if (const auto skinned = world.TryGet<SkinnedMeshComponent>(m_selectedSceneEntity))
					{
						ImGui::Text("Animation: clip %u, time %.2f, speed %.2f", skinned->clipIndex, skinned->animTime, skinned->playbackSpeed);
					}
					if (const auto rigid = world.TryGet<RigidBodyComponent>(m_selectedSceneEntity))
					{
						const char* motion = "Dynamic";
						if (rigid->motionType == PhysicsMotionType::Static)
						{
							motion = "Static";
						}
						else if (rigid->motionType == PhysicsMotionType::Kinematic)
						{
							motion = "Kinematic";
						}
						ImGui::Text("Rigid body: %s", motion);
					}
					if (const auto physics = world.TryGet<PhysicsStateComponent>(m_selectedSceneEntity))
					{
						ImGui::Text("Physics pos: %.2f, %.2f, %.2f", physics->currPosition.x, physics->currPosition.y, physics->currPosition.z);
						ImGui::Text("Physics scale: %.2f, %.2f, %.2f", physics->scale.x, physics->scale.y, physics->scale.z);
					}
				}
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::End();
	}
} // namespace aether::app
