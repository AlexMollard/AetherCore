#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <regex>
#include <string_view>
#include <type_traits>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "Color.hpp"
#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "passes/PostProcessStack.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"
#include "vulkan/RenderGraphStorage.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	namespace
	{
		ImVec4 FpsColor(float fps) noexcept
		{
			using colors::Success, colors::Warn, colors::Error;
			if (fps >= 55.f)
			{
				return {Success.r, Success.g, Success.b, Success.a};
			}
			if (fps >= 30.f)
			{
				return {Warn.r, Warn.g, Warn.b, Warn.a};
			}
			return {Error.r, Error.g, Error.b, Error.a};
		}

		ImVec4 MsColor(float ms) noexcept
		{
			using colors::Success, colors::Warn, colors::Error;
			if (ms <= 16.667f)
			{
				return {Success.r, Success.g, Success.b, Success.a};
			}
			if (ms <= 25.f)
			{
				return {Warn.r, Warn.g, Warn.b, Warn.a};
			}
			return {Error.r, Error.g, Error.b, Error.a};
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

		const char* FormatName(gpu::Format format) noexcept
		{
			switch (format)
			{
				case gpu::Format::R8Unorm:
					return "R8Unorm";
				case gpu::Format::R8G8B8A8Unorm:
					return "R8G8B8A8Unorm";
				case gpu::Format::R8G8B8A8Srgb:
					return "R8G8B8A8Srgb";
				case gpu::Format::B8G8R8A8Unorm:
					return "B8G8R8A8Unorm";
				case gpu::Format::B8G8R8A8Srgb:
					return "B8G8R8A8Srgb";
				case gpu::Format::R16G16B16A16Sfloat:
					return "R16G16B16A16Sfloat";
				case gpu::Format::R32G32Sfloat:
					return "R32G32Sfloat";
				case gpu::Format::R32G32B32Sfloat:
					return "R32G32B32Sfloat";
				case gpu::Format::R32G32B32A32Sfloat:
					return "R32G32B32A32Sfloat";
				case gpu::Format::D16Unorm:
					return "D16Unorm";
				case gpu::Format::D24UnormS8Uint:
					return "D24UnormS8Uint";
				case gpu::Format::X8D24UnormPack32:
					return "X8D24UnormPack32";
				case gpu::Format::D32Sfloat:
					return "D32Sfloat";
				case gpu::Format::D16UnormS8Uint:
					return "D16UnormS8Uint";
				case gpu::Format::D32SfloatS8Uint:
					return "D32SfloatS8Uint";
				case gpu::Format::BC4UnormBlock:
					return "BC4UnormBlock";
				case gpu::Format::BC7UnormBlock:
					return "BC7UnormBlock";
				case gpu::Format::BC7SrgbBlock:
					return "BC7SrgbBlock";
				case gpu::Format::Undefined:
				default:
					return "Undefined";
			}
		}

		template<typename Enum>
		bool HasFlag(Enum value, Enum flag) noexcept
		{
			using Underlying = std::underlying_type_t<Enum>;
			return (static_cast<Underlying>(value) & static_cast<Underlying>(flag)) != 0;
		}

		std::string ImageUsageText(gpu::ImageUsage usage)
		{
			std::string text;
			auto add = [&](std::string_view part)
			{
				if (!text.empty())
				{
					text += " | ";
				}
				text += part;
			};
			if (HasFlag(usage, gpu::ImageUsage::TransferSrc))
			{
				add("TransferSrc");
			}
			if (HasFlag(usage, gpu::ImageUsage::TransferDst))
			{
				add("TransferDst");
			}
			if (HasFlag(usage, gpu::ImageUsage::Sampled))
			{
				add("Sampled");
			}
			if (HasFlag(usage, gpu::ImageUsage::Storage))
			{
				add("Storage");
			}
			if (HasFlag(usage, gpu::ImageUsage::ColorAttachment))
			{
				add("ColorAttachment");
			}
			if (HasFlag(usage, gpu::ImageUsage::DepthStencilAttachment))
			{
				add("DepthStencilAttachment");
			}
			if (HasFlag(usage, gpu::ImageUsage::HostTransfer))
			{
				add("HostTransfer");
			}
			return text.empty() ? "None" : text;
		}

		std::string ImageAspectText(gpu::ImageAspect aspect)
		{
			std::string text;
			auto add = [&](std::string_view part)
			{
				if (!text.empty())
				{
					text += " | ";
				}
				text += part;
			};
			if (HasFlag(aspect, gpu::ImageAspect::Color))
			{
				add("Color");
			}
			if (HasFlag(aspect, gpu::ImageAspect::Depth))
			{
				add("Depth");
			}
			if (HasFlag(aspect, gpu::ImageAspect::Stencil))
			{
				add("Stencil");
			}
			return text.empty() ? "None" : text;
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

		void DrawMetricRow(const char* label, const char* value, ImVec4 color = {colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a})
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
			static const std::regex errorPattern(R"(error\[\d+\]:)");
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

	void DebugLayer::LoadSettings(LayerContext& context)
	{
		if (!m_debugConfig.LoadFile("debug"))
		{
			return;
		}

		m_visible = m_debugConfig.GetBool("debug.visible", m_visible);
		m_debugTestShapes = m_debugConfig.GetBool("debug.testshapes", m_debugTestShapes);
		m_lightGizmos = m_debugConfig.GetBool("debug.lightgizmos", m_lightGizmos);
		m_lightGizmoPointVolumes = m_debugConfig.GetBool("debug.lightgizmopointvolumes", m_lightGizmoPointVolumes);
		m_lightGizmoSpotCones = m_debugConfig.GetBool("debug.lightgizmospotcones", m_lightGizmoSpotCones);
		m_lightGizmoSunDirection = m_debugConfig.GetBool("debug.lightgizmosundirection", m_lightGizmoSunDirection);
		m_lightGizmoShadowMarkers = m_debugConfig.GetBool("debug.lightgizmoshadowmarkers", m_lightGizmoShadowMarkers);
		m_lightGizmoScale = m_debugConfig.GetFloat("debug.lightgizmoscale", m_lightGizmoScale);

		bool overlay = m_debugConfig.GetBool("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		aether::SetDebugRenderingEnabled(overlay);

		bool physicsShapes = m_debugConfig.GetBool("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
		aether::SetPhysicsDebugShapesEnabled(physicsShapes);

		auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
		bool selfTest = m_debugConfig.GetBool("debug.physicsdebugselftest", physicsDebug.IsSelfTestEnabled());
		physicsDebug.SetSelfTestEnabled(selfTest);

		AE_INFO(LogCategory::App, "Debug settings loaded");
	}

	void DebugLayer::SaveSettings(LayerContext& context)
	{
		m_debugConfig.Set("debug.visible", m_visible);
		m_debugConfig.Set("debug.testshapes", m_debugTestShapes);
		m_debugConfig.Set("debug.lightgizmos", m_lightGizmos);
		m_debugConfig.Set("debug.lightgizmopointvolumes", m_lightGizmoPointVolumes);
		m_debugConfig.Set("debug.lightgizmospotcones", m_lightGizmoSpotCones);
		m_debugConfig.Set("debug.lightgizmosundirection", m_lightGizmoSunDirection);
		m_debugConfig.Set("debug.lightgizmoshadowmarkers", m_lightGizmoShadowMarkers);
		m_debugConfig.Set("debug.lightgizmoscale", m_lightGizmoScale);
		m_debugConfig.Set("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		m_debugConfig.Set("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());

		auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
		m_debugConfig.Set("debug.physicsdebugselftest", physicsDebug.IsSelfTestEnabled());

		if (m_debugConfig.SaveIfDirty("debug", "Debug layer settings"))
		{
			AE_INFO(LogCategory::App, "Debug settings saved");
		}
	}

	void DebugLayer::ReleaseSceneViewportTexture(LayerContext& context)
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

	void DebugLayer::ReleaseTextureInspectorTextures(LayerContext& context)
	{
		if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
		{
			for (const auto& [_, textureId]: m_textureInspectorTextureIds)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(textureId));
			}
		}
		m_textureInspectorTextureIds.clear();
	}

	void DebugLayer::DrawSceneViewport(LayerContext& context)
	{
		ImGui::Begin("Viewport");

		auto& rendering = context.Get<aether::RenderingSubsystem>();
		auto& post = rendering.GetPostProcessStack();
		const gpu::ImageView imageView = post.GetFinalColorImageView();

		if (imageView != m_sceneViewportImageView)
		{
			ReleaseSceneViewportTexture(context);
			if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				const ImTextureID textureId = imgui->RegisterTexture(imageView, gpu::ImageLayout::ShaderReadOnly);
				m_sceneViewportTextureId = static_cast<std::uint64_t>(textureId);
				m_sceneViewportImageView = imageView;
			}
		}

		if (m_sceneViewportTextureId == 0)
		{
			context.Get<Input>().ClearMouseViewportTransform();
			ImGui::TextDisabled("Scene viewport texture unavailable");
			ImGui::End();
			return;
		}

		const ImVec2 available = ImGui::GetContentRegionAvail();
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
		if (m_viewportDisplayMode == 2)
		{
			imageSize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
		}
		else if (m_viewportDisplayMode == 3)
		{
			const float sx = extent.width > 0 ? std::floor(available.x / static_cast<float>(extent.width)) : 1.0f;
			const float sy = extent.height > 0 ? std::floor(available.y / static_cast<float>(extent.height)) : 1.0f;
			const float scale = std::max(1.0f, std::min(sx, sy));
			imageSize = ImVec2(static_cast<float>(extent.width) * scale, static_cast<float>(extent.height) * scale);
		}
		else if (m_viewportDisplayMode == 1)
		{
			if (imageSize.x < imageSize.y * targetAspect)
			{
				imageSize.x = imageSize.y * targetAspect;
			}
			else
			{
				imageSize.y = imageSize.x / targetAspect;
			}
		}
		else if (m_viewportAspectMode != 1)
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

		const ImVec2 cursor = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(cursor.x + (available.x - imageSize.x) * 0.5f, cursor.y + (available.y - imageSize.y) * 0.5f));
		if (imageSize.x <= 0.0f || imageSize.y <= 0.0f)
		{
			context.Get<Input>().SetMouseViewportInputActive(false);
			ImGui::End();
			return;
		}

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
		ImGui::End();
	}

	void DebugLayer::DrawTextureInspector(LayerContext& context)
	{
		ImGui::Begin("Textures");

		const std::vector<gpu::DebugTextureInfo> textures = gpu::ResourceRegistry::ListDebugTextures();
		if (textures.empty())
		{
			ImGui::TextDisabled("No registered textures");
			ImGui::End();
			return;
		}

		if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
		{
			for (auto it = m_textureInspectorTextureIds.begin(); it != m_textureInspectorTextureIds.end();)
			{
				const bool stillLive = std::ranges::any_of(textures, [bits = it->first](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == bits; });
				if (!stillLive)
				{
					imgui->UnregisterTexture(static_cast<ImTextureID>(it->second));
					it = m_textureInspectorTextureIds.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		const auto selectedIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		if (selectedIt == textures.end())
		{
			m_selectedTextureBits = textures.front().handle.bits;
		}

		if (ImGui::BeginTable("TextureList", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f)))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Extent", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 140.0f);
			ImGui::TableSetupColumn("Bindless", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableHeadersRow();

			for (const gpu::DebugTextureInfo& texture: textures)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool selected = texture.handle.bits == m_selectedTextureBits;
				const std::string label = std::format("{}###tex{}", ShortRenderPassName(texture.debugName), texture.handle.bits);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_selectedTextureBits = texture.handle.bits;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u x %u", texture.extent.width, texture.extent.height);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(FormatName(texture.format));
				ImGui::TableSetColumnIndex(3);
				if (texture.hasBindlessSampled)
				{
					ImGui::Text("%u", texture.bindlessSampledSlot);
				}
				else
				{
					ImGui::TextDisabled("-");
				}
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("0x%08X", texture.handle.bits);
			}
			ImGui::EndTable();
		}

		const auto currentIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		if (currentIt == textures.end())
		{
			ImGui::End();
			return;
		}

		const gpu::DebugTextureInfo& texture = *currentIt;
		ImGui::SeparatorText("Preview");
		const char* channelNames[] = {"RGBA", "Red", "Green", "Blue"};
		m_texturePreviewChannel = std::clamp(m_texturePreviewChannel, 0, static_cast<int>(std::size(channelNames)) - 1);
		ImGui::Combo("Channel tint", &m_texturePreviewChannel, channelNames, static_cast<int>(std::size(channelNames)));
		ImGui::SliderFloat("Zoom", &m_texturePreviewZoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		ImGui::Checkbox("Checkerboard", &m_texturePreviewCheckerboard);

		const bool canPreview = texture.view != nullptr && HasFlag(texture.usage, gpu::ImageUsage::Sampled) && HasFlag(texture.aspect, gpu::ImageAspect::Color);
		if (canPreview)
		{
			std::uint64_t& cachedTextureId = m_textureInspectorTextureIds[texture.handle.bits];
			if (cachedTextureId == 0)
			{
				if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
				{
					cachedTextureId = static_cast<std::uint64_t>(imgui->RegisterTexture(texture.view, gpu::ImageLayout::ShaderReadOnly));
				}
			}

			if (cachedTextureId != 0)
			{
				const ImVec2 region = ImGui::GetContentRegionAvail();
				const float aspect = texture.extent.height > 0 ? static_cast<float>(texture.extent.width) / static_cast<float>(texture.extent.height) : 1.0f;
				ImVec2 previewSize(std::min(region.x, static_cast<float>(texture.extent.width) * m_texturePreviewZoom), 0.0f);
				previewSize.y = previewSize.x / aspect;
				if (previewSize.y > region.y)
				{
					previewSize.y = region.y;
					previewSize.x = previewSize.y * aspect;
				}

				if (m_texturePreviewCheckerboard)
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					const float cell = 12.0f;
					for (float y = 0.0f; y < previewSize.y; y += cell)
					{
						for (float x = 0.0f; x < previewSize.x; x += cell)
						{
							const bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2 == 0;
							drawList->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + cell, previewSize.x), p.y + std::min(y + cell, previewSize.y)), dark ? IM_COL32(72, 76, 84, 255) : IM_COL32(112, 116, 124, 255));
						}
					}
				}
				const ImU32 tint = m_texturePreviewChannel == 1 ? IM_COL32(255, 0, 0, 255) : m_texturePreviewChannel == 2 ? IM_COL32(0, 255, 0, 255) : m_texturePreviewChannel == 3 ? IM_COL32(0, 0, 255, 255) : IM_COL32_WHITE;
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(cachedTextureId)), p, ImVec2(p.x + previewSize.x, p.y + previewSize.y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
				ImGui::Dummy(previewSize);
			}
		}
		else
		{
			ImGui::TextDisabled("Preview unavailable for this texture");
		}

		ImGui::SeparatorText("Metadata");
		if (ImGui::BeginTable("TextureMetadata", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawMetricRow("Name", ShortRenderPassName(texture.debugName).c_str());
			DrawMetricRow("Format", FormatName(texture.format));
			DrawMetricRow("Extent", std::format("{} x {}", texture.extent.width, texture.extent.height).c_str());
			DrawMetricRow("Mips", std::format("{}", texture.mipLevels).c_str());
			DrawMetricRow("Layers", std::format("{}", texture.arrayLayers).c_str());
			DrawMetricRow("Usage", ImageUsageText(texture.usage).c_str());
			DrawMetricRow("Aspect", ImageAspectText(texture.aspect).c_str());
			DrawMetricRow("Bindless", texture.hasBindlessSampled ? std::format("{}", texture.bindlessSampledSlot).c_str() : "-");
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		LoadSettings(context);
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, true);
	}

	void DebugLayer::OnDetach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		ReleaseSceneViewportTexture(context);
		ReleaseTextureInspectorTextures(context);
		context.Get<Input>().ClearMouseViewportTransform();
		context.Get<aether::RenderingSubsystem>().SetSceneViewportEnabled(context.services, false);
		SaveSettings(context);
		m_errorToasts.clear();
		m_selectedSceneEntity = {};
		m_dockspaceBuilt = false;
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
					AddDebugAabb(verts, boxCenter - glm::vec3(0.5f), boxCenter + glm::vec3(0.5f), colors::Red);
					AddDebugAxes(verts, glm::translate(glm::mat4(1.0f), boxCenter), 0.75f);
				}

				AddDebugAabb(verts, glm::vec3(-2.5f), glm::vec3(2.5f), colors::Yellow);
				AddDebugSphere(verts, glm::vec3(0.0f), 2.0f, colors::Info, 16);
				AddDebugLine(verts, glm::vec3(0.0f, -5.0f, 0.0f), glm::vec3(0.0f, 5.0f, 0.0f), colors::Neutral);
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
			const auto& scriptErr = colors::Error;
			ImGui::TextColored(ImVec4(scriptErr.r, scriptErr.g, scriptErr.b, scriptErr.a), "Script Error%s", m_errorToasts.size() > 1 ? "s" : "");
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
			context.Get<Input>().ClearMouseViewportTransform();
			return;
		}

		const gpu::Extent2D ext = context.Get<Swapchain>().GetExtent();

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

		// Root dockspace: invisible full-screen window for docking
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
		                             | ImGuiWindowFlags_NoBackground;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DebugDockSpace", nullptr, hostFlags);
		ImGui::PopStyleVar(3);

		ImGuiID dockspace_id = ImGui::GetID("AetherDebugDockSpaceV2");
		const bool hasSavedDockspace = ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

		if (!m_dockspaceBuilt && !hasSavedDockspace)
		{
			ImGui::DockBuilderRemoveNode(dockspace_id);
			ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

			ImGuiID remaining = dockspace_id;
			ImGuiID dock_left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.20f, nullptr, &remaining);
			ImGuiID dock_right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.28f, nullptr, &remaining);
			ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.30f, nullptr, &remaining);

			ImGui::DockBuilderDockWindow("Scene", dock_left);
			ImGui::DockBuilderDockWindow("Viewport", remaining);
			ImGui::DockBuilderDockWindow("Inspector", dock_bottom);
			ImGui::DockBuilderDockWindow("Performance", dock_bottom);
			ImGui::DockBuilderDockWindow("Camera", dock_bottom);
			ImGui::DockBuilderDockWindow("Textures", dock_bottom);
			ImGui::DockBuilderDockWindow("Render", dock_right);
			ImGui::DockBuilderDockWindow("Debug", dock_right);

			ImGui::DockBuilderFinish(dockspace_id);
		}
		m_dockspaceBuilt = true;

		ImGui::End();

		DrawSceneViewport(context);

		// Performance
		ImGui::Begin(std::format("Performance  |  {:.0f} FPS  |  {:.2f} ms###Performance", curFps, curMs).c_str());
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
		ImGui::End();

		// Render
		ImGui::Begin("Render");
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

			ImGui::SeparatorText("Viewport");
			SceneViewportSettings viewportSettings = rendering.GetSceneViewportSettings();
			int resolutionMode = static_cast<int>(viewportSettings.resolutionMode);
			const char* resolutionModes[] = {"Window native", "1280 x 720", "1920 x 1080", "2560 x 1440", "Custom"};
			bool viewportSettingsChanged = ImGui::Combo("Render resolution", &resolutionMode, resolutionModes, static_cast<int>(std::size(resolutionModes)));
			viewportSettings.resolutionMode = static_cast<SceneViewportResolutionMode>(resolutionMode);
			int customExtent[2] = {static_cast<int>(viewportSettings.customExtent.width), static_cast<int>(viewportSettings.customExtent.height)};
			ImGui::BeginDisabled(viewportSettings.resolutionMode != SceneViewportResolutionMode::Custom);
			if (ImGui::InputInt2("Custom size", customExtent))
			{
				viewportSettings.customExtent.width = static_cast<std::uint32_t>(std::clamp(customExtent[0], 64, 8192));
				viewportSettings.customExtent.height = static_cast<std::uint32_t>(std::clamp(customExtent[1], 64, 8192));
				viewportSettingsChanged = true;
			}
			ImGui::EndDisabled();
			if (viewportSettingsChanged)
			{
				ReleaseSceneViewportTexture(context);
				ReleaseTextureInspectorTextures(context);
				rendering.SetSceneViewportSettings(context.services, viewportSettings);
			}

			const char* displayModes[] = {"Fit", "Fill", "Actual", "Integer"};
			ImGui::Combo("Display mode", &m_viewportDisplayMode, displayModes, static_cast<int>(std::size(displayModes)));
			const char* aspectModes[] = {"Render", "Free", "16:9", "16:10", "4:3", "1:1"};
			ImGui::Combo("Aspect", &m_viewportAspectMode, aspectModes, static_cast<int>(std::size(aspectModes)));
			ImGui::Checkbox("Viewport stats", &m_viewportShowStats);
			ImGui::SameLine();
			ImGui::Checkbox("Mouse coords", &m_viewportShowMouse);

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
					DrawMetricRow("Transient hits", std::format("{}", stats.transientCacheHit).c_str(), {colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a});
					DrawMetricRow("Transient misses",
					        std::format("{}", stats.transientCacheMiss).c_str(),
					        stats.transientCacheMiss == 0 ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a});
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
		}
		ImGui::End();

		DrawTextureInspector(context);

		// Debug
		ImGui::Begin("Debug");
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
		}
		ImGui::End();

		// Camera
		ImGui::Begin("Camera");
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
		}
		ImGui::End();

		// Scene
		ImGui::Begin("Scene");
		{
			World& world = context.Get<World>();
			const auto entities = CollectSceneEntities(world);
			if (!IsAlive(world, m_selectedSceneEntity))
			{
				m_selectedSceneEntity = {};
			}

			ImGui::Text("%zu entities", entities.size());
			ImGui::BeginChild("SceneList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
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
		}
		ImGui::End();

		// Inspector
		ImGui::Begin("Inspector");
		{
			World& world = context.Get<World>();
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
		}
		ImGui::End();
	}
} // namespace aether::app
