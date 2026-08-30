#include "debug/TonemapPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <imgui.h>

#include "Color.hpp"
#include "layers/AppLayer.hpp"
#include "passes/TonemapDefs.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "rendering/Renderer.hpp"
#include "utils/SettingsService.hpp"

namespace aether::editor
{
	namespace
	{

		float Reinhard(float x)
		{
			return x / (x + 1.0f);
		}

		// Stephen Hill's RRT+ODT fit, the same one AcesFilmic.slangh evaluates. Grey
		// survives the AP1 matrices untouched - their rows each sum to one - so on the
		// achromatic axis the whole transform reduces to this fit alone, which is exactly
		// what a one-dimensional curve plot and a grey probe need.
		//
		// This used to be the Narkowicz curve the shader deliberately replaced, so the
		// plot and the HDR probe were reporting a different operator than the one being
		// rendered: 0.267 against 0.106 at mid grey, and triple the correct value down in
		// the shadows. A diagnostic that lies is worse than no diagnostic.
		float AcesFilmic(float x)
		{
			const float v = (std::max) (x, 0.0f);
			const float a = v * (v + 0.0245786f) - 0.000090537f;
			const float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
			return (std::min) ((std::max) (a / (std::max) (b, 1e-6f), 0.0f), 1.0f);
		}

		float Uncharted2Partial(float x)
		{
			const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
			return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
		}

		float Uncharted2(float x)
		{
			const float curr = Uncharted2Partial(x * 2.0f);
			const float whiteScale = 1.0f / Uncharted2Partial(11.2f);
			const float v = curr * whiteScale;
			return (std::min) ((std::max) (v, 0.0f), 1.0f);
		}

		float HejlRichard(float x)
		{
			// Gamma is baked into this curve, so linearise it the way the shader does -
			// the pass re-applies the encode on the way to the screen.
			const float v = (std::max) (x - 0.004f, 0.0f);
			return std::pow((v * (6.2f * v + 0.5f)) / (v * (6.2f * v + 1.7f) + 0.06f), 2.2f);
		}

		float LinearClamp(float x)
		{
			return (std::min) ((std::max) (x, 0.0f), 1.0f);
		}

		float ExponentialShift(float x)
		{
			return 1.0f - std::exp(-x);
		}

		float FilmicDice(float x)
		{
			const float v = (std::max) (x, 0.0f);
			return std::pow((v * (6.2f * v + 0.5f)) / (v * (6.2f * v + 1.7f) + 0.06f), 2.2f);
		}

		float Lottes(float x)
		{
			return x * (1.47f * x + 0.12f) / (x * (1.47f * x + 1.46f) + 0.12f);
		}

		float RomBinDaHouse(float x)
		{
			const float v = x * (x * 0.26f + 0.68f) / (x * (x * 0.26f + 1.14f) + 0.14f);
			return (std::min) ((std::max) (v, 0.0f), 1.0f);
		}

		float Vanilla(float x)
		{
			return x;
		}

		// AgX on the achromatic axis. The inset and outset matrices are what give
		// AgX its hue behaviour, and both have rows summing to one, so on grey they
		// cancel and what is left is the log encoding and the sigmoid - which is
		// exactly the part a one-dimensional curve plot can show.
		float Agx(float x)
		{
			constexpr float kMinEv = -12.47393f;
			constexpr float kMaxEv = 4.026069f;

			float v = std::log2((std::max) (x, 1e-10f));
			v = (std::min) ((std::max) (v, kMinEv), kMaxEv);
			v = (v - kMinEv) / (kMaxEv - kMinEv);

			const float v2 = v * v;
			const float v4 = v2 * v2;
			float y = 15.5f * v4 * v2 - 40.14f * v4 * v + 31.96f * v4 - 6.868f * v2 * v + 0.4298f * v2 + 0.1191f * v - 0.00232f;
			y = (std::min) ((std::max) (y, 0.0f), 1.0f);

			// Matches the shader, which returns linear so the pass's own 1/2.2 can
			// re-apply AgX's 2.2 encoding.
			return std::pow(y, 2.2f);
		}

		float TonemapByIndex(std::uint32_t idx, float x)
		{
			switch (idx)
			{
				case 0u:
					return Reinhard(x);
				case 1u:
					return AcesFilmic(x);
				case 2u:
					return Uncharted2(x);
				case 3u:
					return HejlRichard(x);
				case 4u:
					return LinearClamp(x);
				case 5u:
					return ExponentialShift(x);
				case 6u:
					return FilmicDice(x);
				case 7u:
					return Lottes(x);
				case 8u:
					return RomBinDaHouse(x);
				case 9u:
					return Vanilla(x);
				case 10u:
					return Agx(x);
				default:
					return Reinhard(x);
			}
		}

