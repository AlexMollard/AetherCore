#include "debug/RenderGraphPanel.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <string_view>
#include <type_traits>

#include <imgui.h>

#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"
#include "vulkan/RenderGraphStorage.hpp"

namespace aether::app
{
	namespace
	{
		std::string LowerCopy(std::string_view text)
		{
			std::string out(text);
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		ImVec4 PassBadgeColor(const RenderGraph::PassInfo& pass)
		{
			if (pass.isDebugDisabled)
			{
				return {colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a};
			}
			if (pass.isCulled)
			{
				return {colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a};
			}
			if (pass.isAsyncCompute)
			{
				return {0.42f, 0.70f, 0.95f, 1.0f};
			}
			if (pass.isCompute)
			{
				return {0.64f, 0.58f, 0.92f, 1.0f};
			}
			return {colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a};
		}

		bool PassMatchesFilter(const RenderGraph::PassInfo& pass, std::string_view filter)
		{
			if (filter.empty())
			{
				return true;
			}
			const std::string needle = LowerCopy(filter);
			if (LowerCopy(pass.name).find(needle) != std::string::npos)
			{
				return true;
			}
			if (LowerCopy(pass.declaredFile).find(needle) != std::string::npos)
			{
				return true;
			}
			return false;
		}

		const char* ResourceKindName(RenderGraph::PassInfo::ResourceAccessInfo::Kind kind) noexcept
		{
			switch (kind)
			{
				case RenderGraph::PassInfo::ResourceAccessInfo::Kind::Image:
					return "Image";
				case RenderGraph::PassInfo::ResourceAccessInfo::Kind::Buffer:
					return "Buffer";
			}
			return "Resource";
		}
	} // anonymous namespace

	void RenderGraphPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (auto rg = context.TryGet<aether::RenderGraph>())
		{
			DrawRenderGraphDebugger(context, *rg);
		}
	}

	void RenderGraphPanel::LoadSettings(TomlConfig& config, LayerContext& /*context*/)
	{
		m_renderGraphAutoSelectHotPass = config.GetBool("renderGraph.AutoSelectHotPass", false);
		m_renderGraphShowDisabled = config.GetBool("renderGraph.ShowDisabled", true);
		m_renderGraphShowCulled = config.GetBool("renderGraph.ShowCulled", true);
	}

	void RenderGraphPanel::SaveSettings(TomlConfig& config, LayerContext& /*context*/) const
	{
		config.Set("renderGraphAutoSelectHotPass", m_renderGraphAutoSelectHotPass);
		config.Set("renderGraphShowDisabled", m_renderGraphShowDisabled);
		config.Set("renderGraphShowCulled", m_renderGraphShowCulled);
	}

	void RenderGraphPanel::DrawRenderGraphDebugger(LayerContext& context, RenderGraph& graph)
	{
		auto passes = graph.GetPasses();
		const auto& frameStats = graph.GetFrameStats();
		const float imguiCpuMs = context.TryGet<ImguiSubsystem>() != nullptr ? context.Get<ImguiSubsystem>().GetLastRenderCpuTimeMs() : 0.0f;

		float graphCpuMs = 0.0f;
		float hottestMs = imguiCpuMs;
		std::string hottestName = "ImGui";
		std::size_t compiledCount = 0;
		std::size_t culledCount = 0;
		std::size_t disabledCount = 0;
		for (const auto& pass: passes)
		{
			graphCpuMs += pass.lastCpuTimeMs;
			compiledCount += pass.isCompiled ? 1u : 0u;
			culledCount += pass.isCulled ? 1u : 0u;
			disabledCount += pass.isDebugDisabled ? 1u : 0u;
			if (pass.lastCpuTimeMs > hottestMs)
			{
				hottestMs = pass.lastCpuTimeMs;
				hottestName = ShortRenderPassName(pass.name);
			}

			auto& history = m_renderPassBenchmarks[pass.name];
			history.samples[history.head] = pass.lastCpuTimeMs;
			history.head = (history.head + 1) % history.samples.size();
			history.count = std::min(history.count + 1, history.samples.size());
		}

		constexpr std::string_view kImguiPassName = "ImGui";
		auto& imguiHistory = m_renderPassBenchmarks[std::string(kImguiPassName)];
		imguiHistory.samples[imguiHistory.head] = imguiCpuMs;
		imguiHistory.head = (imguiHistory.head + 1) % imguiHistory.samples.size();
		imguiHistory.count = std::min(imguiHistory.count + 1, imguiHistory.samples.size());

		if (m_selectedRenderPass.empty() || m_renderGraphAutoSelectHotPass)
		{
			const auto hottestGraphPass = std::ranges::max_element(passes, {}, &RenderGraph::PassInfo::lastCpuTimeMs);
			m_selectedRenderPass = (hottestGraphPass != passes.end() && hottestGraphPass->lastCpuTimeMs >= imguiCpuMs) ? hottestGraphPass->name : std::string(kImguiPassName);
		}

		if (ImGui::BeginTable("RenderGraphSummary", 2, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			DrawMetricRow("Registered", std::format("{}", passes.size()).c_str());
			DrawMetricRow("Compiled", std::format("{}", compiledCount).c_str());
			DrawMetricRow("Disabled",
			        std::format("{}", disabledCount).c_str(),
			        disabledCount == 0 ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a});
			DrawMetricRow("Culled",
			        std::format("{}", culledCount).c_str(),
			        culledCount == 0 ? ImVec4{colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a});
			DrawMetricRow("Graph CPU", std::format("{:.3f} ms", graphCpuMs).c_str(), MsColor(graphCpuMs));
			DrawMetricRow("ImGui CPU", std::format("{:.3f} ms", imguiCpuMs).c_str(), MsColor(imguiCpuMs));
			DrawMetricRow("Hottest", std::format("{} ({:.3f} ms)", hottestName, hottestMs).c_str(), MsColor(hottestMs));
			DrawMetricRow("Barriers", std::format("{}", frameStats.barrierCount).c_str());
			DrawMetricRow("Transient heap", std::format("{:.1f}/{:.1f} MB", static_cast<double>(frameStats.heapUsed) / (1024.0 * 1024.0), static_cast<double>(frameStats.heapCapacity) / (1024.0 * 1024.0)).c_str());
			DrawMetricRow("Aliased", std::format("{} images, {} buffers", frameStats.aliasedImageCount, frameStats.aliasedBufferCount).c_str());
			DrawMetricRow("Cache",
			        std::format("{} hit, {} miss, {} kept", frameStats.transientCacheHit, frameStats.transientCacheMiss, frameStats.cacheSize).c_str(),
			        frameStats.transientCacheMiss == 0 ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a});
			ImGui::EndTable();
		}

