#include "debug/ProjectLauncherWindow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include <imgui.h>

#include "Color.hpp"
#include "debug/Icons.hpp"
#include "debug/EditorChrome.hpp"

namespace aether::app
{
	namespace
	{
		using namespace aether::editor::chrome;

		const ImVec4 kBgTop = kBg;
		const ImVec4 kBgBottom = kPanel;

		constexpr float kBaseFontPx = 15.0f;

		struct Px
		{
			float scale = 1.0f;

			[[nodiscard]] float operator()(const float v) const
			{
				return v * scale;
			}

			[[nodiscard]] ImVec2 operator()(const float x, const float y) const
			{
				return ImVec2(x * scale, y * scale);
			}
		};

		[[nodiscard]] Px MakeScale()
		{
			return Px{ImGui::GetFontSize() / kBaseFontPx};
		}

		// Keep design geometry beside the painter that owns it. Px is the only shared
		// layout primitive: it converts the readable design values below to UI scale.

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

		[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		template<std::size_t N>
		void CopyToBuffer(std::array<char, N>& buffer, const std::filesystem::path& path)
		{
			std::snprintf(buffer.data(), buffer.size(), "%s", DisplayPath(path).c_str());
		}

		void ScrimBehind(ImDrawList* drawList, const ImVec2 textPos, const ImVec2 textSize, const ImVec2 pad, const ImVec4& fill, const float corner)
		{
			drawList->AddRectFilled(Sub(textPos, pad), Add(textPos, Add(textSize, pad)), ToU32(fill), corner);
		}

		void SectionLabel(ImDrawList* drawList, const ImVec2 pos, const char* label, const Px& dp)
		{
			drawList->AddRectFilled(pos, Add(pos, dp(3.0f, 12.0f)), ToU32(kAccent));
			TextSized(drawList, dp(13.0f), Add(pos, dp(10.0f, -1.0f)), kMuted, label);
		}

		bool DrawProjectCard(const EditorProjectContext& project, std::uint64_t previewTextureId, const std::string& modifiedLabel, bool selected, bool missing, const ImVec2& cardSize, const Px& dp)
		{
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImGui::PushID(DisplayPath(project.root).c_str());
			const bool pressed = ImGui::InvisibleButton("##projectCard", cardSize);
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 end = Add(start, cardSize);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const bool lit = hovered && !missing;
			const float corner = dp(4.0f);
			const float thumbnailInset = dp(1.0f);
			const float textInset = dp(12.0f);
			const ImVec2 chipPadding = dp(6.0f, 4.0f);

			drawList->AddRectFilled(start, end, ToU32(lit ? kPanelHi : kPanel), corner);
			drawList->AddRect(start, end, ToU32(lit ? WithAlpha(kAccent, 0.5f) : kStroke), corner, 0, dp(1.0f));
			if (selected && !missing)
			{
				drawList->AddRectFilled(start, ImVec2(end.x, start.y + dp(3.0f)), ToU32(kAccent), corner, ImDrawFlags_RoundCornersTop);
			}
			if (lit)
			{
				const float bleed = dp(3.0f);
				CornerBrackets(drawList, Add(start, ImVec2(-bleed, -bleed)), Add(end, ImVec2(bleed, bleed)), dp(12.0f), dp(2.0f), kAccent);
			}

			const float thumbHeight = std::floor(cardSize.x * 9.0f / 16.0f);
			const ImVec2 thumbMin = Add(start, ImVec2(thumbnailInset, thumbnailInset));
			const ImVec2 thumbMax(end.x - thumbnailInset, start.y + thumbHeight);
			if (previewTextureId != 0 && !missing)
			{
				drawList->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(previewTextureId)), thumbMin, thumbMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255), corner, ImDrawFlags_RoundCornersTop);
			}
			else
			{
				drawList->AddRectFilled(thumbMin, thumbMax, ToU32(kBg), corner, ImDrawFlags_RoundCornersTop);
				const float iconFontSize = dp(34.0f);
				const ImVec2 iconSize = MeasureSized(iconFontSize, ICON_FA_CUBE);
				const ImVec2 iconPos((thumbMin.x + thumbMax.x - iconSize.x) * 0.5f, (thumbMin.y + thumbMax.y - iconSize.y) * 0.5f);
				TextSized(drawList, iconFontSize, iconPos, missing ? kFaint : WithAlpha(kAccent, 0.5f), ICON_FA_CUBE);
			}
			drawList->AddLine(ImVec2(thumbMin.x, thumbMax.y), ImVec2(thumbMax.x, thumbMax.y), ToU32(kStroke), 1.0f);