		constexpr ImU32 kCurveColors[] = {
		        IM_COL32(200, 100, 100, 220),
		        IM_COL32(100, 200, 100, 220),
		        IM_COL32(100, 100, 200, 220),
		        IM_COL32(200, 200, 80, 220),
		        IM_COL32(120, 120, 120, 220),
		        IM_COL32(200, 120, 200, 220),
		        IM_COL32(80, 200, 200, 220),
		        IM_COL32(200, 150, 80, 220),
		        IM_COL32(80, 200, 150, 220),
		        IM_COL32(200, 200, 200, 220),
		        IM_COL32(150, 120, 200, 220),
		};

	} // namespace

	void TonemapPanel::OnImGui(app::LayerContext& context)
	{
		auto& rendering = context.Get<RenderingSubsystem>();
		PostProcessStack& stack = rendering.GetPostProcessStack();

		if (!ImGui::Begin(GetName().data(), VisiblePtr()))
		{
			stack.SetHistogramCaptureEnabled(false);
			ImGui::End();
			return;
		}
		stack.SetHistogramCaptureEnabled(true);
		chrome::PanelHeader("TONEMAP");

		const char* preview = kTonemapDefs[static_cast<std::size_t>(stack.GetTonemapMode())].name;
		if (ImGui::BeginCombo("Operator", preview))
		{
			for (std::size_t i = 0; i < kTonemapCount; ++i)
			{
				const bool selected = static_cast<std::uint32_t>(stack.GetTonemapMode()) == i;
				if (ImGui::Selectable(kTonemapDefs[i].name, selected))
				{
					// Write through the setting where there is one, so this panel and the
					// settings file cannot end up disagreeing about which operator is
					// selected - and so the choice survives a restart. Falling back to the
					// stack directly keeps the panel working in a build with no settings
					// service, which is what the preview tools run as.
					if (auto* settings = context.TryGet<aether::SettingsService>())
					{
						settings->Values().graphics.tonemap = kTonemapDefs[i].name;
						settings->ApplyField("graphics.tonemap");
						settings->MarkDirty();
					}
					else
					{
						stack.SetTonemapMode(kTonemapDefs[i].mode);
					}
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		bool autoExposure = stack.IsAutoExposureEnabled();
		if (ImGui::Checkbox("Auto exposure", &autoExposure))
		{
			stack.SetAutoExposureEnabled(autoExposure);
		}

		float exposure = stack.GetExposure();
		if (ImGui::SliderFloat(autoExposure ? "Exposure compensation" : "Exposure", &exposure, 0.01f, 10.0f, "%.2f"))
		{
			stack.SetExposure(exposure);
		}

		if (autoExposure)
		{
			float key = stack.GetAutoExposureKey();
			if (ImGui::SliderFloat("Key", &key, 0.02f, 0.6f, "%.3f"))
			{
				stack.SetAutoExposureKey(key);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Middle grey the scene average is driven toward. Higher is a brighter image.");
			}
			float speed = stack.GetAutoExposureSpeed();
			if (ImGui::SliderFloat("Adaptation", &speed, 0.05f, 8.0f, "%.2f"))
			{
				stack.SetAutoExposureSpeed(speed);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("How quickly the eye adapts, in e-folds per second. Low is a slow, cinematic adjust.");
			}
			ImGui::Text("Adapted: %.3fx  (final %.3fx)", stack.GetAutoExposureValue(), stack.GetAutoExposureValue() * exposure);
		}

		ImGui::SeparatorText("Bloom");
		float bloomStrength = stack.GetBloomStrength();
		if (ImGui::SliderFloat("Strength", &bloomStrength, 0.0f, 0.5f, "%.3f"))
		{
			stack.SetBloomStrength(bloomStrength);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("How far the image is blended toward the blurred chain. 0 disables bloom.");
		}
		float bloomRadius = stack.GetBloomFilterRadius();
		if (ImGui::SliderFloat("Radius", &bloomRadius, 0.5f, 4.0f, "%.2f"))
		{
			stack.SetBloomFilterRadius(bloomRadius);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Width of the tent filter on the way back up the chain, in source texels. Wider spreads the glow further at the same cost.");
		}
		// Only offered when there is a settings service to write through. The grade lives
		// entirely in the settings, so without one there is nothing to edit or persist -
		// and the preview tools run exactly that way, which is why the operator selector
		// above carries a fallback.
		if (auto* settings = context.TryGet<aether::SettingsService>())
		{
			ImGui::SeparatorText("Colour grade");
			// Written through the settings service for the same reason the operator above
			// is: the grade is a persisted setting, so editing it here and leaving the
			// settings file behind would make the two disagree and the change vanish on
			// restart. Each slider pushes all four values, because SetGrade takes the
			// transform whole.
			auto& g = settings->Values().graphics;

			bool changed = false;
			changed |= ImGui::SliderFloat("Contrast", &g.gradeContrast, 0.25f, 2.5f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Contrast about middle grey, in linear light before the tonemap curve. 1 leaves the image untouched.");
			}
			changed |= ImGui::SliderFloat("Saturation", &g.gradeSaturation, 0.0f, 2.5f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("0 is greyscale, 1 is untouched, above 1 pushes further from grey.");
			}
			changed |= ImGui::SliderFloat("Temperature", &g.gradeTemperature, -1.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Warm/cool white balance. Luminance is held constant, so this does not double as an exposure control.");
			}
			changed |= ImGui::SliderFloat("Tint", &g.gradeTint, -1.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Green/magenta white balance.");
			}

			ImGui::SeparatorText("Vignette");
			changed |= ImGui::SliderFloat("Amount", &g.vignetteIntensity, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Lens falloff toward the edge of frame, in linear light so corner highlights roll off rather than just dimming. 0 is off.");
			}
			changed |= ImGui::SliderFloat("Roundness", &g.vignetteRoundness, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("1 is circular on screen; 0 follows the aspect ratio, darkening the sides of a wide frame rather than only the corners.");
			}

			ImGui::SeparatorText("Chromatic aberration");
			changed |= ImGui::SliderFloat("Fringing", &g.chromaticAberration, 0.0f, 16.0f, "%.2f px");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Red/blue separation at the corner of frame, in pixels. Zero at the centre and strongest at the edges, as on a real lens. 0 is off.");
			}

			if (ImGui::SmallButton("Reset grade"))
			{
				g.gradeContrast = 1.0f;
				g.gradeSaturation = 1.0f;
				g.gradeTemperature = 0.0f;
				g.gradeTint = 0.0f;
				g.vignetteIntensity = 0.0f;
				g.vignetteRoundness = 1.0f;
				g.chromaticAberration = 0.0f;
				changed = true;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Back to the identity transform - the image the renderer produces with no grade at all.");
			}

			if (changed)
			{
				// Both keys, because the grade and the vignette are pushed by separate
				// setters - applying only one would leave the other stale until something
				// else happened to touch it.
				settings->ApplyField("graphics.gradeContrast");
				settings->ApplyField("graphics.vignetteIntensity");
				settings->MarkDirty();
			}
		}

		ImGui::Separator();

		bool debugCompare = stack.IsDebugCompareEnabled();
		if (ImGui::Checkbox("Side-by-side comparison", &debugCompare))
		{
			stack.SetDebugCompare(debugCompare);
			stack.SetDebugModeCount(debugCompare ? static_cast<std::uint32_t>(kTonemapCount) : 0u);
		}
		if (debugCompare)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Screen is split into %zu vertical strips, each using a different operator.\nA colored bar at the top identifies each strip.", kTonemapCount);
			}
			ImGui::Indent();
			int modeCount = static_cast<int>(stack.GetDebugModeCount());
			if (ImGui::SliderInt("Visible modes", &modeCount, 1, static_cast<int>(kTonemapCount)))
			{
				stack.SetDebugModeCount(static_cast<std::uint32_t>(modeCount));
			}

			ImGui::Separator();
			ImGui::TextUnformatted("Strip legend");
			if (ImGui::BeginTable("##stripLegend", 2, ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Bar");
				ImGui::TableSetupColumn("Operator");
				ImGui::TableHeadersRow();
				for (std::size_t i = 0; i < kTonemapCount && i < static_cast<std::size_t>(modeCount); ++i)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const ImVec2 barMin = ImGui::GetCursorScreenPos();
					const ImVec2 barMax = ImVec2(barMin.x + 24, barMin.y + 14);
					ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, kCurveColors[i]);
					ImGui::Dummy(ImVec2(28, 14));
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(kTonemapDefs[i].name);
				}
				ImGui::EndTable();
			}

			ImGui::Unindent();
		}

