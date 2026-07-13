#include "debug/ThemePanel.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "debug/Icons.hpp"
#include "io/PlatformPaths.hpp"
#include "layers/AppLayer.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
		using chrome::EditorTheme;

		std::filesystem::path ThemePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "EditorTheme.toml";
		}

		std::string ToHex(const ImVec4& c)
		{
			const auto ch = [](float v)
			{
				return static_cast<int>(v * 255.0f + 0.5f) & 0xFF;
			};
			char buf[10];
			std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", ch(c.x), ch(c.y), ch(c.z), ch(c.w));
			return buf;
		}

		ImVec4 FromHex(const std::string& hex, const ImVec4& fallback)
		{
			// "#RRGGBB" or "#RRGGBBAA".
			if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#')
			{
				return fallback;
			}
			const auto nib = [](char c) -> int
			{
				if (c >= '0' && c <= '9')
				{
					return c - '0';
				}
				if (c >= 'a' && c <= 'f')
				{
					return c - 'a' + 10;
				}
				if (c >= 'A' && c <= 'F')
				{
					return c - 'A' + 10;
				}
				return -1;
			};
			const auto byteAt = [&](std::size_t i) -> int
			{
				const int hi = nib(hex[i]);
				const int lo = nib(hex[i + 1]);
				return (hi < 0 || lo < 0) ? -1 : (hi * 16 + lo);
			};
			const int r = byteAt(1), g = byteAt(3), b = byteAt(5);
			const int a = hex.size() == 9 ? byteAt(7) : 255;
			if (r < 0 || g < 0 || b < 0 || a < 0)
			{
				return fallback;
			}
			return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
		}

		// An accent preset keeps Night Amber's warm surfaces + text and swaps only
		// the accent triad (the common "I don't want amber" request). onAccent is
		// dark or light to stay legible on the accent fill.
		EditorTheme AccentPreset(const ImVec4& accent, const ImVec4& hover, const ImVec4& active, const ImVec4& onAccent)
		{
			EditorTheme t = chrome::NightAmberTheme();
			t.accent = accent;
			t.accentHover = hover;
			t.accentActive = active;
			t.onAccent = onAccent;
			return t;
		}

		EditorTheme GraphitePreset()
		{
			// A fully cool, neutral alternative (non-warm surfaces + steel accent).
			EditorTheme t;
			t.background = ImVec4(0.055f, 0.058f, 0.066f, 1.0f);
			t.surface = ImVec4(0.086f, 0.090f, 0.102f, 1.0f);
			t.surfaceElevated = ImVec4(0.114f, 0.120f, 0.133f, 1.0f);
			t.border = ImVec4(0.188f, 0.196f, 0.216f, 1.0f);
			t.accent = ImVec4(0.36f, 0.60f, 0.92f, 1.0f);
			t.accentHover = ImVec4(0.47f, 0.70f, 1.00f, 1.0f);
			t.accentActive = ImVec4(0.26f, 0.46f, 0.74f, 1.0f);
			t.onAccent = ImVec4(0.05f, 0.07f, 0.10f, 1.0f);
			t.textPrimary = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
			t.textSecondary = ImVec4(0.58f, 0.62f, 0.69f, 1.0f);
			t.textFaint = ImVec4(0.38f, 0.41f, 0.47f, 1.0f);
			t.success = ImVec4(0.42f, 0.80f, 0.52f, 1.0f);
			t.warning = ImVec4(0.95f, 0.76f, 0.35f, 1.0f);
			t.error = ImVec4(0.92f, 0.42f, 0.42f, 1.0f);
			return t;
		}

		struct Preset
		{
			const char* name;
			EditorTheme theme;
		};

		std::array<Preset, 6> Presets()
		{
			return {{
			        {"Night Amber", chrome::NightAmberTheme()},
			        {"Steel Blue", AccentPreset(ImVec4(0.36f, 0.60f, 0.92f, 1.0f), ImVec4(0.50f, 0.72f, 1.0f, 1.0f), ImVec4(0.26f, 0.46f, 0.74f, 1.0f), ImVec4(0.05f, 0.07f, 0.10f, 1.0f))},
			        {"Emerald", AccentPreset(ImVec4(0.30f, 0.78f, 0.53f, 1.0f), ImVec4(0.42f, 0.90f, 0.64f, 1.0f), ImVec4(0.20f, 0.60f, 0.40f, 1.0f), ImVec4(0.04f, 0.10f, 0.07f, 1.0f))},
			        {"Crimson", AccentPreset(ImVec4(0.90f, 0.36f, 0.42f, 1.0f), ImVec4(1.0f, 0.48f, 0.53f, 1.0f), ImVec4(0.72f, 0.26f, 0.32f, 1.0f), ImVec4(0.12f, 0.05f, 0.06f, 1.0f))},
			        {"Violet", AccentPreset(ImVec4(0.64f, 0.48f, 0.95f, 1.0f), ImVec4(0.74f, 0.60f, 1.0f, 1.0f), ImVec4(0.50f, 0.36f, 0.78f, 1.0f), ImVec4(0.08f, 0.06f, 0.12f, 1.0f))},
			        {"Graphite", GraphitePreset()},
			}};
		}
	} // namespace

	void ThemePanel::OnAttach(app::LayerContext& /*context*/)
	{
		if (LoadPersisted())
		{
			m_loaded = true;
		}
		// Apply on attach so a persisted theme takes hold at startup (ImguiSubsystem
		// already themed to the Night Amber default; this overrides it if saved).
		Apply();
	}

	bool ThemePanel::LoadPersisted()
	{
		std::error_code ec;
		if (!std::filesystem::exists(ThemePath(), ec))
		{
			return false;
		}
		TomlConfig cfg;
		if (!cfg.LoadFromPath(ThemePath()))
		{
			return false;
		}
		const EditorTheme def = chrome::NightAmberTheme();
		const auto read = [&](const char* key, const ImVec4& fallback)
		{
			return FromHex(cfg.GetString(key, ToHex(fallback)), fallback);
		};
		m_theme.background = read("theme.background", def.background);
		m_theme.surface = read("theme.surface", def.surface);
		m_theme.surfaceElevated = read("theme.surface_elevated", def.surfaceElevated);
		m_theme.border = read("theme.border", def.border);
		m_theme.accent = read("theme.accent", def.accent);
		m_theme.accentHover = read("theme.accent_hover", def.accentHover);
		m_theme.accentActive = read("theme.accent_active", def.accentActive);
		m_theme.onAccent = read("theme.on_accent", def.onAccent);
		m_theme.textPrimary = read("theme.text_primary", def.textPrimary);
		m_theme.textSecondary = read("theme.text_secondary", def.textSecondary);
		m_theme.textFaint = read("theme.text_faint", def.textFaint);
		m_theme.success = read("theme.success", def.success);
		m_theme.warning = read("theme.warning", def.warning);
		m_theme.error = read("theme.error", def.error);
		return true;
	}

	void ThemePanel::Persist() const
	{
		TomlConfig cfg;
		cfg.Set("theme.background", ToHex(m_theme.background));
		cfg.Set("theme.surface", ToHex(m_theme.surface));
		cfg.Set("theme.surface_elevated", ToHex(m_theme.surfaceElevated));
		cfg.Set("theme.border", ToHex(m_theme.border));
		cfg.Set("theme.accent", ToHex(m_theme.accent));
		cfg.Set("theme.accent_hover", ToHex(m_theme.accentHover));
		cfg.Set("theme.accent_active", ToHex(m_theme.accentActive));
		cfg.Set("theme.on_accent", ToHex(m_theme.onAccent));
		cfg.Set("theme.text_primary", ToHex(m_theme.textPrimary));
		cfg.Set("theme.text_secondary", ToHex(m_theme.textSecondary));
		cfg.Set("theme.text_faint", ToHex(m_theme.textFaint));
		cfg.Set("theme.success", ToHex(m_theme.success));
		cfg.Set("theme.warning", ToHex(m_theme.warning));
		cfg.Set("theme.error", ToHex(m_theme.error));
		(void) cfg.SaveToPath(ThemePath(), "AetherCore editor theme (edit via the Theme panel).");
	}

	void ThemePanel::Apply()
	{
		chrome::ApplyTheme(m_theme); // rewrites chrome tokens + restyles ImGui widgets
	}

	void ThemePanel::OnImGui(app::LayerContext& /*context*/)
	{
		AE_PROFILE_ZONE();
		ImGui::Begin(GetName().data(), VisiblePtr());
		chrome::PanelHeader("THEME", m_loaded ? "CUSTOM" : "NIGHT AMBER");

		// ── Presets ─────────────────────────────────────────────────────────────
		chrome::SectionTag("PRESETS");
		ImGui::Spacing();
		{
			const auto presets = Presets();
			float avail = ImGui::GetContentRegionAvail().x;
			int perRow = 3;
			const float btnW = (avail - ImGui::GetStyle().ItemSpacing.x * (perRow - 1)) / static_cast<float>(perRow);
			for (std::size_t i = 0; i < presets.size(); ++i)
			{
				if (i % perRow != 0)
				{
					ImGui::SameLine();
				}
				if (chrome::GhostButton(presets[i].name, ImVec2(btnW, 0.0f)))
				{
					m_theme = presets[i].theme;
					m_loaded = std::string(presets[i].name) != "Night Amber";
					Apply();
					Persist();
				}
			}
		}
		ImGui::Spacing();

		// ── Palette editors ─────────────────────────────────────────────────────
		// `changed` fires every frame while a swatch is being dragged (live recolour);
		// `commit` fires once, on release, so the file write happens only when the edit
		// is finished - never per frame (see the no-per-frame-save-spam project rule).
		bool changed = false;
		bool commit = false;
		const auto swatch = [&](const char* label, ImVec4& color)
		{
			if (ImGui::ColorEdit3(label, &color.x, ImGuiColorEditFlags_NoInputs))
			{
				changed = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				commit = true;
			}
		};

		ImGui::Spacing();
		chrome::SectionTag("ACCENT");
		ImGui::Spacing();
		swatch("Accent (Primary)", m_theme.accent);
		swatch("Accent Hover", m_theme.accentHover);
		swatch("Accent Active", m_theme.accentActive);
		swatch("On Accent (text on fills)", m_theme.onAccent);

		ImGui::Spacing();
		chrome::SectionTag("SURFACES");
		ImGui::Spacing();
		swatch("Background", m_theme.background);
		swatch("Surface (panels)", m_theme.surface);
		swatch("Surface Elevated", m_theme.surfaceElevated);
		swatch("Border / hairline", m_theme.border);

		ImGui::Spacing();
		chrome::SectionTag("TEXT");
		ImGui::Spacing();
		swatch("Primary", m_theme.textPrimary);
		swatch("Secondary", m_theme.textSecondary);
		swatch("Faint", m_theme.textFaint);

		ImGui::Spacing();
		chrome::SectionTag("SEMANTIC");
		ImGui::Spacing();
		swatch("Success", m_theme.success);
		swatch("Warning", m_theme.warning);
		swatch("Error", m_theme.error);

		if (changed)
		{
			m_loaded = true;
			Apply(); // in-memory recolour for instant feedback while dragging
		}
		if (commit)
		{
			Persist(); // flush to EditorTheme.toml once, when the edit is released
		}

		ImGui::Spacing();
		chrome::AccentHairline(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0.22f);
		ImGui::Dummy(ImVec2(0.0f, 6.0f));
		if (chrome::OutlineButton(ICON_FA_ROTATE " Reset to Night Amber"))
		{
			m_theme = chrome::NightAmberTheme();
			m_loaded = false;
			Apply();
			std::error_code ec;
			std::filesystem::remove(ThemePath(), ec); // back to the shipped default on next launch
		}

		ImGui::End();
	}
} // namespace aether::editor
