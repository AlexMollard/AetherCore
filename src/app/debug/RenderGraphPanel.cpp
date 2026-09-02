#include "debug/RenderGraphPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

#include <imgui.h>
#include <implot.h>

#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"
#include "vulkan/RenderGraphStorage.hpp"

namespace aether::editor
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

		bool PassMatchesFilter(const RenderGraph::PassInfo& pass, std::string_view lowerFilter)
		{
			if (lowerFilter.empty())
			{
				return true;
			}
			if (LowerCopy(pass.name).contains(lowerFilter))
			{
				return true;
			}
			if (LowerCopy(pass.declaredFile).contains(lowerFilter))
			{
				return true;
			}
			if (LowerCopy(pass.sideEffectReason).contains(lowerFilter))
			{
				return true;
			}
			for (const std::string& dependency: pass.logicalDependencies)
			{
				if (LowerCopy(dependency).contains(lowerFilter))
				{
					return true;
				}
			}
			for (const std::string& drawList: pass.producedDrawLists)
			{
				if (LowerCopy(drawList).contains(lowerFilter))
				{
					return true;
				}
			}
			for (const std::string& drawList: pass.consumedDrawLists)
			{
				if (LowerCopy(drawList).contains(lowerFilter))
				{
					return true;
				}
			}
			for (const std::string& product: pass.producedFrameProducts)
			{
				if (LowerCopy(product).contains(lowerFilter))
				{
					return true;
				}
			}
			for (const std::string& product: pass.consumedFrameProducts)
			{
				if (LowerCopy(product).contains(lowerFilter))
				{
					return true;
				}
			}
			for (const std::string& warning: pass.contractWarnings)
			{
				if (LowerCopy(warning).contains(lowerFilter))
				{
					return true;
				}
			}
			return false;
		}

		std::string JoinStrings(const std::vector<std::string>& values)
		{
			std::string joined;
			for (const std::string& value: values)
			{
				if (!joined.empty())
				{
					joined += ", ";
				}
				joined += value;
			}
			return joined;
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

		const char* ProductSourceName(FrameBlackboard::ProductSource source) noexcept
		{
			switch (source)
			{
				case FrameBlackboard::ProductSource::Imported:
					return "Imported";
				case FrameBlackboard::ProductSource::FrameSetup:
					return "FrameSetup";
				case FrameBlackboard::ProductSource::GraphPass:
					return "GraphPass";
			}
			return "Unknown";
		}

		template<std::size_t N, typename... Args>
		void FormatToBuffer(std::array<char, N>& buffer, std::format_string<Args...> fmt, Args&&... args)
		{
			const auto result = std::format_to_n(buffer.begin(), buffer.size() - 1, fmt, std::forward<Args>(args)...);
			*result.out = '\0';
		}

		template<typename... Args>
		void DrawMetricRowFormat(const char* label, std::format_string<Args...> fmt, Args&&... args)
		{
			std::array<char, 256> buffer{};
			FormatToBuffer(buffer, fmt, std::forward<Args>(args)...);
			DrawMetricRow(label, buffer.data());
		}

		template<typename... Args>
		void DrawMetricRowFormat(const char* label, ImVec4 color, std::format_string<Args...> fmt, Args&&... args)
		{
			std::array<char, 256> buffer{};
			FormatToBuffer(buffer, fmt, std::forward<Args>(args)...);
			DrawMetricRow(label, buffer.data(), color);
		}
	} // namespace

	void RenderGraphPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (!ImGui::Begin("Render Graph", VisiblePtr()))
		{
			ImGui::End();
			return;
		}

		chrome::PanelHeader("RENDER GRAPH");

		if (auto* rg = context.TryGet<aether::RenderGraph>())
		{
			DrawRenderGraphDebugger(context, *rg);
		}

		ImGui::End();
	}

	void RenderGraphPanel::LoadSettings(TomlConfig& config, app::LayerContext& /*context*/)
	{
		m_renderGraphAutoSelectHotPass = config.GetBool("debug.rendergraphautoselecthotpass", false);
		m_renderGraphShowDisabled = config.GetBool("debug.rendergraphshowdisabled", true);
		m_renderGraphShowCulled = config.GetBool("debug.rendergraphshowculled", true);
	}

	void RenderGraphPanel::SaveSettings(TomlConfig& config, app::LayerContext& /*context*/) const
	{
		config.Set("debug.rendergraphautoselecthotpass", m_renderGraphAutoSelectHotPass);
		config.Set("debug.rendergraphshowdisabled", m_renderGraphShowDisabled);
		config.Set("debug.rendergraphshowculled", m_renderGraphShowCulled);
	}

	void RenderGraphPanel::DrawRenderGraphDebugger(app::LayerContext& context, RenderGraph& graph)
	{
		AE_PROFILE_ZONE();

		auto passes = graph.GetPasses();
		const auto products = graph.GetBlackboard().GetProducts();
		const auto& frameStats = graph.GetFrameStats();
		const auto& frame = graph.GetLastFrameContext();
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
			DrawMetricRowFormat("Registered", "{}", passes.size());
			DrawMetricRowFormat("Compiled", "{}", compiledCount);
			DrawMetricRowFormat("Products", "{}", products.size());
			if (frame.swapchainImageIndex == UINT32_MAX)
			{
				DrawMetricRowFormat("Frame", "{} / slot {} / image -", frame.frameIndex, frame.frameSlot);
			}
			else
			{
				DrawMetricRowFormat("Frame", "{} / slot {} / image {}", frame.frameIndex, frame.frameSlot, frame.swapchainImageIndex);
			}
			DrawMetricRowFormat("Extent", "{} x {}", frame.extent.width, frame.extent.height);
			DrawMetricRowFormat("Disabled", disabledCount == 0 ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "{}", disabledCount);
			DrawMetricRowFormat(
			        "Culled", culledCount == 0 ? ImVec4{colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "{}", culledCount);
			DrawMetricRowFormat("Graph CPU", MsColor(graphCpuMs), "{:.3f} ms", graphCpuMs);
			DrawMetricRowFormat("ImGui CPU", MsColor(imguiCpuMs), "{:.3f} ms", imguiCpuMs);
			DrawMetricRowFormat("Hottest", MsColor(hottestMs), "{} ({:.3f} ms)", hottestName, hottestMs);
			DrawMetricRowFormat("Barriers", "{}", frameStats.barrierCount);
			DrawMetricRowFormat("Transient heap", "{:.1f}/{:.1f} MB", static_cast<double>(frameStats.heapUsed) / (1024.0 * 1024.0), static_cast<double>(frameStats.heapCapacity) / (1024.0 * 1024.0));
			DrawMetricRowFormat("Pooled", "{} images, {} buffers", frameStats.aliasedImageCount, frameStats.aliasedBufferCount);
			DrawMetricRowFormat("Transient VRAM",
			        "{:.1f} MB for {:.1f} MB of resources",
			        static_cast<double>(frameStats.transientPhysicalBytes) / (1024.0 * 1024.0),
			        static_cast<double>(frameStats.transientLogicalBytes) / (1024.0 * 1024.0));
			DrawMetricRowFormat("Cache (total)",
			        frameStats.transientCacheMiss == 0 ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a},
			        "{} hit, {} miss, {} kept",
			        frameStats.transientCacheHit,
			        frameStats.transientCacheMiss,
			        frameStats.cacheSize);
			ImGui::EndTable();
		}

		// Where the frame's GPU time actually goes, sorted, before the table of everything.
		// The table has the same numbers, but a column of 50 floats does not tell you that
		// two passes are the frame - a sorted bar chart does it at a glance.
		{
			struct PassCost
			{
				const std::string* name;
				float gpuMs;
			};
			std::vector<PassCost> costs;
			costs.reserve(passes.size());
			float totalGpuMs = 0.0f;
			for (const auto& pass: passes)
			{
				if (pass.isCulled || pass.isDebugDisabled)
				{
					continue;
				}
				totalGpuMs += pass.lastGpuTimeMs;
				costs.push_back({&pass.name, pass.lastGpuTimeMs});
			}
			std::ranges::sort(costs, std::ranges::greater{}, &PassCost::gpuMs);

			// A long tail of sub-microsecond passes is noise on a chart; the table still has
			// every one of them.
			constexpr std::size_t kMaxBars = 12;
			if (costs.size() > kMaxBars)
			{
				costs.resize(kMaxBars);
			}

			if (!costs.empty() && totalGpuMs > 0.0f)
			{
				ImGui::SeparatorText("GPU cost by pass");
				std::vector<double> values;
				std::vector<double> ticks;
				std::vector<std::string> labelStorage;
				std::vector<const char*> labels;
				values.reserve(costs.size());
				labelStorage.reserve(costs.size());
				for (std::size_t i = 0; i < costs.size(); ++i)
				{
					// Drawn top-down: the highest bar belongs at the top, and the y axis
					// grows upward, so the first entry takes the largest coordinate.
					values.push_back(static_cast<double>(costs[i].gpuMs));
					ticks.push_back(static_cast<double>(costs.size() - 1 - i));
					labelStorage.push_back(ShortRenderPassName(*costs[i].name));
				}
				for (const std::string& label: labelStorage)
				{
					labels.push_back(label.c_str());
				}

				const float chartHeight = 22.0f * static_cast<float>(costs.size()) + 40.0f;
				if (ImPlot::BeginPlot("##passcost", ImVec2(-1.0f, chartHeight), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText))
				{
					ImPlot::SetupAxes("ms", nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_NoGridLines);
					ImPlot::SetupAxisTicks(ImAxis_Y1, ticks.data(), static_cast<int>(ticks.size()), labels.data());
					ImPlot::SetupAxisLimits(ImAxis_Y1, -0.75, static_cast<double>(costs.size()) - 0.25, ImPlotCond_Always);

					std::vector<double> ys = ticks;
					ImPlotSpec bars;
					bars.Flags = ImPlotBarsFlags_Horizontal;
					bars.FillAlpha = 0.85f;
					ImPlot::PlotBars("GPU", values.data(), ys.data(), static_cast<int>(values.size()), 0.62, bars);
					ImPlot::EndPlot();
				}
				ImGui::TextDisabled("%.3f ms total across %zu live passes", static_cast<double>(totalGpuMs), passes.size());
			}
		}

		ImGui::InputTextWithHint("##RenderGraphFilter", "Filter passes or source files", m_renderGraphFilter, sizeof(m_renderGraphFilter));
		ImGui::SameLine();
		if (ImGui::Button("Reset samples"))
		{
			m_renderPassBenchmarks.clear();
		}
		chrome::SameLineOrWrap(chrome::ButtonWidth("Enable all"));
		if (ImGui::Button("Enable all"))
		{
			graph.ClearDebugDisabledPasses();
		}
		ImGui::Checkbox("Show disabled", &m_renderGraphShowDisabled);
		ImGui::SameLine();
		ImGui::Checkbox("Show culled", &m_renderGraphShowCulled);
		ImGui::SameLine();
		ImGui::Checkbox("Track hottest", &m_renderGraphAutoSelectHotPass);

		const std::string filterLower = LowerCopy(m_renderGraphFilter);
		if (ImGui::BeginTable("RenderGraphPassTable", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 300.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 34.0f);
			ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 78.0f);
			ImGui::TableSetupColumn("Pass");
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 132.0f);
			ImGui::TableSetupColumn("Contract", ImGuiTableColumnFlags_WidthFixed, 96.0f);
			ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, 62.0f);
			ImGui::TableSetupColumn("Resources", ImGuiTableColumnFlags_WidthFixed, 74.0f);
			ImGui::TableHeadersRow();

			for (const auto& pass: passes)
			{
				if ((!m_renderGraphShowDisabled && pass.isDebugDisabled) || (!m_renderGraphShowCulled && pass.isCulled) || !PassMatchesFilter(pass, filterLower))
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
				ImGui::TextDisabled("%s:%u", pass.declaredFile.empty() ? "-" : pass.declaredFile.c_str(), pass.declaredLine);
				ImGui::TableSetColumnIndex(4);
				if (pass.hasSideEffects)
				{
					ImGui::TextColored(ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "Effect");
				}
				else
				{
					ImGui::TextDisabled("-");
				}
				if (!pass.logicalDependencies.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{0.42f, 0.70f, 0.95f, 1.0f}, "+Dep");
				}
				if (!pass.producedDrawLists.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{0.52f, 0.86f, 0.62f, 1.0f}, "DL+");
				}
				if (!pass.consumedDrawLists.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{0.42f, 0.70f, 0.95f, 1.0f}, "DL-");
				}
				if (!pass.producedFrameProducts.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{0.52f, 0.86f, 0.62f, 1.0f}, "FP+");
				}
				if (!pass.consumedFrameProducts.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{0.42f, 0.70f, 0.95f, 1.0f}, "FP-");
				}
				if (!pass.contractWarnings.empty())
				{
					ImGui::SameLine();
					ImGui::TextColored(ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "Warn");
				}
				ImGui::TableSetColumnIndex(5);
				ImGui::TextColored(MsColor(pass.lastCpuTimeMs), "%.3f", pass.lastCpuTimeMs);
				ImGui::TableSetColumnIndex(6);
				ImGui::TextColored(MsColor(stats.avg), "%.3f", stats.avg);
				ImGui::TableSetColumnIndex(7);
				ImGui::TextColored(MsColor(stats.max), "%.3f", stats.max);
				ImGui::TableSetColumnIndex(8);
				ImGui::Text("%u", pass.preBarrierCount + pass.bufferBarrierCount + pass.signalBarrierCount);
				ImGui::TableSetColumnIndex(9);
				ImGui::Text("%zu", pass.resources.size());
				ImGui::PopID();
			}

			RenderGraph::PassInfo imguiPass;
			imguiPass.name = std::string(kImguiPassName);
			imguiPass.declaredFile = "ImguiSubsystem.cpp";
			if (PassMatchesFilter(imguiPass, filterLower))
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
				ImGui::TextDisabled("src/app/imgui");
				ImGui::TableSetColumnIndex(4);
				ImGui::TextDisabled("External");
				ImGui::TableSetColumnIndex(5);
				ImGui::TextColored(MsColor(imguiCpuMs), "%.3f", imguiCpuMs);
				ImGui::TableSetColumnIndex(6);
				ImGui::TextColored(MsColor(stats.avg), "%.3f", stats.avg);
				ImGui::TableSetColumnIndex(7);
				ImGui::TextColored(MsColor(stats.max), "%.3f", stats.max);
				ImGui::TableSetColumnIndex(8);
				ImGui::TextDisabled("-");
				ImGui::TableSetColumnIndex(9);
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
				DrawMetricRow("Source", "src/app/imgui/ImguiSubsystem.cpp");
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
			DrawMetricRow("Side effects",
			        pass.hasSideEffects ? (pass.sideEffectReason.empty() ? "Yes" : pass.sideEffectReason.c_str()) : "No",
			        pass.hasSideEffects ? ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a} : ImVec4{colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a});
			const std::string dependencies = JoinStrings(pass.logicalDependencies);
			const std::string producedDrawLists = JoinStrings(pass.producedDrawLists);
			const std::string consumedDrawLists = JoinStrings(pass.consumedDrawLists);
			const std::string producedProducts = JoinStrings(pass.producedFrameProducts);
			const std::string consumedProducts = JoinStrings(pass.consumedFrameProducts);
			const std::string contractWarnings = JoinStrings(pass.contractWarnings);
			DrawMetricRow("Warnings",
			        contractWarnings.empty() ? "None" : contractWarnings.c_str(),
			        contractWarnings.empty() ? ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a} : ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a});
			DrawMetricRow("Dependencies", dependencies.empty() ? "None" : dependencies.c_str());
			DrawMetricRow("Draw lists out", producedDrawLists.empty() ? "None" : producedDrawLists.c_str());
			DrawMetricRow("Draw lists in", consumedDrawLists.empty() ? "None" : consumedDrawLists.c_str());
			DrawMetricRow("Products out", producedProducts.empty() ? "None" : producedProducts.c_str());
			DrawMetricRow("Products in", consumedProducts.empty() ? "None" : consumedProducts.c_str());
			DrawMetricRowFormat("Barriers", "{} image, {} buffer, {} signal, {} wait groups", pass.preBarrierCount, pass.bufferBarrierCount, pass.signalBarrierCount, pass.waitCount);
			DrawMetricRowFormat("Writes", "{} color, depth {}", pass.colorWriteCount, pass.hasDepthWrite ? "yes" : "no");
			DrawMetricRowFormat("Reads/accesses", "{} images, {} buffers", pass.imageAccessCount, pass.bufferAccessCount);
			if (pass.extentOverride.has_value())
			{
				DrawMetricRowFormat("Extent", "{} x {}", pass.extentOverride->width, pass.extentOverride->height);
			}
			else
			{
				DrawMetricRow("Extent", "Frame target");
			}
			if (!pass.declaredFile.empty())
			{
				DrawMetricRowFormat("Declared", "{}:{}", pass.declaredFile, pass.declaredLine);
			}
			ImGui::EndTable();
		}

		if (!products.empty() && ImGui::CollapsingHeader("Frame Products", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("RenderGraphFrameProducts", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Type");
				ImGui::TableSetupColumn("Source");
				ImGui::TableSetupColumn("Producer");
				ImGui::TableSetupColumn("Consumers");
				ImGui::TableSetupColumn("Metadata");
				ImGui::TableHeadersRow();
				for (const auto& product: products)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(product.name.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("%s", product.typeName.c_str());
					ImGui::TableSetColumnIndex(2);
					ImGui::TextDisabled("%s", ProductSourceName(product.metadata.source));
					ImGui::TableSetColumnIndex(3);
					if (product.producerPass.empty())
					{
						ImGui::TextColored(ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "Missing");
					}
					else
					{
						ImGui::TextUnformatted(product.producerPass.c_str());
					}
					ImGui::TableSetColumnIndex(4);
					const std::string consumers = JoinStrings(product.consumerPasses);
					if (consumers.empty())
					{
						ImGui::TextColored(ImVec4{colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a}, "None");
					}
					else
					{
						ImGui::TextUnformatted(consumers.c_str());
					}
					ImGui::TableSetColumnIndex(5);
					std::array<char, 96> metadata{};
					if (product.metadata.extent.has_value())
					{
						if (product.metadata.frameSlot == UINT32_MAX && product.metadata.bindlessSlot == UINT32_MAX)
						{
							FormatToBuffer(metadata, "slot -, extent {}x{}, bindless -", product.metadata.extent->width, product.metadata.extent->height);
						}
						else if (product.metadata.frameSlot == UINT32_MAX)
						{
							FormatToBuffer(metadata, "slot -, extent {}x{}, bindless {}", product.metadata.extent->width, product.metadata.extent->height, product.metadata.bindlessSlot);
						}
						else if (product.metadata.bindlessSlot == UINT32_MAX)
						{
							FormatToBuffer(metadata, "slot {}, extent {}x{}, bindless -", product.metadata.frameSlot, product.metadata.extent->width, product.metadata.extent->height);
						}
						else
						{
							FormatToBuffer(metadata, "slot {}, extent {}x{}, bindless {}", product.metadata.frameSlot, product.metadata.extent->width, product.metadata.extent->height, product.metadata.bindlessSlot);
						}
					}
					else if (product.metadata.frameSlot == UINT32_MAX && product.metadata.bindlessSlot == UINT32_MAX)
					{
						FormatToBuffer(metadata, "slot -, extent -, bindless -");
					}
					else if (product.metadata.frameSlot == UINT32_MAX)
					{
						FormatToBuffer(metadata, "slot -, extent -, bindless {}", product.metadata.bindlessSlot);
					}
					else if (product.metadata.bindlessSlot == UINT32_MAX)
					{
						FormatToBuffer(metadata, "slot {}, extent -, bindless -", product.metadata.frameSlot);
					}
					else
					{
						FormatToBuffer(metadata, "slot {}, extent -, bindless {}", product.metadata.frameSlot, product.metadata.bindlessSlot);
					}
					ImGui::TextDisabled("%s", metadata.data());
				}
				ImGui::EndTable();
			}
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
} // namespace aether::editor
