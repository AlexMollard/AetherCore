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
}

namespace aether::editor
{
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

	inline ImU32 ToU32(const glm::vec4& c) noexcept
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
	}

	inline std::string ShortRenderPassName(std::string_view name)
	{
		const std::size_t sourceSuffix = name.find(" (");
		if (sourceSuffix != std::string_view::npos)
		{
			name = name.substr(0, sourceSuffix);
		}
		return std::string(name);
	}

	class DebugPanel
	{
	public:
		virtual ~DebugPanel() = default;

		[[nodiscard]] virtual std::string_view GetName() const = 0;

		// Whether the panel's window is open on a fresh layout, before any persisted
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

		[[nodiscard]] bool* VisiblePtr()
		{
			return &m_visible;
		}

		virtual void OnAttach(app::LayerContext& /*unused*/)
		{
		}

		virtual void OnDetach(app::LayerContext& /*unused*/)
		{
		}

		virtual void OnUpdate(app::LayerContext& /*unused*/)
		{
		}

		virtual void OnImGui(app::LayerContext& context) = 0;

		virtual void OnRenderTargetsInvalidated(app::LayerContext& /*unused*/)
		{
		}

		virtual void LoadSettings(TomlConfig& /*unused*/, app::LayerContext& /*unused*/)
		{
		}

		virtual void SaveSettings(TomlConfig& /*unused*/, app::LayerContext& /*unused*/) const
		{
		}

	protected:
		bool m_visible = true;
	};
} // namespace aether::editor
