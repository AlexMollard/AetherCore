#pragma once

#include <bit>
#include <cstdint>
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

	// Window title carrying an unsaved-work marker. Everything after ### is the ImGui ID, so
	// the visible label can change while the docking layout and every SetWindowFocus lookup
	// (which hashes the same trailing name) keep resolving to the same window.
	inline std::string DocumentTitle(std::string_view name, bool dirty)
	{
		std::string title(name);
		if (dirty)
		{
			title += " *";
		}
		title += "###";
		title += name;
		return title;
	}

	// Folds a document's authored state into one 64-bit value (FNV-1a). Panels compare this
	// instead of rebuilding a signature string because the title bar asks "is this unsaved?"
	// every frame: a thousand-region atlas would otherwise churn tens of kilobytes per frame
	// to draw one asterisk. Allocation-free by construction.
	class DocumentHash
	{
	public:
		void Add(std::string_view text) noexcept
		{
			for (const char c: text)
			{
				Mix(static_cast<std::uint8_t>(c));
			}
			// Separator, so "ab" + "c" cannot collide with "a" + "bc".
			Mix(0xFFu);
		}

		void Add(std::int64_t value) noexcept
		{
			const auto bits = static_cast<std::uint64_t>(value);
			for (int shift = 0; shift < 64; shift += 8)
			{
				Mix(static_cast<std::uint8_t>((bits >> shift) & 0xFFu));
			}
		}

		void Add(float value) noexcept
		{
			Add(static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(value)));
		}

		[[nodiscard]] std::uint64_t Value() const noexcept
		{
			return m_hash;
		}

	private:
		void Mix(std::uint8_t byte) noexcept
		{
			m_hash ^= byte;
			m_hash *= 1099511628211ull;
		}

		std::uint64_t m_hash = 14695981039346656037ull;
	};

	class DebugPanel
	{
	public:
		virtual ~DebugPanel() = default;

		[[nodiscard]] virtual std::string_view GetName() const = 0;

		// Whether this panel is holding an edited document that is not on disk. The scene's
		// undo stack cannot see these - a material, atlas, clip or canvas is a separate file -
		// so closing the editor asks every panel rather than only the scene.
		[[nodiscard]] virtual bool HasUnsavedWork(app::LayerContext& /*context*/) const
		{
			return false;
		}

		// Writes that document out. Returns whether the panel is actually clean afterwards,
		// so a failed write cannot be mistaken for a saved one and close anyway.
		virtual bool SaveUnsavedWork(app::LayerContext& /*context*/)
		{
			return true;
		}

		// Whether the panel's window is open on a fresh layout, before any persisted
		// Off unless a panel says otherwise.
		//
		// This was on, which meant a panel joined the default layout simply by existing. With
		// 23 of them that is not a layout, it is everything at once: a first run opened with a
		// UI Canvas editor over the main area, a Lighting panel, and a raw texture list of
		// RenderGraph.Transient.Aliased[n] filling the bottom half - while the Console, where
		// script errors and Log.Info go, was one of the few that had opted out.
		//
		// Opting in is also the safer direction. A new panel added later is a specialist tool
		// far more often than it is something every project wants on screen, and the cost of
		// guessing wrong is now a panel someone has to go and find rather than one shipped
		// into everybody's first impression.
		[[nodiscard]] virtual bool DefaultVisible() const
		{
			return false;
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

		// The ImGui window title this panel Begins, which is what DockBuilderDockWindow keys
		// on. Usually the same as GetName(), but a few panels present a shorter title than
		// their registered name - and a dock call naming the wrong string silently does
		// nothing, which is exactly the kind of no-op that looks like it works.
		[[nodiscard]] virtual std::string_view GetWindowTitle() const
		{
			return GetName();
		}

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