		ImGui::Separator();

		{
			// The legend sits to the right of the plot, so the plot cannot have the full
			// content width or the legend lands outside the window and is clipped away -
			// which left the curves unlabelled. Measure the widest operator name rather
			// than guessing a column width, so adding an operator cannot silently
			// truncate it.
			float legendTextW = 0.0f;
			for (const TonemapDef& def: kTonemapDefs)
			{
				legendTextW = (std::max) (legendTextW, ImGui::CalcTextSize(def.name).x);
			}
			const float legendW = legendTextW + 26.0f; // swatch + gap
			const float avail = ImGui::GetContentRegionAvail().x;
			const float plotW = (std::max) (avail - legendW, avail * 0.5f);
			const float plotH = 220.0f;
			const ImVec2 plotPos = ImGui::GetCursorScreenPos();
			const ImVec2 plotSize(plotW, plotH);
			const ImVec2 plotMin = plotPos;
			const ImVec2 plotMax(plotPos.x + plotSize.x, plotPos.y + plotSize.y);

			ImGui::InvisibleButton("##curvePlot", ImVec2(avail, plotH));

			ImDrawList* dl = ImGui::GetWindowDrawList();

			constexpr std::size_t kSamples = 256;
			constexpr float kXMax = 10.0f;

			dl->AddRectFilled(plotPos, ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y), IM_COL32(20, 20, 25, 220));