		ImGui::InputTextWithHint("##RenderGraphFilter", "Filter passes or source files", m_renderGraphFilter, sizeof(m_renderGraphFilter));
		ImGui::SameLine();
		if (ImGui::Button("Reset samples"))
		{
			m_renderPassBenchmarks.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Enable all"))
		{
			graph.ClearDebugDisabledPasses();
		}
		ImGui::Checkbox("Show disabled", &m_renderGraphShowDisabled);
		ImGui::SameLine();
		ImGui::Checkbox("Show culled", &m_renderGraphShowCulled);
		ImGui::SameLine();
		ImGui::Checkbox("Track hottest", &m_renderGraphAutoSelectHotPass);

		const std::string_view filter(m_renderGraphFilter);
		if (ImGui::BeginTable("RenderGraphPassTable", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 300.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 34.0f);
			ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 78.0f);
			ImGui::TableSetupColumn("Pass");
			ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, 62.0f);
			ImGui::TableSetupColumn("Resources", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableHeadersRow();

			for (const auto& pass: passes)
			{
				if ((!m_renderGraphShowDisabled && pass.isDebugDisabled) || (!m_renderGraphShowCulled && pass.isCulled) || !PassMatchesFilter(pass, filter))
				{
					continue;
				}

				auto& history = m_renderPassBenchmarks[pass.name];
				const BenchmarkStats stats = ComputeBenchmarkStats(history);
				const bool selected = m_selectedRenderPass == pass.name;
				ImGui::PushID(pass.name.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				bool enabled = !pass.isDebugDisabled;
				if (ImGui::Checkbox("##enabled", &enabled))
				{
					graph.SetPassDebugDisabled(pass.name, !enabled);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(PassBadgeColor(pass), "%s", pass.isAsyncCompute ? "Async" : pass.isCompute ? "Compute" : "Graphics");
				ImGui::TableSetColumnIndex(2);
				if (ImGui::Selectable(ShortRenderPassName(pass.name).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_selectedRenderPass = pass.name;
					m_renderGraphAutoSelectHotPass = false;
				}
				if (pass.isCulled)
				{
					ImGui::SameLine();
					ImGui::TextDisabled("culled");
				}
				ImGui::TableSetColumnIndex(3);
				ImGui::TextColored(MsColor(pass.lastCpuTimeMs), "%.3f", pass.lastCpuTimeMs);
				ImGui::TableSetColumnIndex(4);
				ImGui::TextColored(MsColor(stats.avg), "%.3f", stats.avg);
				ImGui::TableSetColumnIndex(5);
				ImGui::TextColored(MsColor(stats.max), "%.3f", stats.max);
				ImGui::TableSetColumnIndex(6);
				ImGui::Text("%u", pass.preBarrierCount + pass.bufferBarrierCount + pass.signalBarrierCount);
				ImGui::TableSetColumnIndex(7);
				ImGui::Text("%zu", pass.resources.size());
				ImGui::PopID();
			}

			RenderGraph::PassInfo imguiPass;
			imguiPass.name = std::string(kImguiPassName);
			imguiPass.declaredFile = "ImguiSubsystem.cpp";
			if (PassMatchesFilter(imguiPass, filter))
			{
				const BenchmarkStats stats = ComputeBenchmarkStats(imguiHistory);
				const bool selected = m_selectedRenderPass == kImguiPassName;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("-");
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(ImVec4{0.95f, 0.45f, 0.15f, 1.0f}, "External");
				ImGui::TableSetColumnIndex(2);
				if (ImGui::Selectable("ImGui", selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_selectedRenderPass = std::string(kImguiPassName);
					m_renderGraphAutoSelectHotPass = false;
				}
				ImGui::SameLine();
				ImGui::TextDisabled("after graph");
				ImGui::TableSetColumnIndex(3);
				ImGui::TextColored(MsColor(imguiCpuMs), "%.3f", imguiCpuMs);
				ImGui::TableSetColumnIndex(4);
				ImGui::TextColored(MsColor(stats.avg), "%.3f", stats.avg);
				ImGui::TableSetColumnIndex(5);
				ImGui::TextColored(MsColor(stats.max), "%.3f", stats.max);
				ImGui::TableSetColumnIndex(6);
				ImGui::TextDisabled("-");
				ImGui::TableSetColumnIndex(7);
				ImGui::TextDisabled("-");
			}
			ImGui::EndTable();
		}

		ImGui::SeparatorText("Pass Inspector");
		if (m_selectedRenderPass.empty())
		{
			ImGui::TextDisabled("Select a pass");
			return;
		}

		auto historyIt = m_renderPassBenchmarks.find(m_selectedRenderPass);
		if (historyIt != m_renderPassBenchmarks.end())
		{
			std::array<float, kRenderBenchmarkSampleCount> ordered{};
			const auto& history = historyIt->second;
			for (std::size_t i = 0; i < history.count; ++i)
			{
				const std::size_t idx = (history.head + history.samples.size() - history.count + i) % history.samples.size();
				ordered[i] = history.samples[idx];
			}
			ImGui::PlotLines("CPU history", ordered.data(), static_cast<int>(history.count), 0, "ms", 0.0f, std::max(2.0f, ComputeBenchmarkStats(history).max * 1.25f), ImVec2(-1.0f, 110.0f));
		}

		if (m_selectedRenderPass == kImguiPassName)
		{
			ImGui::TextUnformatted("ImGui is rendered after RenderGraph::Execute on the swapchain color target.");
			if (ImGui::BeginTable("ImguiPassDetails", 2, ImGuiTableFlags_SizingStretchProp))
			{
				DrawMetricRow("Queue", "Graphics");
				DrawMetricRow("Attachment", "Swapchain color");
				DrawMetricRow("Load", "Load");
				DrawMetricRow("Store", "Store");
				DrawMetricRow("Source", "src/engine/imgui/ImguiSubsystem.cpp");
				ImGui::EndTable();
			}
			return;
		}

		const auto selectedPass = std::ranges::find_if(passes, [&](const RenderGraph::PassInfo& pass) { return pass.name == m_selectedRenderPass; });
		if (selectedPass == passes.end())
		{
			ImGui::TextDisabled("Selected pass no longer exists");
			return;
		}

		const RenderGraph::PassInfo& pass = *selectedPass;
		if (ImGui::BeginTable("SelectedRenderPassDetails", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawMetricRow("Name", pass.name.c_str());
			DrawMetricRow("Queue", pass.isAsyncCompute ? "Async compute" : "Graphics");
			DrawMetricRow("Kind", pass.isCompute ? "Compute" : "Graphics");
			DrawMetricRow("Compiled", pass.isCompiled ? "Yes" : "No");
			DrawMetricRow("Culled",
			        pass.isCulled ? "Yes" : "No",
			        pass.isCulled ? ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a} : ImVec4{colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a});
			DrawMetricRow(
			        "Disabled", pass.isDebugDisabled ? "Yes" : "No", pass.isDebugDisabled ? ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a} : ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a});
			DrawMetricRow("Barriers", std::format("{} image, {} buffer, {} signal, {} wait groups", pass.preBarrierCount, pass.bufferBarrierCount, pass.signalBarrierCount, pass.waitCount).c_str());
			DrawMetricRow("Writes", std::format("{} color, depth {}", pass.colorWriteCount, pass.hasDepthWrite ? "yes" : "no").c_str());
			DrawMetricRow("Reads/accesses", std::format("{} images, {} buffers", pass.imageAccessCount, pass.bufferAccessCount).c_str());
			DrawMetricRow("Extent", pass.extentOverride.has_value() ? std::format("{} x {}", pass.extentOverride->width, pass.extentOverride->height).c_str() : "Frame target");
			if (!pass.declaredFile.empty())
			{
				DrawMetricRow("Declared", std::format("{}:{}", pass.declaredFile, pass.declaredLine).c_str());
			}
			ImGui::EndTable();
		}

		if (ImGui::BeginTable("SelectedRenderPassResources", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("ID");
			ImGui::TableSetupColumn("Access");
			ImGui::TableSetupColumn("Mode");
			ImGui::TableHeadersRow();
			for (const auto& resource: pass.resources)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ResourceKindName(resource.kind));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u", resource.id);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(resource.usage.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextColored(
				        resource.writes ? ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a} : ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a}, "%s", resource.writes ? "Write" : "Read");
			}
			ImGui::EndTable();
		}
	}
} // namespace aether::app
