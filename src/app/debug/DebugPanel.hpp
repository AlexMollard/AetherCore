#pragma once

#include <string>
#include <string_view>

#include <imgui.h>

#include "Color.hpp"

namespace aether
{
	class TomlConfig;
}

namespace aether::app
{
	struct LayerContext;

	// Shared ImGui draw helpers used across panels.
	inline void DrawMetricRow(const char* label, const char* value, ImVec4 color = {colors::TextSecondary.r, colors::TextSecondary.g, colors::TextSecondary.b, colors::TextSecondary.a})
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(color, "%s", value);
	}

	inline ImVec4 FpsColor(float fps) noexcept
	{
		if (fps >= 55.f)
		{
			return {colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a};
		}
		if (fps >= 30.f)
		{
			return {colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a};
		}
		return {colors::Error.r, colors::Error.g, colors::Error.b, colors::Error.a};
	}

	inline ImVec4 MsColor(float ms) noexcept
	{
		if (ms <= 16.667f)
		{
			return {colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a};
		}
		if (ms <= 25.f)
		{
			return {colors::Warn.r, colors::Warn.g, colors::Warn.b, colors::Warn.a};
		}
		return {colors::Error.r, colors::Error.g, colors::Error.b, colors::Error.a};
	}

	// Converts a glm::vec4 color (from colors:: namespace) to ImU32 for ImDrawList.
	inline ImU32 ToU32(const glm::vec4& c) noexcept
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
	}

	// Strips the " (SourceFile.cpp:123)" suffix from a render pass name.
	inline std::string ShortRenderPassName(std::string_view name)
	{
		const std::size_t sourceSuffix = name.find(" (");
		if (sourceSuffix != std::string_view::npos)
		{
			name = name.substr(0, sourceSuffix);
		}
		return std::string(name);
	}

	// Base class for a debug sub-panel.
	// DebugLayer owns a vector of these and delegates lifecycle calls.
	class DebugPanel
	{
	public:
		virtual ~DebugPanel() = default;

		virtual std::string_view GetName() const = 0;

		// Whether the panel's window is open on a fresh layout, before any persisted
		// per-panel visibility is applied. Override to start a niche panel hidden.
		[[nodiscard]] virtual bool DefaultVisible() const
		{
			return true;
		}

		[[nodiscard]] bool IsVisible() const
		{
			return m_visible;
		}

		void SetVisible(bool visible)
		{
			m_visible = visible;
		}

		// Pass to ImGui::Begin(name, VisiblePtr()) so the window's close button and
		// the Window menu toggle share a single flag.
		[[nodiscard]] bool* VisiblePtr()
		{
			return &m_visible;
		}

		virtual void OnAttach(LayerContext&)
		{
		}

		virtual void OnDetach(LayerContext&)
		{
		}

		virtual void OnUpdate(LayerContext&)
		{
		}

		virtual void OnImGui(LayerContext& context) = 0;

		// See AppLayer::OnRenderTargetsInvalidated. DebugLayer forwards the
		// broadcast to each panel so panels holding ImGui texture descriptors
		// (viewport, texture inspector) can drop them on recreate.
		virtual void OnRenderTargetsInvalidated(LayerContext&)
		{
		}

		virtual void LoadSettings(TomlConfig&, LayerContext&)
		{
		}

		virtual void SaveSettings(TomlConfig&, LayerContext&) const
		{
		}

	protected:
		bool m_visible = true;
	};
} // namespace aether::app