			for (int i = 0; i <= 10; ++i)
			{
				const float x = plotPos.x + (static_cast<float>(i) / 10.0f) * plotSize.x;
				dl->AddLine(ImVec2(x, plotPos.y), ImVec2(x, plotPos.y + plotSize.y), IM_COL32(50, 50, 60, 180));
			}
			for (int i = 0; i <= 10; ++i)
			{
				const float y = plotPos.y + plotSize.y - (static_cast<float>(i) / 10.0f) * plotSize.y;
				dl->AddLine(ImVec2(plotPos.x, y), ImVec2(plotPos.x + plotSize.x, y), IM_COL32(50, 50, 60, 180));
			}

			dl->AddText(ImVec2(plotPos.x + 2, plotPos.y + 2), IM_COL32(180, 180, 180, 200), "1.0");
			dl->AddText(ImVec2(plotPos.x + plotSize.x - 30, plotPos.y + plotSize.y - 14), IM_COL32(180, 180, 180, 200), "10");

			// Clip the curves to the plot box. Not every operator lands inside it: Vanilla
			// is the identity, so at the right-hand edge of the graph it returns 10 and its
			// point sits nine plot-heights above the top. The draw list clips to the WINDOW,
			// not to this rectangle, so without pushing one that line streaked up across the
			// whole panel and over the tab bar. Clipping rather than clamping keeps the plot
			// honest - a curve that leaves the top is one that exceeds 1.0, which is exactly
			// what Vanilla does and worth being able to see.
			dl->PushClipRect(plotMin, plotMax, true);
			for (std::size_t m = 0; m < kTonemapCount; ++m)
			{
				ImVec2 pts[kSamples];
				for (std::size_t s = 0; s < kSamples; ++s)
				{
					const float input = (static_cast<float>(s) / static_cast<float>(kSamples - 1)) * kXMax;
					const float output = TonemapByIndex(static_cast<std::uint32_t>(m), input);
					pts[s].x = plotPos.x + (input / kXMax) * plotSize.x;
					// Keep the coordinate finite and near the box even for an operator that
					// runs far off it; the clip decides what is actually visible.
					const float clamped = (std::min) ((std::max) (output, -1.0f), 2.0f);
					pts[s].y = plotPos.y + plotSize.y - (clamped * plotSize.y);
				}
				dl->AddPolyline(pts, kSamples, kCurveColors[m], ImDrawFlags_None, 2.0f);
			}
			dl->PopClipRect();