			if (!modifiedLabel.empty() && !missing)
			{
				const ImVec2 badgeSize = MeasureSized(dp(12.0f), modifiedLabel.c_str());
				const ImVec2 badgePos(thumbMin.x + dp(10.0f), thumbMax.y - badgeSize.y - dp(10.0f));
				ScrimBehind(drawList, badgePos, badgeSize, chipPadding, WithAlpha(kBg, 0.7f), dp(2.0f));
				TextSized(drawList, dp(12.0f), badgePos, kMuted, modifiedLabel.c_str());
			}

			const float textX = start.x + textInset;
			TextSized(drawList, dp(16.0f), ImVec2(textX, thumbMax.y + dp(10.0f)), missing ? kMuted : kText, project.name.c_str());
			const std::string path = DisplayPath(project.root);
			ImGui::PushClipRect(ImVec2(textX, thumbMax.y + dp(32.0f)), Sub(end, ImVec2(textInset, dp(8.0f))), true);
			TextSized(drawList, dp(13.0f), ImVec2(textX, thumbMax.y + dp(33.0f)), missing ? kFaint : kMuted, path.c_str());
			ImGui::PopClipRect();

			if (missing)
			{
				const char* tag = "MISSING";
				const ImVec2 tagSize = MeasureSized(dp(11.0f), tag);
				const ImVec2 tp(thumbMax.x - tagSize.x - dp(16.0f), thumbMin.y + dp(12.0f));
				ScrimBehind(drawList, tp, tagSize, chipPadding, WithAlpha(kBg, 0.82f), dp(2.0f));
				TextSized(drawList, dp(11.0f), tp, kError, tag);
				if (hovered)
				{
					ImGui::SetTooltip("ProjectSettings.toml no longer exists at\n%s", path.c_str());
				}
			}
			else if (hovered)
			{
				const char* hint = "OPEN";
				const ImVec2 hintSize = MeasureSized(dp(12.0f), hint);
				const ImVec2 hp(thumbMax.x - hintSize.x - dp(16.0f), thumbMin.y + dp(12.0f));
				ScrimBehind(drawList, hp, hintSize, dp(8.0f, 5.0f), WithAlpha(kAccent, 0.9f), dp(2.0f));
				TextSized(drawList, dp(12.0f), hp, kOnAccent, hint);
				ImGui::SetTooltip("%s", path.c_str());
			}
			ImGui::PopID();
			return pressed;
		}

		void PushInputStyles(const Px& dp)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, dp(3.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, dp(12.0f, 9.0f));
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

		struct Frame
		{
			ImDrawList* drawList = nullptr;
			Px dp;
			ImVec2 contentMin, contentMax;
			float contentX = 0.0f, contentWidth = 0.0f;
			float wordmarkSize = 0.0f;
			float gridTop = 0.0f, gridBottom = 0.0f;
		};

		void PaintBackground(const Frame& f, const ImVec2 min, const ImVec2 max)
		{
			ImDrawList* dl = f.drawList;
			const Px& dp = f.dp;

			dl->AddRectFilledMultiColor(min, max, ToU32(kBgTop), ToU32(kBgTop), ToU32(kBgBottom), ToU32(kBgBottom));

			const float pitch = dp(56.0f);
			const ImU32 gridColor = ToU32(WithAlpha(kStroke, 0.16f));
			for (float x = min.x + pitch; x < max.x; x += pitch)
			{
				dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), gridColor, 1.0f);
			}
			for (float y = min.y + pitch; y < max.y; y += pitch)
			{
				dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), gridColor, 1.0f);
			}

			const ImVec2 washMax(min.x + (max.x - min.x) * 0.55f, min.y + dp(240.0f));
			dl->AddRectFilledMultiColor(min, washMax, ToU32(WithAlpha(kAccent, 0.05f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)));

			dl->AddRectFilledMultiColor(ImVec2(min.x, max.y - dp(2.0f)), max, ToU32(WithAlpha(kAccent, 0.85f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.85f)));

			const ImVec2 slashOrigin(max.x - dp(96.0f), min.y + dp(44.0f));
			for (int i = 0; i < 3; ++i)
			{
				const float x = slashOrigin.x + static_cast<float>(i) * dp(18.0f);
				dl->AddLine(ImVec2(x + dp(14.0f), slashOrigin.y), ImVec2(x, slashOrigin.y + dp(26.0f)), ToU32(WithAlpha(kAccent, 0.65f - static_cast<float>(i) * 0.2f)), dp(3.0f));
			}
		}

		void PaintHeader(const Frame& f, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowState& state)
		{
			ImDrawList* dl = f.drawList;
			const Px& dp = f.dp;
			const float wordmark = f.wordmarkSize;

			const bool hasLogo = model.logoTextureId != 0;
			const float logoSize = wordmark + dp(16.0f);
			if (hasLogo)
			{
				dl->AddImage(ImTextureRef(static_cast<ImTextureID>(model.logoTextureId)), f.contentMin, Add(f.contentMin, ImVec2(logoSize, logoSize)), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ToU32(kAccent));
			}

			const ImVec2 wordmarkPos = Add(f.contentMin, ImVec2(hasLogo ? logoSize + dp(14.0f) : 0.0f, 0.0f));
			const ImVec2 aetherSize = MeasureSized(wordmark, "AETHER");
			TextSized(dl, wordmark, wordmarkPos, kText, "AETHER");
			TextSized(dl, wordmark, Add(wordmarkPos, ImVec2(aetherSize.x, 0.0f)), kAccent, "CORE");
			const ImVec2 coreSize = MeasureSized(wordmark, "CORE");

			const ImVec2 tagPadding = dp(8.0f, 5.0f);
			const ImVec2 tagText = MeasureSized(dp(12.0f), "EDITOR");
			const ImVec2 tagMin = Add(wordmarkPos, ImVec2(aetherSize.x + coreSize.x + dp(16.0f), dp(10.0f)));
			const ImVec2 tagMax = Add(tagMin, Add(tagText, ImVec2(tagPadding.x * 2.0f, tagPadding.y * 2.0f)));
			dl->AddRect(tagMin, tagMax, ToU32(WithAlpha(kAccent, 0.5f)), dp(2.0f), 0, dp(1.0f));
			TextSized(dl, dp(12.0f), Add(tagMin, tagPadding), kAccentHi, "EDITOR");

			const ImVec2 underlineMin = Add(wordmarkPos, ImVec2(dp(2.0f), wordmark + dp(10.0f)));
			dl->AddRectFilled(underlineMin, Add(underlineMin, dp(56.0f, 3.0f)), ToU32(kAccent));
			const char* subtitle = state.launching ? "Opening project in the editor…" : "Select a project to begin.";
			const ImVec4 subtitleColor = state.launching ? kAccentHi : kMuted;
			TextSized(dl, dp(14.5f), Add(wordmarkPos, ImVec2(dp(2.0f), wordmark + dp(24.0f))), subtitleColor, subtitle);
		}

		void DrawProjectDialog(ProjectLauncherWindowState& state, const ProjectLauncherWindowActions& actions, const Frame& f)
		{
			if (state.dialog == ProjectLauncherDialog::None)
			{
				return;
			}

			const Px& dp = f.dp;
			const bool opening = state.dialog == ProjectLauncherDialog::Open;
			const char* popupName = opening ? "Open Project" : "Create Project";
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			const float viewportInset = dp(20.0f);
			const float viewportWidth = std::max(1.0f, viewport->WorkSize.x - viewportInset * 2.0f);
			const float dialogWidth = std::min(f.contentWidth * 0.5f, viewportWidth);
			ImGui::OpenPopup(popupName);
			ImGui::SetNextWindowSizeConstraints(ImVec2(dialogWidth, 0.0f), ImVec2(dialogWidth, std::numeric_limits<float>::max()));
			if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			{
				return;
			}

			PushInputStyles(dp);
			const float width = ImGui::GetContentRegionAvail().x;
			const float browseSize = ImGui::GetFrameHeight();
			const float controlGap = dp(8.0f);
			bool submit = false;
			if (opening)
			{
				ImGui::TextUnformatted("Open an existing AetherCore project");
				ImGui::TextDisabled("Choose its ProjectSettings.toml file.");
				ImGui::Spacing();
				ImGui::SetNextItemWidth(width - browseSize - controlGap);
				submit = ImGui::InputTextWithHint("##openProjectPath", "Path to ProjectSettings.toml...", state.openPath.data(), state.openPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine(0.0f, controlGap);
				if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseOpen", ImVec2(browseSize, browseSize)) && actions.browseProjectFile)
				{
					if (const auto file = actions.browseProjectFile())
					{
						CopyToBuffer(state.openPath, *file);
					}
				}
			}
			else
			{
				ImGui::TextUnformatted("Create a new AetherCore project");
				ImGui::TextDisabled("Choose a name and an empty project folder.");
				ImGui::Spacing();
				ImGui::SetNextItemWidth(width);
				submit = ImGui::InputTextWithHint("##newProjectName", "Project name...", state.newName.data(), state.newName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SetNextItemWidth(width - browseSize - controlGap);
				submit = ImGui::InputTextWithHint("##newProjectPath", "Project folder...", state.newPath.data(), state.newPath.size(), ImGuiInputTextFlags_EnterReturnsTrue) || submit;
				ImGui::SameLine(0.0f, controlGap);
				if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseNew", ImVec2(browseSize, browseSize)) && actions.browseFolder)
				{
					if (const auto folder = actions.browseFolder())
					{
						CopyToBuffer(state.newPath, *folder);
					}
				}
			}

			if (!state.error.empty())
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, kError);
				ImGui::TextWrapped("%s", state.error.c_str());
				ImGui::PopStyleColor();
			}

			ImGui::Spacing();
			const bool incomplete = opening ? state.openPath[0] == '\0' : state.newName[0] == '\0' || state.newPath[0] == '\0';
			const float cancelWidth = dp(100.0f);
			const float actionWidth = dp(108.0f);
			const float buttonHeight = dp(38.0f);
			ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - cancelWidth - controlGap - actionWidth);
			if (OutlineButton("Cancel", ImVec2(cancelWidth, buttonHeight)))
			{
				state.dialog = ProjectLauncherDialog::None;
				state.error.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine(0.0f, controlGap);
			ImGui::BeginDisabled(incomplete);
			const char* actionLabel = opening ? ICON_FA_FOLDER_OPEN "  Open" : ICON_FA_PLUS "  Create";
			submit = PrimaryButton(actionLabel, ImVec2(actionWidth, buttonHeight)) || submit;
			ImGui::EndDisabled();
			if (submit && !incomplete)
			{
				if (opening && actions.openProject)
				{
					actions.openProject(std::filesystem::path(state.openPath.data()));
				}
				else if (!opening && actions.createProject)
				{
					actions.createProject(std::filesystem::path(state.newPath.data()), state.newName.data());
				}
				state.dialog = ProjectLauncherDialog::None;
				ImGui::CloseCurrentPopup();
			}
			PopInputStyles();
			ImGui::EndPopup();
		}

		void PaintRecentsGrid(const Frame& f, ProjectLauncherWindowState& state, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowActions& actions)
		{
			ImDrawList* dl = f.drawList;
			const Px& dp = f.dp;

			const ImVec2 leftMin(f.contentMin.x, f.gridTop);
			const ImVec2 leftMax(f.contentMax.x, f.gridBottom);
			if (leftMax.y - leftMin.y <= dp(96.0f) || leftMax.x - leftMin.x <= dp(220.0f))
			{
				return;
			}

			const float gridWidth = leftMax.x - leftMin.x;
			SectionLabel(dl, leftMin, "RECENT PROJECTS", dp);
			const float newWidth = dp(132.0f);
			const float openWidth = dp(112.0f);
			const float actionGap = dp(8.0f);
			const float buttonHeight = dp(38.0f);
			ImGui::SetCursorScreenPos(ImVec2(leftMax.x - newWidth - actionGap - openWidth, leftMin.y - dp(8.0f)));
			if (OutlineButton(ICON_FA_FOLDER_OPEN "  Open", ImVec2(openWidth, buttonHeight)))
			{
				state.dialog = ProjectLauncherDialog::Open;
				state.error.clear();
			}
			ImGui::SameLine(0.0f, actionGap);
			if (PrimaryButton(ICON_FA_PLUS "  New Project", ImVec2(newWidth, buttonHeight)))
			{
				state.dialog = ProjectLauncherDialog::Create;
				state.error.clear();
			}

			float gridStart = leftMin.y + dp(30.0f);
			if (!state.error.empty())
			{
				TextSized(dl, dp(13.0f), ImVec2(leftMin.x, gridStart), kError, state.error.c_str());
				gridStart += dp(44.0f);
			}

			const float bleed = dp(6.0f);
			ImGui::SetCursorScreenPos(ImVec2(leftMin.x - bleed, gridStart));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(bleed, bleed));
			ImGui::BeginChild("##launcherRecentProjects", ImVec2(gridWidth + bleed * 2.0f, leftMax.y - gridStart + bleed), ImGuiChildFlags_AlwaysUseWindowPadding);

			if (model.recentProjects.empty())
			{
				const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
				ImDrawList* childList = ImGui::GetWindowDrawList();
				TextSized(childList, dp(15.0f), Add(emptyPos, dp(2.0f, 8.0f)), kMuted, "Nothing here yet.");
				TextSized(childList, dp(14.0f), Add(emptyPos, dp(2.0f, 34.0f)), kMuted, "Open an existing project or create a new one to get started.");
			}
			else
			{
				const float gridGap = dp(16.0f);
				const float avail = ImGui::GetContentRegionAvail().x;
				const int columns = std::max(1, static_cast<int>((avail + gridGap) / (dp(210.0f) + gridGap)));
				const float cardWidth = std::floor((avail - gridGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
				const float cardHeight = std::floor(cardWidth * 9.0f / 16.0f) + dp(64.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gridGap, gridGap));
				for (std::size_t i = 0; i < model.recentProjects.size(); ++i)
				{
					const EditorProjectContext& project = model.recentProjects[i];
					if (i % static_cast<std::size_t>(columns) != 0)
					{
						ImGui::SameLine(0.0f, gridGap);
					}
					const bool selected = model.currentProject != nullptr && model.currentProject->root == project.root;
					std::error_code existsEc;
					const bool missing = !std::filesystem::exists(project.root / "ProjectSettings.toml", existsEc);
					const std::uint64_t preview = model.previewTextureId ? model.previewTextureId(project) : 0;
					const std::string edited = model.modifiedLabel ? model.modifiedLabel(project) : std::string{};
					if (DrawProjectCard(project, preview, edited, selected, missing, ImVec2(cardWidth, cardHeight), dp) && actions.openProject)
					{
						actions.openProject(project.root);
					}
				}
				ImGui::PopStyleVar();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}

		void PaintFooter(const Frame& f, ProjectLauncherWindowState& state, const ProjectLauncherWindowActions& actions)
		{
			const float textSize = f.dp(14.0f);
			TextSized(f.drawList, textSize, ImVec2(f.contentMin.x, f.contentMax.y - textSize - f.dp(4.0f)), kMuted, "AetherCore Editor");
			const char* label = "Open last project on startup";
			const float controlWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize(label).x;
			ImGui::SetCursorScreenPos(ImVec2(f.contentMax.x - controlWidth, f.contentMax.y - textSize - f.dp(8.0f)));
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			if (ImGui::Checkbox(label, &state.openLastProject) && actions.saveSettings)
			{
				actions.saveSettings();
			}
			ImGui::PopStyleColor();
		}

		// Resolve the responsive layout for the current window size into a Frame.
		[[nodiscard]] Frame ResolveFrame(ImDrawList* drawList, const Px& dp, const ImVec2 windowMin, const ImVec2 windowMax)
		{
			Frame f;
			f.drawList = drawList;
			f.dp = dp;

			const float width = windowMax.x - windowMin.x;
			const float height = windowMax.y - windowMin.y;

			const float marginX = std::clamp(width * 0.05f, dp(20.0f), dp(84.0f));
			f.contentWidth = std::min(width - marginX * 2.0f, dp(1720.0f));
			f.contentX = windowMin.x + (width - f.contentWidth) * 0.5f;
			f.contentMin = ImVec2(f.contentX, windowMin.y + std::clamp(height * 0.06f, dp(18.0f), dp(64.0f)));
			f.contentMax = ImVec2(f.contentX + f.contentWidth, windowMax.y - std::clamp(height * 0.05f, dp(16.0f), dp(56.0f)));

			f.wordmarkSize = std::clamp(f.contentWidth * 0.032f, dp(28.0f), dp(42.0f));

			f.gridTop = f.contentMin.y + f.wordmarkSize + dp(62.0f);
			f.gridBottom = f.contentMax.y - dp(30.0f);
			return f;
		}
	} // namespace

	void ProjectLauncherWindow::Draw(ProjectLauncherWindowState& state, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowActions& actions)
	{
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
		const Frame f = ResolveFrame(ImGui::GetWindowDrawList(), MakeScale(), windowMin, windowMax);

		PaintBackground(f, windowMin, windowMax);
		PaintHeader(f, model, state);

		ImGui::BeginDisabled(state.launching);
		PaintRecentsGrid(f, state, model, actions);
		PaintFooter(f, state, actions);
		ImGui::EndDisabled();
		DrawProjectDialog(state, actions, f);

		ImGui::End();
	}
} // namespace aether::app
