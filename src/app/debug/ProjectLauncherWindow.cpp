#include "debug/ProjectLauncherWindow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
		using namespace aether::editor::chrome;

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

		// A project tile in the recents grid: a scene-preview thumbnail on top (or a
		// cube placeholder when the project has no <root>/.aether/preview.png yet),
		// then the name and path. Hover lifts the fill + adds corner brackets; a
		// project whose ProjectSettings.toml is gone renders muted with a MISSING tag.
		// Returns true when clicked (the caller opens it, which reports the error for
		// a missing project).
		bool DrawProjectCard(const EditorProjectContext& project, std::uint64_t previewTextureId, const std::string& modifiedLabel, bool selected, bool missing, const ImVec2& cardSize)
		{
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImGui::PushID(DisplayPath(project.root).c_str());
			const bool pressed = ImGui::InvisibleButton("##projectCard", cardSize);
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 end = Add(start, cardSize);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const bool lit = hovered && !missing;

			// Card body.
			drawList->AddRectFilled(start, end, ToU32(lit ? kPanelHi : kPanel), 4.0f);
			drawList->AddRect(start, end, ToU32(lit ? WithAlpha(kAccent, 0.5f) : kStroke), 4.0f, 0, 1.0f);
			if (selected && !missing)
			{
				drawList->AddRectFilled(start, ImVec2(end.x, start.y + 3.0f), ToU32(kAccent), 4.0f, ImDrawFlags_RoundCornersTop);
			}
			if (lit)
			{
				DrawCornerBrackets(drawList, Add(start, ImVec2(-3.0f, -3.0f)), Add(end, ImVec2(3.0f, 3.0f)), 12.0f, 2.0f, kAccent);
			}

			// Thumbnail region (top), 16:9.
			const float thumbHeight = std::floor(cardSize.x * 9.0f / 16.0f);
			const ImVec2 thumbMin = Add(start, ImVec2(1.0f, 1.0f));
			const ImVec2 thumbMax(end.x - 1.0f, start.y + thumbHeight);
			if (previewTextureId != 0 && !missing)
			{
				drawList->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(previewTextureId)), thumbMin, thumbMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255), 4.0f, ImDrawFlags_RoundCornersTop);
			}
			else
			{
				drawList->AddRectFilled(thumbMin, thumbMax, ToU32(kBg), 4.0f, ImDrawFlags_RoundCornersTop);
				const ImVec2 iconSize = MeasureSized(34.0f, ICON_FA_CUBE);
				const ImVec2 iconPos((thumbMin.x + thumbMax.x - iconSize.x) * 0.5f, (thumbMin.y + thumbMax.y - iconSize.y) * 0.5f);
				TextSized(drawList, 34.0f, iconPos, missing ? kFaint : WithAlpha(kAccent, 0.5f), ICON_FA_CUBE);
			}
			drawList->AddLine(ImVec2(thumbMin.x, thumbMax.y), ImVec2(thumbMax.x, thumbMax.y), ToU32(kStroke), 1.0f);

			// "Last edited" badge, pinned to the thumbnail's bottom-left over a subtle
			// scrim so it reads on both a bright scene preview and the dark placeholder.
			if (!modifiedLabel.empty() && !missing)
			{
				const ImVec2 badgeSize = MeasureSized(11.5f, modifiedLabel.c_str());
				const ImVec2 badgePos(thumbMin.x + 10.0f, thumbMax.y - badgeSize.y - 10.0f);
				drawList->AddRectFilled(Sub(badgePos, ImVec2(6.0f, 4.0f)), Add(badgePos, Add(badgeSize, ImVec2(6.0f, 4.0f))), ToU32(WithAlpha(kBg, 0.7f)), 2.0f);
				TextSized(drawList, 11.5f, badgePos, kMuted, modifiedLabel.c_str());
			}

			// Name + path below the thumbnail.
			const float textX = start.x + 12.0f;
			TextSized(drawList, 16.0f, ImVec2(textX, thumbMax.y + 10.0f), missing ? kMuted : kText, project.name.c_str());
			const std::string path = DisplayPath(project.root);
			ImGui::PushClipRect(ImVec2(textX, thumbMax.y + 32.0f), Sub(end, ImVec2(12.0f, 8.0f)), true);
			TextSized(drawList, 12.5f, ImVec2(textX, thumbMax.y + 33.0f), missing ? kFaint : kMuted, path.c_str());
			ImGui::PopClipRect();

			// Overlays on the thumbnail: MISSING tag, or an OPEN chip on hover.
			if (missing)
			{
				const char* tag = "MISSING";
				const ImVec2 tagSize = MeasureSized(11.0f, tag);
				const ImVec2 tp(thumbMax.x - tagSize.x - 16.0f, thumbMin.y + 12.0f);
				drawList->AddRectFilled(Sub(tp, ImVec2(6.0f, 4.0f)), Add(tp, Add(tagSize, ImVec2(6.0f, 4.0f))), ToU32(WithAlpha(kBg, 0.82f)), 2.0f);
				TextSized(drawList, 11.0f, tp, kError, tag);
				if (hovered)
				{
					ImGui::SetTooltip("ProjectSettings.toml no longer exists at\n%s", path.c_str());
				}
			}
			else if (hovered)
			{
				const char* hint = "OPEN";
				const ImVec2 hintSize = MeasureSized(12.0f, hint);
				const ImVec2 hp(thumbMax.x - hintSize.x - 16.0f, thumbMin.y + 12.0f);
				drawList->AddRectFilled(Sub(hp, ImVec2(8.0f, 5.0f)), Add(hp, Add(hintSize, ImVec2(8.0f, 5.0f))), ToU32(WithAlpha(kAccent, 0.9f)), 2.0f);
				TextSized(drawList, 12.0f, hp, kOnAccent, hint);
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
		// The hub FILLS its host window - the standalone Launcher's own OS window, or
		// the editor's main window when shown there as an overlay - and lays itself
		// out responsively: the content column caps + centers on ultra-wide windows,
		// and below a width breakpoint the actions panel and recents list stack into
		// a single column. It tracks resizes every frame, so there is no fixed design
		// size; kProjectLauncherDefault{Width,Height} (header) is only the standalone
		// Launcher's initial window size.
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);

		const ImGuiWindowFlags flags =
		        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
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
		const float height = windowMax.y - windowMin.y;

		// ── Responsive content band ─────────────────────────────────────────────
		// Side margins scale with the window; the content column caps at
		// kMaxContentWidth and centers itself on ultra-wide windows.
		constexpr float kMaxContentWidth = 1720.0f;
		const float marginX = std::clamp(width * 0.05f, 20.0f, 84.0f);
		const float contentWidth = std::min(width - marginX * 2.0f, kMaxContentWidth);
		const float contentX = windowMin.x + (width - contentWidth) * 0.5f;
		const ImVec2 contentMin(contentX, windowMin.y + std::clamp(height * 0.06f, 18.0f, 64.0f));
		const ImVec2 contentMax(contentX + contentWidth, windowMax.y - std::clamp(height * 0.05f, 16.0f, 56.0f));

		// Below this content width the two columns cannot both breathe: stack the
		// actions panel above the recents list instead.
		const bool singleColumn = contentWidth < 980.0f;

		// ── Wordmark (scales down with the window) ──────────────────────────────
		const float wordmarkSize = std::clamp(contentWidth * 0.032f, 28.0f, 42.0f);
		const float logoSize = wordmarkSize + 16.0f;
		constexpr float kLogoGap = 14.0f;
		const bool hasLogo = model.logoTextureId != 0;
		if (hasLogo)
		{
			drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(model.logoTextureId)), contentMin, Add(contentMin, ImVec2(logoSize, logoSize)), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ToU32(kAccent));
		}
		const ImVec2 wordmarkPos = Add(contentMin, ImVec2(hasLogo ? logoSize + kLogoGap : 0.0f, 0.0f));
		const ImVec2 aetherSize = MeasureSized(wordmarkSize, "AETHER");
		TextSized(drawList, wordmarkSize, wordmarkPos, kText, "AETHER");
		TextSized(drawList, wordmarkSize, Add(wordmarkPos, ImVec2(aetherSize.x, 0.0f)), kAccent, "CORE");
		const ImVec2 coreSize = MeasureSized(wordmarkSize, "CORE");
		// EDITOR tag: small outlined chip after the wordmark.
		{
			const ImVec2 tagTextSize = MeasureSized(12.0f, "EDITOR");
			const ImVec2 tagMin = Add(wordmarkPos, ImVec2(aetherSize.x + coreSize.x + 16.0f, 10.0f));
			const ImVec2 tagMax = Add(tagMin, Add(tagTextSize, ImVec2(16.0f, 10.0f)));
			drawList->AddRect(tagMin, tagMax, ToU32(WithAlpha(kAccent, 0.5f)), 2.0f, 0, 1.0f);
			TextSized(drawList, 12.0f, Add(tagMin, ImVec2(8.0f, 5.0f)), kAccentHi, "EDITOR");
		}
		// Accent underline + subtitle.
		drawList->AddRectFilled(Add(wordmarkPos, ImVec2(2.0f, wordmarkSize + 10.0f)), Add(wordmarkPos, ImVec2(58.0f, wordmarkSize + 13.0f)), ToU32(kAccent));
		TextSized(drawList, 14.5f, Add(wordmarkPos, ImVec2(2.0f, wordmarkSize + 24.0f)), kMuted, "Select a project to begin.");

		if (model.projectLoaded)
		{
			ImGui::SetCursorScreenPos(ImVec2(contentMax.x - 150.0f, contentMin.y + 4.0f));
			if (GhostButton(ICON_FA_XMARK "  Back to Editor", ImVec2(150.0f, 32.0f)) && actions.closeLauncher)
			{
				actions.closeLauncher();
			}
		}

		// ── Layout rects ────────────────────────────────────────────────────────
		const float columnsTop = contentMin.y + wordmarkSize + 62.0f;
		const float footerH = 30.0f;
		const float columnsBottom = contentMax.y - footerH;

		// Actions panel: right column normally; a centered column on narrow windows.
		const float rightWidth = singleColumn ? std::min(contentWidth, 520.0f) : std::clamp(contentWidth * 0.32f, 380.0f, 460.0f);
		const ImVec2 rightMin = singleColumn ? ImVec2(contentX + (contentWidth - rightWidth) * 0.5f, columnsTop) : ImVec2(contentMax.x - rightWidth, columnsTop);
		const ImVec2 rightMax(rightMin.x + rightWidth, columnsBottom);

		// ── Actions panel ───────────────────────────────────────────────────────
		// Drawn BEFORE the recents list: on narrow windows the list flows below the
		// panel, so its measured height decides where the list starts. The panel
		// hugs its content: widgets draw on channel 1 first, then the panel chrome
		// lands behind them on channel 0 once the height is known.
		drawList->ChannelsSplit(2);
		drawList->ChannelsSetCurrent(1);

		constexpr float kPad = 22.0f;
		float cursorY = rightMin.y + kPad;
		const float innerWidth = rightWidth - kPad * 2.0f;

		PushInputStyles();
		// Square browse buttons: width == the input-row height so the icon-only button
		// is a true square that still lines up with the field beside it.
		const float browseSize = ImGui::GetFrameHeight();

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
		ImGui::SetNextItemWidth(innerWidth - browseSize - 8.0f);
		bool openSubmitted = ImGui::InputTextWithHint("##openProjectPath", "Path to ProjectSettings.toml...", state.openPath.data(), state.openPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine(0.0f, 8.0f);
		if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseOpen", ImVec2(browseSize, browseSize)) && actions.browseProjectFile)
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
		ImGui::SetNextItemWidth(innerWidth - browseSize - 8.0f);
		createSubmitted = ImGui::InputTextWithHint("##newProjectPath", "Project folder...", state.newPath.data(), state.newPath.size(), ImGuiInputTextFlags_EnterReturnsTrue) || createSubmitted;
		ImGui::SameLine(0.0f, 8.0f);
		if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseNew", ImVec2(browseSize, browseSize)) && actions.browseFolder)
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

		// ── Recent projects ─────────────────────────────────────────────────────
		// Beside the panel normally; below it on narrow windows. Skipped entirely
		// when the window leaves no meaningful room (the list scrolls, but a
		// sliver-sized list is worse than none).
		const ImVec2 leftMin = singleColumn ? ImVec2(contentMin.x, panelMax.y + 30.0f) : ImVec2(contentMin.x, columnsTop);
		const ImVec2 leftMax = singleColumn ? ImVec2(contentMax.x, columnsBottom) : ImVec2(rightMin.x - 32.0f, columnsBottom);
		if (leftMax.y - leftMin.y > 96.0f && leftMax.x - leftMin.x > 220.0f)
		{
			const float gridWidth = leftMax.x - leftMin.x;
			SectionLabel(drawList, leftMin, "RECENT PROJECTS");
			ImGui::SetCursorScreenPos(Add(leftMin, ImVec2(0.0f, 30.0f)));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
			ImGui::BeginChild("##launcherRecentProjects", ImVec2(gridWidth, leftMax.y - leftMin.y - 30.0f), false);
			if (model.recentProjects.empty())
			{
				const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
				ImDrawList* childDrawList = ImGui::GetWindowDrawList();
				TextSized(childDrawList, 15.0f, Add(emptyPos, ImVec2(2.0f, 8.0f)), kFaint, "Nothing here yet.");
				TextSized(childDrawList, 14.0f, Add(emptyPos, ImVec2(2.0f, 34.0f)), kFaint, "Open an existing project or create a new one to get started.");
			}
			else
			{
				// Responsive card grid: fit as many ~kMinCardWidth cards across as the
				// child allows, then widen them to fill the row evenly. Cards keep a
				// 16:9 thumbnail, so the row height follows the card width.
				constexpr float kGridGap = 16.0f;
				constexpr float kMinCardWidth = 210.0f;
				const float avail = ImGui::GetContentRegionAvail().x;
				const int columns = std::max(1, static_cast<int>((avail + kGridGap) / (kMinCardWidth + kGridGap)));
				const float cardWidth = std::floor((avail - kGridGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
				const float cardHeight = std::floor(cardWidth * 9.0f / 16.0f) + 64.0f;
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kGridGap, kGridGap));
				for (std::size_t i = 0; i < model.recentProjects.size(); ++i)
				{
					const EditorProjectContext& project = model.recentProjects[i];
					if (i % static_cast<std::size_t>(columns) != 0)
					{
						ImGui::SameLine(0.0f, kGridGap);
					}
					const bool selected = model.currentProject != nullptr && model.currentProject->root == project.root;
					std::error_code existsEc;
					const bool missing = !std::filesystem::exists(project.root / "ProjectSettings.toml", existsEc);
					const std::uint64_t preview = model.previewTextureId ? model.previewTextureId(project) : 0;
					const std::string edited = model.modifiedLabel ? model.modifiedLabel(project) : std::string{};
					if (DrawProjectCard(project, preview, edited, selected, missing, ImVec2(cardWidth, cardHeight)) && actions.openProject)
					{
						actions.openProject(project.root);
					}
				}
				ImGui::PopStyleVar();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor(); // ChildBg
		}

		// ── Footer ──────────────────────────────────────────────────────────────
		TextSized(drawList, 13.0f, ImVec2(contentMin.x, contentMax.y - 16.0f), kFaint, "AetherCore Editor");

		ImGui::End();
	}
} // namespace aether::app
