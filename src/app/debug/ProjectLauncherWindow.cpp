#include "debug/ProjectLauncherWindow.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "Color.hpp"
#include "debug/Icons.hpp"
#include "debug/EditorChrome.hpp"

namespace aether::app
{
	namespace
	{
		// The palette IS the editor palette ("Night Amber", engine/Color.hpp). The
		// launcher defined this language; the shared primitives now live in
		// debug/EditorChrome.hpp so every panel adopting the look moves with it.
		using namespace chrome;

		// Launcher-specific aliases on top of the shared tokens (kError now comes
		// from chrome:: too - it is themeable).
		const ImVec4 kBgTop = kBg;
		const ImVec4 kBgBottom = kPanel; // gradient lifts into the panel tone
		const ImVec4 kBtnText = kOnAccent;

		[[nodiscard]] ImU32 ToU32(const ImVec4& color)
		{
			return U32(color);
		}

		[[nodiscard]] ImVec2 Add(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x + b.x, a.y + b.y);
		}

		[[nodiscard]] ImVec2 Sub(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x - b.x, a.y - b.y);
		}

		// WithAlpha comes from chrome::.

		[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		template<std::size_t N>
		void CopyToBuffer(std::array<char, N>& buffer, const std::filesystem::path& path)
		{
			std::snprintf(buffer.data(), buffer.size(), "%s", DisplayPath(path).c_str());
		}

		// TextSized / MeasureSized come from chrome::; brackets forward to the
		// shared primitive (the launcher's call sites predate the extraction).
		void DrawCornerBrackets(ImDrawList* drawList, const ImVec2 min, const ImVec2 max, const float arm, const float thickness, const ImVec4& color)
		{
			CornerBrackets(drawList, min, max, arm, thickness, color);
		}

		void DrawLauncherBackground(ImDrawList* drawList, const ImVec2 min, const ImVec2 max)
		{
			// Vertical near-black gradient.
			drawList->AddRectFilledMultiColor(min, max, ToU32(kBgTop), ToU32(kBgTop), ToU32(kBgBottom), ToU32(kBgBottom));

			// Fine grid, barely-there. Structure without noise.
			constexpr float kPitch = 56.0f;
			const ImU32 gridColor = ToU32(WithAlpha(kStroke, 0.16f));
			for (float x = min.x + kPitch; x < max.x; x += kPitch)
			{
				drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), gridColor, 1.0f);
			}
			for (float y = min.y + kPitch; y < max.y; y += kPitch)
			{
				drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), gridColor, 1.0f);
			}

			// Soft amber wash behind the header band, fading right.
			const float bandBottom = min.y + 200.0f;
			drawList->AddRectFilledMultiColor(min, ImVec2(min.x + (max.x - min.x) * 0.55f, bandBottom), ToU32(WithAlpha(kAccent, 0.045f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.03f)));

			// Bottom edge: 2px accent hairline fading out to the right.
			drawList->AddRectFilledMultiColor(ImVec2(min.x, max.y - 2.0f), max, ToU32(WithAlpha(kAccent, 0.85f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.85f)));

			// HUD stripe motif: three diagonal slashes in the top-right corner.
			const float slashBaseX = max.x - 96.0f;
			const float slashY = min.y + 44.0f;
			for (int i = 0; i < 3; ++i)
			{
				const float x = slashBaseX + static_cast<float>(i) * 18.0f;
				drawList->AddLine(ImVec2(x + 14.0f, slashY), ImVec2(x, slashY + 26.0f), ToU32(WithAlpha(kAccent, 0.65f - static_cast<float>(i) * 0.2f)), 3.0f);
			}
		}

		// Micro section label: small amber tick + spaced uppercase text.
		void SectionLabel(ImDrawList* drawList, ImVec2 pos, const char* label)
		{
			drawList->AddRectFilled(pos, Add(pos, ImVec2(3.0f, 12.0f)), ToU32(kAccent));
			TextSized(drawList, 13.0f, Add(pos, ImVec2(10.0f, -1.0f)), kMuted, label);
		}

		// PrimaryButton / OutlineButton / GhostButton come from chrome::.

		bool DrawRecentProjectRow(const EditorProjectContext& project, bool selected, bool missing)
		{
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			constexpr float kRowHeight = 72.0f;
			ImGui::PushID(DisplayPath(project.root).c_str());
			const bool pressed = ImGui::InvisibleButton("##recentProject", ImVec2(width, kRowHeight));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 end = Add(start, ImVec2(width, kRowHeight));
			ImDrawList* drawList = ImGui::GetWindowDrawList();

			// Card: flat dark fill, hairline border; hover lifts the fill and adds
			// the corner brackets; selected keeps a solid left accent bar. A project
			// whose ProjectSettings.toml no longer exists renders muted, with no
			// hover affordances - clicking it still reports the error.
			drawList->AddRectFilled(start, end, ToU32(hovered && !missing ? kPanelHi : kPanel), 3.0f);
			drawList->AddRect(start, end, ToU32(hovered && !missing ? WithAlpha(kAccent, 0.45f) : kStroke), 3.0f, 0, 1.0f);
			if (selected && !missing)
			{
				drawList->AddRectFilled(start, Add(start, ImVec2(3.0f, kRowHeight)), ToU32(kAccent), 2.0f);
			}
			if (hovered && !missing)
			{
				DrawCornerBrackets(drawList, Add(start, ImVec2(-3.0f, -3.0f)), Add(end, ImVec2(3.0f, 3.0f)), 10.0f, 2.0f, kAccent);
			}

			// Icon chip.
			const bool lit = (hovered || selected) && !missing;
			const ImVec2 chipMin = Add(start, ImVec2(16.0f, 18.0f));
			const ImVec2 chipMax = Add(chipMin, ImVec2(36.0f, 36.0f));
			const ImVec4& chipTint = missing ? kFaint : kAccent;
			drawList->AddRectFilled(chipMin, chipMax, ToU32(WithAlpha(chipTint, lit ? 0.18f : 0.10f)), 3.0f);
			drawList->AddRect(chipMin, chipMax, ToU32(WithAlpha(chipTint, lit ? 0.7f : 0.35f)), 3.0f, 0, 1.0f);
			TextSized(drawList, 16.0f, Add(chipMin, ImVec2(9.0f, 10.0f)), missing ? kFaint : (lit ? kAccentHi : kAccentDim), ICON_FA_CUBE);

			// Name + path.
			TextSized(drawList, 17.0f, Add(start, ImVec2(66.0f, 14.0f)), missing ? kMuted : kText, project.name.c_str());
			const std::string path = DisplayPath(project.root);
			ImGui::PushClipRect(Add(start, ImVec2(66.0f, 38.0f)), Sub(end, ImVec2(86.0f, 8.0f)), true);
			TextSized(drawList, 13.5f, Add(start, ImVec2(66.0f, 40.0f)), missing ? kFaint : kMuted, path.c_str());
			ImGui::PopClipRect();

			// Right-edge affordance: OPEN + chevron on hover, or a persistent
			// MISSING tag when the project is gone from disk.
			if (missing)
			{
				const char* tag = "MISSING";
				const ImVec2 tagSize = MeasureSized(12.0f, tag);
				TextSized(drawList, 12.0f, ImVec2(end.x - tagSize.x - 18.0f, start.y + (kRowHeight - tagSize.y) * 0.5f), kError, tag);
				if (hovered)
				{
					ImGui::SetTooltip("ProjectSettings.toml no longer exists at\n%s", path.c_str());
				}
			}
			else if (hovered)
			{
				const char* hint = "OPEN";
				const ImVec2 hintSize = MeasureSized(13.0f, hint);
				TextSized(drawList, 13.0f, ImVec2(end.x - hintSize.x - 34.0f, start.y + (kRowHeight - hintSize.y) * 0.5f), kAccentHi, hint);
				TextSized(drawList, 15.0f, ImVec2(end.x - 24.0f, start.y + (kRowHeight - 15.0f) * 0.5f - 1.0f), kAccentHi, ">");
				ImGui::SetTooltip("%s", path.c_str());
			}
			ImGui::PopID();
			return pressed;
		}

		void PushInputStyles()
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 9.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.043f, 0.047f, 0.055f, 1.0f});
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.055f, 0.059f, 0.071f, 1.0f});
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{0.055f, 0.059f, 0.071f, 1.0f});
			ImGui::PushStyleColor(ImGuiCol_Border, kStroke);
			ImGui::PushStyleColor(ImGuiCol_Text, kText);
			ImGui::PushStyleColor(ImGuiCol_TextDisabled, kFaint);
			ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
		}

		void PopInputStyles()
		{
			ImGui::PopStyleColor(7);
			ImGui::PopStyleVar(3);
		}
	} // namespace

	void ProjectLauncherWindow::Draw(ProjectLauncherWindowState& state, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowActions& actions)
	{
		// The launcher is its own OS window: force it into a dedicated platform
		// viewport (never auto-merged into the editor's main window) at the fixed size
		// its layout is tuned for, centered on the primary monitor on first show.
		constexpr int kLauncherWidth = 1450;
		constexpr int kLauncherHeight = 880;

		ImGuiWindowClass ownViewport;
		ownViewport.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		ImGui::SetNextWindowClass(&ownViewport);

		const ImVec2 launcherSize(static_cast<float>(kLauncherWidth), static_cast<float>(kLauncherHeight));
		ImGui::SetNextWindowSize(launcherSize, ImGuiCond_Once);
		const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		if (!platformIO.Monitors.empty())
		{
			const ImGuiPlatformMonitor& monitor = platformIO.Monitors[0];
			const ImVec2 center(monitor.WorkPos.x + (monitor.WorkSize.x - launcherSize.x) * 0.5f,
			                    monitor.WorkPos.y + (monitor.WorkSize.y - launcherSize.y) * 0.5f);
			ImGui::SetNextWindowPos(center, ImGuiCond_Once);
		}

		const ImGuiWindowFlags flags =
		        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("AetherCoreProjectLauncher", nullptr, flags);
		ImGui::PopStyleVar(3);

		const ImVec2 windowMin = ImGui::GetWindowPos();
		const ImVec2 windowMax = Add(windowMin, ImGui::GetWindowSize());
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		DrawLauncherBackground(drawList, windowMin, windowMax);

		const float width = windowMax.x - windowMin.x;
		const float margin = std::clamp(width * 0.05f, 40.0f, 84.0f);
		const ImVec2 contentMin = Add(windowMin, ImVec2(margin, margin * 0.8f));
		const ImVec2 contentMax = Sub(windowMax, ImVec2(margin, margin * 0.7f));

		// ── Wordmark ────────────────────────────────────────────────────────────
		constexpr float kWordmarkSize = 42.0f;
		constexpr float kLogoSize = 58.0f;
		constexpr float kLogoGap = 14.0f;
		const bool hasLogo = model.logoTextureId != 0;
		if (hasLogo)
		{
			drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(model.logoTextureId)), contentMin, Add(contentMin, ImVec2(kLogoSize, kLogoSize)), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ToU32(kAccent));
		}
		const ImVec2 wordmarkPos = Add(contentMin, ImVec2(hasLogo ? kLogoSize + kLogoGap : 0.0f, 0.0f));
		const ImVec2 aetherSize = MeasureSized(kWordmarkSize, "AETHER");
		TextSized(drawList, kWordmarkSize, wordmarkPos, kText, "AETHER");
		TextSized(drawList, kWordmarkSize, Add(wordmarkPos, ImVec2(aetherSize.x, 0.0f)), kAccent, "CORE");
		const ImVec2 coreSize = MeasureSized(kWordmarkSize, "CORE");
		// EDITOR tag: small outlined chip after the wordmark.
		{
			const ImVec2 tagTextSize = MeasureSized(12.0f, "EDITOR");
			const ImVec2 tagMin = Add(wordmarkPos, ImVec2(aetherSize.x + coreSize.x + 16.0f, 10.0f));
			const ImVec2 tagMax = Add(tagMin, Add(tagTextSize, ImVec2(16.0f, 10.0f)));
			drawList->AddRect(tagMin, tagMax, ToU32(WithAlpha(kAccent, 0.5f)), 2.0f, 0, 1.0f);
			TextSized(drawList, 12.0f, Add(tagMin, ImVec2(8.0f, 5.0f)), kAccentHi, "EDITOR");
		}
		// Accent underline + subtitle.
		drawList->AddRectFilled(Add(wordmarkPos, ImVec2(2.0f, kWordmarkSize + 10.0f)), Add(wordmarkPos, ImVec2(58.0f, kWordmarkSize + 13.0f)), ToU32(kAccent));
		TextSized(drawList, 14.5f, Add(wordmarkPos, ImVec2(2.0f, kWordmarkSize + 24.0f)), kMuted, "Select a project to begin.");

		if (model.projectLoaded)
		{
			ImGui::SetCursorScreenPos(ImVec2(contentMax.x - 150.0f, contentMin.y + 4.0f));
			if (GhostButton(ICON_FA_XMARK "  Back to Editor", ImVec2(150.0f, 32.0f)) && actions.closeLauncher)
			{
				actions.closeLauncher();
			}
		}

		// ── Columns ─────────────────────────────────────────────────────────────
		const float columnsTop = contentMin.y + kWordmarkSize + 62.0f;
		const float footerH = 30.0f;
		const float rightWidth = std::clamp(width * 0.30f, 400.0f, 460.0f);
		constexpr float kGap = 32.0f;
		const ImVec2 leftMin{contentMin.x, columnsTop};
		const ImVec2 leftMax{contentMax.x - rightWidth - kGap, contentMax.y - footerH};
		const ImVec2 rightMin{contentMax.x - rightWidth, columnsTop};
		const ImVec2 rightMax{contentMax.x, contentMax.y - footerH};

		// ── Left: recent projects ───────────────────────────────────────────────
		// Cap the list width so rows stay readable on wide displays.
		const float listWidth = std::min(leftMax.x - leftMin.x, 860.0f);
		SectionLabel(drawList, leftMin, "RECENT PROJECTS");
		ImGui::SetCursorScreenPos(Add(leftMin, ImVec2(0.0f, 30.0f)));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
		ImGui::BeginChild("##launcherRecentProjects", ImVec2(listWidth, leftMax.y - leftMin.y - 30.0f), false);
		if (model.recentProjects.empty())
		{
			const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
			ImDrawList* childDrawList = ImGui::GetWindowDrawList();
			TextSized(childDrawList, 15.0f, Add(emptyPos, ImVec2(2.0f, 8.0f)), kFaint, "Nothing here yet.");
			TextSized(childDrawList, 14.0f, Add(emptyPos, ImVec2(2.0f, 34.0f)), kFaint, "Open an existing project or create a new one to get started.");
		}
		for (const EditorProjectContext& project: model.recentProjects)
		{
			const bool selected = model.currentProject != nullptr && model.currentProject->root == project.root;
			std::error_code existsEc;
			const bool missing = !std::filesystem::exists(project.root / "ProjectSettings.toml", existsEc);
			if (DrawRecentProjectRow(project, selected, missing) && actions.openProject)
			{
				actions.openProject(project.root);
			}
			ImGui::Dummy(ImVec2(1.0f, 10.0f));
		}
		ImGui::EndChild();
		ImGui::PopStyleColor(); // ChildBg

		// ── Right: actions panel ────────────────────────────────────────────────
		// The panel hugs its content: widgets draw on channel 1 first, then the
		// panel chrome lands behind them on channel 0 once the height is known.
		drawList->ChannelsSplit(2);
		drawList->ChannelsSetCurrent(1);

		constexpr float kPad = 22.0f;
		float cursorY = rightMin.y + kPad;
		const float innerWidth = rightWidth - kPad * 2.0f;

		PushInputStyles();

		// Continue (only when a current project exists).
		if (model.hasCurrentProject && model.currentProject != nullptr)
		{
			SectionLabel(drawList, ImVec2(rightMin.x + kPad, cursorY), "CONTINUE");
			cursorY += 28.0f;
			TextSized(drawList, 17.0f, ImVec2(rightMin.x + kPad, cursorY), kText, model.currentProject->name.c_str());
			cursorY += 24.0f;
			ImGui::PushClipRect(ImVec2(rightMin.x + kPad, cursorY), ImVec2(rightMax.x - kPad, cursorY + 18.0f), true);
			TextSized(drawList, 13.0f, ImVec2(rightMin.x + kPad, cursorY), kMuted, DisplayPath(model.currentProject->root).c_str());
			ImGui::PopClipRect();
			cursorY += 28.0f;
			ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
			if (PrimaryButton(ICON_FA_PLAY "  Continue", ImVec2(innerWidth, 40.0f)) && actions.openProject)
			{
				actions.openProject(model.currentProject->root);
			}
			cursorY += 54.0f;
			drawList->AddLine(ImVec2(rightMin.x + kPad, cursorY), ImVec2(rightMax.x - kPad, cursorY), ToU32(kStroke), 1.0f);
			cursorY += 20.0f;
		}

		// Open existing. Enter in the path field submits; the button disables
		// until there is a path, so requirements read before the error does.
		SectionLabel(drawList, ImVec2(rightMin.x + kPad, cursorY), "OPEN PROJECT");
		cursorY += 28.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		ImGui::SetNextItemWidth(innerWidth - 44.0f);
		bool openSubmitted = ImGui::InputTextWithHint("##openProjectPath", "Path to ProjectSettings.toml...", state.openPath.data(), state.openPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine(0.0f, 8.0f);
		if (OutlineButton(ICON_FA_FOLDER_OPEN "##browseOpen", ImVec2(36.0f, 0.0f)) && actions.browseProjectFile)
		{
			if (const auto file = actions.browseProjectFile())
			{
				CopyToBuffer(state.openPath, *file);
			}
		}
		cursorY += 44.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		const bool openPathEmpty = state.openPath[0] == '\0';
		ImGui::BeginDisabled(openPathEmpty);
		openSubmitted = OutlineButton(ICON_FA_FOLDER_OPEN "  Open", ImVec2(innerWidth, 38.0f)) || openSubmitted;
		ImGui::EndDisabled();
		if (openSubmitted && !openPathEmpty && actions.openProject)
		{
			actions.openProject(std::filesystem::path(state.openPath.data()));
		}
		cursorY += 56.0f;
		drawList->AddLine(ImVec2(rightMin.x + kPad, cursorY), ImVec2(rightMax.x - kPad, cursorY), ToU32(kStroke), 1.0f);
		cursorY += 20.0f;

		// Create new. Enter in either field submits; Create disables until both
		// the name and folder are present.
		SectionLabel(drawList, ImVec2(rightMin.x + kPad, cursorY), "NEW PROJECT");
		cursorY += 28.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		ImGui::SetNextItemWidth(innerWidth);
		bool createSubmitted = ImGui::InputTextWithHint("##newProjectName", "Project name...", state.newName.data(), state.newName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		cursorY += 44.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		ImGui::SetNextItemWidth(innerWidth - 44.0f);
		createSubmitted = ImGui::InputTextWithHint("##newProjectPath", "Project folder...", state.newPath.data(), state.newPath.size(), ImGuiInputTextFlags_EnterReturnsTrue) || createSubmitted;
		ImGui::SameLine(0.0f, 8.0f);
		if (OutlineButton(ICON_FA_FOLDER_OPEN "##browseNew", ImVec2(36.0f, 0.0f)) && actions.browseFolder)
		{
			if (const auto folder = actions.browseFolder())
			{
				CopyToBuffer(state.newPath, *folder);
			}
		}
		cursorY += 44.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		const bool createIncomplete = state.newName[0] == '\0' || state.newPath[0] == '\0';
		ImGui::BeginDisabled(createIncomplete);
		createSubmitted = PrimaryButton(ICON_FA_PLUS "  Create", ImVec2(innerWidth, 38.0f)) || createSubmitted;
		ImGui::EndDisabled();
		if (createSubmitted && !createIncomplete && actions.createProject)
		{
			actions.createProject(std::filesystem::path(state.newPath.data()), state.newName.data());
		}
		cursorY += 54.0f;

		// Error (if any), a hairline, then the startup toggle - all inline so the
		// panel can hug its content.
		if (!state.error.empty())
		{
			ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::PushTextWrapPos(rightMax.x - kPad);
			ImGui::TextWrapped("%s", state.error.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			cursorY = ImGui::GetItemRectMax().y + 12.0f;
		}

		drawList->AddLine(ImVec2(rightMin.x + kPad, cursorY), ImVec2(rightMax.x - kPad, cursorY), ToU32(kStroke), 1.0f);
		cursorY += 16.0f;
		ImGui::SetCursorScreenPos(ImVec2(rightMin.x + kPad, cursorY));
		ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		if (ImGui::Checkbox("Open last project on startup", &state.openLastProject) && actions.saveSettings)
		{
			actions.saveSettings();
		}
		ImGui::PopStyleColor();
		cursorY = ImGui::GetItemRectMax().y + kPad;

		PopInputStyles();

		// Panel chrome behind the content, now that the height is known.
		const ImVec2 panelMax{rightMax.x, std::min(cursorY, rightMax.y)};
		drawList->ChannelsSetCurrent(0);
		drawList->AddRectFilled(rightMin, panelMax, ToU32(kPanel), 4.0f);
		drawList->AddRect(rightMin, panelMax, ToU32(kStroke), 4.0f, 0, 1.0f);
		drawList->AddRectFilled(rightMin, ImVec2(panelMax.x, rightMin.y + 2.0f), ToU32(WithAlpha(kAccent, 0.9f)), 4.0f, ImDrawFlags_RoundCornersTop);
		drawList->ChannelsMerge();

		// ── Footer ──────────────────────────────────────────────────────────────
		TextSized(drawList, 13.0f, ImVec2(contentMin.x, contentMax.y - 16.0f), kFaint, "AetherCore Editor");

		ImGui::End();
	}
} // namespace aether::app