			for (std::size_t m = 0; m < kTonemapCount; ++m)
			{
				const float legendX = plotMax.x + 8.0f;
				const float legendY = plotPos.y + 4.0f + static_cast<float>(m) * 18.0f;
				dl->AddRectFilled(ImVec2(legendX, legendY), ImVec2(legendX + 10, legendY + 10), kCurveColors[m]);
				dl->AddText(ImVec2(legendX + 14, legendY - 2), IM_COL32(200, 200, 200, 220), kTonemapDefs[m].name);
			}
		}

		ImGui::Dummy(ImVec2(0.0f, 4.0f));

		{
			ImGui::TextUnformatted("HDR Probe");
			ImGui::Separator();

			static float s_probeValue = 1.0f;
			ImGui::SetNextItemWidth(100.0f);
			ImGui::InputFloat("HDR", &s_probeValue, 0.1f, 1.0f, "%.3f");

			if (ImGui::BeginTable("##probeTable", 2, ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Operator");
				ImGui::TableSetupColumn("Mapped");
				ImGui::TableHeadersRow();

				for (std::size_t i = 0; i < kTonemapCount; ++i)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(kTonemapDefs[i].name);
					ImGui::TableSetColumnIndex(1);
					const float mapped = TonemapByIndex(static_cast<std::uint32_t>(i), s_probeValue);
					const bool clipped = mapped >= 0.999f || mapped <= 0.001f;
					const ImVec4 color = clipped ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
					ImGui::TextColored(color, "%.4f", mapped);
				}
				ImGui::EndTable();
			}
		}

		ImGui::Separator();

		{
			int updatePeriod = static_cast<int>(stack.GetHistogramUpdatePeriod());
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::SliderInt("Histogram period", &updatePeriod, 1, 30))
			{
				stack.SetHistogramUpdatePeriod(static_cast<std::uint32_t>(updatePeriod));
			}

			int sampleStride = static_cast<int>(stack.GetHistogramSampleStride());
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::SliderInt("Histogram stride", &sampleStride, 1, 16))
			{
				stack.SetHistogramSampleStride(static_cast<std::uint32_t>(sampleStride));
			}
		}

		enum class HistogramAxis
		{
			HdrLog,
			LdrLinear,
		};

		auto drawHistogram = [](const char* label, const float* bins, std::uint32_t binCount, ImU32 color, HistogramAxis axis)
		{
			const float plotW = (std::max) (ImGui::GetContentRegionAvail().x, 1.0f);
			const float plotH = 80.0f;
			const ImVec2 plotPos = ImGui::GetCursorScreenPos();

			ImGui::TextUnformatted(label);
			ImGui::PushID(label);
			ImGui::InvisibleButton("##hist", ImVec2(plotW, plotH));
			ImGui::PopID();

			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(plotPos, ImVec2(plotPos.x + plotW, plotPos.y + plotH), IM_COL32(20, 20, 25, 220));

			const float barW = plotW / static_cast<float>(binCount);
			for (std::uint32_t i = 0; i < binCount; ++i)
			{
				const float h = std::sqrt((std::min) ((std::max) (bins[i], 0.0f), 1.0f)) * plotH;
				if (h > 0.0f)
				{
					const float x0 = plotPos.x + static_cast<float>(i) * barW;
					const float y0 = plotPos.y + plotH - h;
					dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW + 1.0f, plotPos.y + plotH), color);
				}
			}

			auto markerX = [&](float luminance)
			{
				float t = luminance;
				if (axis == HistogramAxis::HdrLog)
				{
					const float logMin = PostProcessStack::GetHistogramLogMin();
					const float logMax = PostProcessStack::GetHistogramLogMax();
					t = (std::log2(luminance) - logMin) / (logMax - logMin);
				}
				return plotPos.x + (std::min) ((std::max) (t, 0.0f), 1.0f) * plotW;
			};

			auto drawMarker = [&](float luminance, const char* text)
			{
				const float x = markerX(luminance);
				dl->AddLine(ImVec2(x, plotPos.y), ImVec2(x, plotPos.y + plotH), IM_COL32(70, 70, 80, 180));
				const ImVec2 textSize = ImGui::CalcTextSize(text);
				const float textX = (std::min) ((std::max) (x - textSize.x * 0.5f, plotPos.x + 2.0f), plotPos.x + plotW - textSize.x - 2.0f);
				dl->AddText(ImVec2(textX, plotPos.y + plotH - 14.0f), IM_COL32(140, 140, 140, 200), text);
			};

			if (axis == HistogramAxis::HdrLog)
			{
				drawMarker(0.001f, "0.001");
				drawMarker(1.0f, "1");
				drawMarker(1000.0f, "~1000");
			}
			else
			{
				drawMarker(0.0f, "0");
				drawMarker(0.5f, "0.5");
				drawMarker(1.0f, "1");
			}
		};

		if (stack.IsHistogramValid())
		{
			constexpr std::uint32_t kBinCount = PostProcessStack::GetHistogramBinCount();
			drawHistogram("HDR log luminance (pre-tonemap)", stack.GetHdrHistogramBins(), kBinCount, IM_COL32(100, 200, 255, 160), HistogramAxis::HdrLog);
			drawHistogram("LDR luminance 0..1 (post-tonemap)", stack.GetLdrHistogramBins(), kBinCount, IM_COL32(255, 180, 100, 160), HistogramAxis::LdrLinear);
		}
		else
		{
			ImGui::TextUnformatted("Luminance Histogram");
			ImGui::SameLine();
			ImGui::TextDisabled("(collecting...)");
		}

		ImGui::End();
	}

} // namespace aether::editor
