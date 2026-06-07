#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <regex>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiTheme.hpp"
#include "ui/UiWidgets.hpp"
#include "utils/Logger.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "physics/PhysicsDebugRenderer.hpp"

namespace aether::app
{
	namespace
	{
		constexpr float kPanelW = 408.f;
		constexpr float kPanelH = 600.f;

		UiRect HeightRect(float h)
		{
			UiRect r{};
			r.offsetMaxPx.y = h;
			return r;
		}

		glm::vec4 FpsColor(float fps) noexcept
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

		glm::vec4 MsColor(float ms) noexcept
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

		UiRect PxRect(float l, float t, float r, float b)
		{
			return UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {0.f, 0.f}, .offsetMinPx = {l, t}, .offsetMaxPx = {r, b}};
		}
	} // namespace

	// ── Stat helpers ──────────────────────────────────────────────────────────

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

	// ── Script error location ─────────────────────────────────────────────────

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
		auto* scripting = context.TryGet<scripting::ScriptingSubsystem>();
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
					std::smatch match = *it;
					if (it == begin)
					{
						lastPos = match.position();
					}
					else
					{
						std::string prevError = err.substr(lastPos, match.position() - lastPos);
						individualErrors.push_back(prevError);
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

	// ── AppLayer overrides ────────────────────────────────────────────────────

	void DebugLayer::OnAttach(LayerContext& context)
	{
		auto& world = context.Get<World>();

		auto reg = [this](Entity e) -> Entity
		{
			m_entities.push_back(e);
			return e;
		};

		m_debugPanel = reg(ui::SpawnPanel(world,
		        UiAnchors::TopRight({12.f, 12.f}, {kPanelW, kPanelH}),
		        "Debug",
		        /*draggable=*/true,
		        /*collapsible=*/true,
		        /*zOrder=*/1.f));

		world.Emplace<ui::UiLayoutComponent>(m_debugPanel,
		        ui::UiLayoutComponent{
		                .direction = ui::UiLayoutComponent::Direction::Vertical,
		                .spacing = 0.f,
		                .padding = 10.f,
		                .autoSize = true,
		        });
		world.Emplace<ui::UiChildrenComponent>(m_debugPanel);

		auto addChild = [&](Entity child)
		{
			ui::AddChild(world, m_debugPanel, child);
		};

		m_headerSpacer = reg(world.Create());
		world.Emplace<ui::UiTransformComponent>(m_headerSpacer, ui::UiTransformComponent{.rect = HeightRect(48.f), .zOrder = 2.f});
		addChild(m_headerSpacer);

		std::vector<std::string> tabNames = {"Perf", "Render", "Camera"};

		auto createPage = [&]([[maybe_unused]] Tab tab) -> Entity
		{
			auto page = reg(ui::SpawnTabPage(world, 2.f));
			addChild(page);
			return page;
		};

		m_tabPages[Tab_Performance] = createPage(Tab_Performance);
		m_tabPages[Tab_Render] = createPage(Tab_Render);
		m_tabPages[Tab_Camera] = createPage(Tab_Camera);

		std::vector<Entity> tabPageVec = {m_tabPages[Tab_Performance], m_tabPages[Tab_Render], m_tabPages[Tab_Camera]};
		m_tabBar = reg(ui::SpawnTabBar(world, UiRect{.anchorMin = {0.f, 0.f}, .anchorMax = {1.f, 0.f}, .offsetMinPx = {0, 0}, .offsetMaxPx = {0, ui::UiTheme::Default().tabHeight}}, tabNames, tabPageVec, 3.f));

		// Insert tab bar after header spacer, before pages.
		// addChild appends; remove and re-add pages so tab bar is ordered correctly.
		{
			auto* panelChildren = world.TryGet<ui::UiChildrenComponent>(m_debugPanel);
			panelChildren->children.erase(panelChildren->children.begin() + 1, panelChildren->children.begin() + 1 + kTabCount);
			addChild(m_tabBar);
			for (std::size_t i = 0; i < kTabCount; ++i)
			{
				addChild(m_tabPages[i]);
			}
		}

		auto addToPage = [&](Tab tab, Entity child)
		{
			ui::AddChild(world, m_tabPages[tab], child);
		};

		for (std::size_t i = 0; i < 6; ++i)
		{
			const char* perfLabels[] = {"Frame", "FPS", "Delta", "Avg FPS", "Min", "Max"};
			m_labelRows[Row_Frame + i] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), perfLabels[i], 2.f));
			addToPage(Tab_Performance, m_labelRows[Row_Frame + i]);
		}
		m_graphEntity = reg(ui::SpawnGraph(world, HeightRect(108.f), "Frame Time (0 - 33 ms)  |  ref: 60fps  30fps", 0.f, 33.333f, 2.f));
		addToPage(Tab_Performance, m_graphEntity);

		m_separators[0] = reg(ui::SpawnSection(world, HeightRect(15.f), 2.f));
		addToPage(Tab_Render, m_separators[0]);

		const char* renderLabels[] = {"Tonemap", "FXAA", "Resolution"};
		for (std::size_t i = 0; i < 3; ++i)
		{
			m_labelRows[Row_Tonemap + i] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), renderLabels[i], 2.f));
			addToPage(Tab_Render, m_labelRows[Row_Tonemap + i]);
		}

		m_separators[1] = reg(ui::SpawnSection(world, HeightRect(15.f), 2.f));
		addToPage(Tab_Render, m_separators[1]);

		m_labelRows[Row_RenderPasses] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), "Render Passes", 2.f));
		addToPage(Tab_Render, m_labelRows[Row_RenderPasses]);

		m_labelRows[Row_PhysicsDebug] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), "Physics Debug", 2.f));
		addToPage(Tab_Render, m_labelRows[Row_PhysicsDebug]);

		m_separators[2] = reg(ui::SpawnSection(world, HeightRect(15.f), 2.f));
		addToPage(Tab_Camera, m_separators[2]);

		const char* camLabels[] = {"Position", "FOV", "Near", "Far"};
		for (std::size_t i = 0; i < 4; ++i)
		{
			m_labelRows[Row_Pos + i] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), camLabels[i], 2.f));
			addToPage(Tab_Camera, m_labelRows[Row_Pos + i]);
		}

		m_separators[3] = reg(ui::SpawnSection(world, HeightRect(15.f), 2.f));
		addToPage(Tab_Camera, m_separators[3]);

		const char* lightLabels[] = {"Point Lights", "Spot Lights", "Sun Intensity"};
		for (std::size_t i = 0; i < 3; ++i)
		{
			m_labelRows[Row_PointLights + i] = reg(ui::SpawnLabelRow(world, HeightRect(20.f), lightLabels[i], 2.f));
			addToPage(Tab_Camera, m_labelRows[Row_PointLights + i]);
		}

		m_reloadButton = reg(ui::SpawnButton(world, HeightRect(28.f), "Reload Script  [F5]", 2.f));
		addChild(m_reloadButton);
	}

	void DebugLayer::OnDetach(LayerContext& context)
	{
		auto& world = context.Get<World>();
		for (const Entity e: m_entities)
		{
			world.Destroy(e);
		}
		m_entities.clear();
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		auto& world = context.Get<World>();
		const Input& input = context.Get<Input>();

		// ── Toggle visibility ───────────────────────────────────────────────
		if (input.IsKeyPressed(aether::Key::F1))
		{
			m_visible = !m_visible;
		}

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto* scripting = context.TryGet<scripting::ScriptingSubsystem>())
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
			const char* names[] = {"Reinhard", "ACES Filmic", "Uncharted2"};
			AE_INFO(aether::LogCategory::App, "Tonemap: {}", names[static_cast<int>(next)]);
		}

		if (input.IsKeyPressed(aether::Key::F6))
		{
			const bool newState = !aether::IsPhysicsDebugRenderingEnabled();
			aether::SetPhysicsDebugRenderingEnabled(newState);
			AE_INFO(aether::LogCategory::App, "Physics debug: {}", newState ? "on" : "off");
		}

		PollScriptErrors(context);

		// ── Update data ─────────────────────────────────────────────────────
		const float frameMs = static_cast<float>(context.deltaTimeSeconds * 1000.0);

		// Push frame time to graph
		if (m_graphEntity.IsValid())
		{
			if (auto* graph = world.TryGet<ui::UiGraphComponent>(m_graphEntity))
			{
				graph->samples[graph->head] = frameMs;
				graph->head = (graph->head + 1) % ui::UiGraphComponent::kMaxSamples;
				graph->count = std::min(graph->count + 1, ui::UiGraphComponent::kMaxSamples);

				float avg = 0.f;
				for (std::size_t i = 0; i < graph->count; ++i)
				{
					const std::size_t idx = (graph->head + ui::UiGraphComponent::kMaxSamples - graph->count + i) % ui::UiGraphComponent::kMaxSamples;
					avg += graph->samples[idx];
				}
				avg /= static_cast<float>(std::max(graph->count, std::size_t{1}));
				graph->label = std::format("Frame Time (0 - 33 ms)  |  avg: {:.2f} ms  |  ref: 60fps  30fps", avg);
			}
		}

		// Compute frame-time statistics from the graph's data.
		float curMs = frameMs;
		float avgMs = curMs;
		float minMs = curMs;
		float maxMs = curMs;
		if (m_graphEntity.IsValid())
		{
			if (auto* graph = world.TryGet<ui::UiGraphComponent>(m_graphEntity))
			{
				curMs = graph->count > 0 ? graph->samples[(graph->head + ui::UiGraphComponent::kMaxSamples - 1) % ui::UiGraphComponent::kMaxSamples] : frameMs;
				if (graph->count > 0)
				{
					float total = 0.f;
					minMs = graph->samples[0];
					maxMs = graph->samples[0];
					for (std::size_t i = 0; i < graph->count; ++i)
					{
						const float v = graph->samples[i];
						total += v;
						minMs = std::min(minMs, v);
						maxMs = std::max(maxMs, v);
					}
					avgMs = total / static_cast<float>(graph->count);
				}
			}
		}
		const float curFps = curMs > 0.f ? 1000.f / curMs : 0.f;
		const float avgFps = avgMs > 0.f ? 1000.f / avgMs : 0.f;

		std::array<char, 128> buf{};

		// Update label rows
		auto setRow = [&](LabelRow idx, const char* val, glm::vec4 color)
		{
			if (auto* row = world.TryGet<ui::UiLabelRowComponent>(m_labelRows[idx]))
			{
				row->value = val;
				row->valueColor = color;
			}
		};

		std::snprintf(buf.data(), buf.size(), "#%llu", static_cast<unsigned long long>(context.frameIndex));
		setRow(Row_Frame, buf.data(), ui::UiTheme::Default().text);

		std::snprintf(buf.data(), buf.size(), "%.1f", curFps);
		setRow(Row_Fps, buf.data(), FpsColor(curFps));

		std::snprintf(buf.data(), buf.size(), "%.2f ms", curMs);
		setRow(Row_Delta, buf.data(), MsColor(curMs));

		std::snprintf(buf.data(), buf.size(), "%.1f", avgFps);
		setRow(Row_AvgFps, buf.data(), FpsColor(avgFps));

		std::snprintf(buf.data(), buf.size(), "%.2f ms", minMs);
		setRow(Row_Min, buf.data(), ui::UiTheme::Default().good);

		std::snprintf(buf.data(), buf.size(), "%.2f ms", maxMs);
		setRow(Row_Max, buf.data(), MsColor(maxMs));

		const Renderer& renderer = context.Get<Renderer>();
		setRow(Row_Tonemap, GetTonemapModeName(renderer.GetTonemapMode()), ui::UiTheme::Default().text);

		const bool fxaa = renderer.IsFxaaEnabled();
		setRow(Row_Fxaa, fxaa ? "On" : "Off", fxaa ? ui::UiTheme::Default().good : (ui::UiTheme::Default().textLabel * glm::vec4{1.f, 1.f, 1.f, 0.5f}));

		const VkExtent2D ext = context.Get<Swapchain>().GetExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		setRow(Row_Resolution, buf.data(), ui::UiTheme::Default().text);

		const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera();
		if (cam)
		{
			const glm::vec3 pos = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
			setRow(Row_Pos, buf.data(), ui::UiTheme::Default().text);

			std::snprintf(buf.data(), buf.size(), "%.0f deg", cam->GetFovDegrees());
			setRow(Row_Fov, buf.data(), ui::UiTheme::Default().text);

			std::snprintf(buf.data(), buf.size(), "%.2f", cam->GetNearPlane());
			setRow(Row_Near, buf.data(), ui::UiTheme::Default().text);

			std::snprintf(buf.data(), buf.size(), "%.0f", cam->GetFarPlane());
			setRow(Row_Far, buf.data(), ui::UiTheme::Default().text);
		}
		else
		{
			setRow(Row_Pos, "No active camera", ui::UiTheme::Default().textLabel);
			setRow(Row_Fov, "", {});
			setRow(Row_Near, "", {});
			setRow(Row_Far, "", {});
		}

		std::snprintf(buf.data(), buf.size(), "%zu", renderer.GetPointLights().size());
		setRow(Row_PointLights, buf.data(), ui::UiTheme::Default().text);

		std::snprintf(buf.data(), buf.size(), "%zu", renderer.GetSpotLights().size());
		setRow(Row_SpotLights, buf.data(), ui::UiTheme::Default().text);

		std::snprintf(buf.data(), buf.size(), "%.2f", renderer.GetDirectionalLightIntensity());
		setRow(Row_SunIntensity, buf.data(), ui::UiTheme::Default().text);

		// Render passes count
		if (auto* rg = aether::GetCurrentRenderGraph())
		{
			auto passes = rg->GetPasses();
			std::snprintf(buf.data(), buf.size(), "%zu passes", passes.size());
			setRow(Row_RenderPasses, buf.data(), ui::UiTheme::Default().text);
		}
		else
		{
			setRow(Row_RenderPasses, "N/A", ui::UiTheme::Default().textLabel);
		}

		// Physics debug state
		const bool physDebug = aether::IsPhysicsDebugRenderingEnabled();
		setRow(Row_PhysicsDebug, physDebug ? "On" : "Off", physDebug ? ui::UiTheme::Default().good : ui::UiTheme::Default().textLabel);

		// Reload button click detection
		if (const auto* inp = world.TryGet<ui::UiInputComponent>(m_reloadButton))
		{
			if (inp->clicked)
			{
				if (auto* scripting = context.TryGet<scripting::ScriptingSubsystem>())
				{
					scripting->RequestReload();
				}
			}
		}

		// Tab switching via keyboard shortcuts (Num1/2/3).
		if (auto* tabComp = world.TryGet<ui::UiTabComponent>(m_tabBar))
		{
			if (input.IsKeyPressed(aether::Key::Num1))
			{
				tabComp->selectedTab = Tab_Performance;
			}
			else if (input.IsKeyPressed(aether::Key::Num2))
			{
				tabComp->selectedTab = Tab_Render;
			}
			else if (input.IsKeyPressed(aether::Key::Num3))
			{
				tabComp->selectedTab = Tab_Camera;
			}
		}

		// Update panel title
		if (m_debugPanel.IsValid())
		{
			if (auto* panel = world.TryGet<ui::UiPanelComponent>(m_debugPanel))
			{
				panel->title = std::format("Debug  |  {:.0f} FPS  |  {:.2f} ms", curFps, curMs);
			}
		}
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		UIRenderer& ui = context.Get<UIRenderer>();
		const Input& input = context.Get<Input>();
		const VkExtent2D extent = context.Get<Swapchain>().GetExtent();
		const float sw = static_cast<float>(extent.width);
		const float sh = static_cast<float>(extent.height);
		const ui::UiTheme& theme = ui::UiTheme::Default();

		// ── Error notification bar (always visible) ──────────────────────────
		if (!m_errorToasts.empty())
		{
			constexpr float kBarHeight = 44.f;
			constexpr float kDismissW = 100.f;
			constexpr float kMargin = 16.f;
			const float barY = sh - kBarHeight - kMargin;

			ui.DrawRect(PxRect(0.f, barY, sw, barY + kBarHeight), {0.14f, 0.04f, 0.04f, 0.95f}, 4.f);

			const std::size_t count = m_errorToasts.size();
			const std::string& summary = m_errorToasts[0].summary;
			std::array<char, 256> errText{};
			if (count == 1)
			{
				std::snprintf(errText.data(), errText.size(), "Script Error: %s", summary.c_str());
			}
			else
			{
				std::snprintf(errText.data(), errText.size(), "Script Errors (%zu): %s", count, summary.c_str());
			}

			const float errTextY = barY + (kBarHeight - theme.bodyFontSize) * 0.5f + theme.bodyFontSize * 0.35f;
			ui.DrawText(errText.data(), UiPoint{.anchor = {0.f, 0.f}, .offsetPx = {kMargin, errTextY}}, theme.bodyFontSize, theme.bad);

			// Dismiss button
			const float btnX = sw - kDismissW - kMargin;
			const float btnY = barY + (kBarHeight - 24.f) * 0.5f;
			const float btnW = kDismissW - 10.f;
			const float btnH = 24.f;

			const bool btnHovered = input.GetMousePos().x >= btnX && input.GetMousePos().x <= btnX + btnW && input.GetMousePos().y >= btnY && input.GetMousePos().y <= btnY + btnH;

			const glm::vec4 btnColor = btnHovered ? glm::vec4{0.45f, 0.15f, 0.15f, 1.f} : glm::vec4{0.35f, 0.10f, 0.10f, 1.f};
			ui.DrawRect(PxRect(btnX, btnY, btnX + btnW, btnY + btnH), btnColor, 3.f);

			const float btnTextW = ui.MeasureText("Dismiss All", 13.f);
			const float btnTextX = btnX + (btnW - btnTextW) * 0.5f;
			const float btnTextY = btnY + btnH * 0.5f + 13.f * 0.35f;
			ui.DrawText("Dismiss All", UiPoint{.anchor = {0.f, 0.f}, .offsetPx = {btnTextX, btnTextY}}, 13.f, {0.85f, 0.55f, 0.55f, 1.f});

			if (input.IsMouseButtonPressed(MouseButton::Left) && btnHovered)
			{
				m_errorToasts.clear();
			}
		}

		// ── Panel visibility toggle ──────────────────────────────────────────
		// Move the panel on/off-screen so the auto-renderer skips it.
		if (m_debugPanel.IsValid())
		{
			if (auto* pt = context.Get<World>().TryGet<ui::UiTransformComponent>(m_debugPanel))
			{
				if (m_visible && pt->rect.offsetMinPx.y < -1000.f)
				{
					pt->rect = m_savedPanelRect;
				}
				else if (!m_visible && pt->rect.offsetMinPx.y >= -1000.f)
				{
					m_savedPanelRect = pt->rect;
					pt->rect.offsetMinPx.y = -9999.f;
					pt->rect.offsetMaxPx.y = -9999.f;
				}
			}
		}
	}
} // namespace aether::app
