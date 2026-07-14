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

		// ── Layout metrics (design pixels) ──────────────────────────────────────────
		namespace m
		{
			inline constexpr float kWordmarkMin = 28.0f, kWordmarkMax = 42.0f;
			inline constexpr float kSubtitle = 14.5f;
			inline constexpr float kSectionLabel = 13.0f;
			inline constexpr float kBodyPath = 13.0f;
			inline constexpr float kCardTitle = 16.0f;
			inline constexpr float kBadge = 12.0f;
			inline constexpr float kChip = 12.0f;
			inline constexpr float kMissing = 11.0f;
			inline constexpr float kFooter = 14.0f;
			inline constexpr float kPlaceholderIcon = 34.0f;
			inline constexpr float kEmptyTitle = 15.0f, kEmptyBody = 14.0f;

			inline constexpr float kContentWidthCap = 1720.0f;
			inline constexpr float kMarginMin = 20.0f, kMarginMax = 84.0f;
			inline constexpr float kBandTopMin = 18.0f, kBandTopMax = 64.0f;
			inline constexpr float kBandBottomMin = 16.0f, kBandBottomMax = 56.0f;
			inline constexpr float kColumnsTopGap = 62.0f;
			inline constexpr float kFooterBand = 30.0f;

			inline constexpr float kFieldStride = 44.0f;
			inline constexpr float kButtonStd = 38.0f;
			inline constexpr float kBrowseGap = 8.0f;
			inline constexpr float kInputRounding = 3.0f;
			inline constexpr float kInputPadX = 12.0f, kInputPadY = 9.0f;

			inline constexpr float kGridGap = 16.0f;
			inline constexpr float kCardMinWidth = 210.0f;
			inline constexpr float kCardTextBlock = 64.0f;
			inline constexpr float kGridBleed = 6.0f;
			inline constexpr float kLabelToGrid = 30.0f;
			inline constexpr float kGridMinHeight = 96.0f, kGridMinWidth = 220.0f;
			inline constexpr float kEmptyTextX = 2.0f;
			inline constexpr float kEmptyTitleY = 8.0f, kEmptyBodyY = 34.0f;

			inline constexpr float kCardCorner = 4.0f;
			inline constexpr float kCardBorder = 1.0f;
			inline constexpr float kSelectedBar = 3.0f;
			inline constexpr float kBracketArm = 12.0f;
			inline constexpr float kBracketThickness = 2.0f;
			inline constexpr float kBracketBleed = 3.0f;
			inline constexpr float kThumbInset = 1.0f;
			inline constexpr float kCardTextInset = 12.0f;
			inline constexpr float kCardTitleTop = 10.0f;
			inline constexpr float kCardPathClipTop = 32.0f;
			inline constexpr float kCardPathTextTop = 33.0f;
			inline constexpr float kCardPathBottomInset = 8.0f;
			inline constexpr float kBadgeInset = 10.0f;
			inline constexpr float kChipInset = 16.0f;
			inline constexpr float kChipTop = 12.0f;
			inline constexpr float kChipCorner = 2.0f;
			inline constexpr float kChipPadX = 6.0f, kChipPadY = 4.0f;
			inline constexpr float kOpenPadX = 8.0f, kOpenPadY = 5.0f;

			inline constexpr float kWordmarkScale = 0.032f;
			inline constexpr float kLogoExtra = 16.0f;
			inline constexpr float kLogoGap = 14.0f;
			inline constexpr float kTagGap = 16.0f;
			inline constexpr float kTagTop = 10.0f;
			inline constexpr float kTagPadX = 8.0f, kTagPadY = 5.0f;
			inline constexpr float kUnderlineInset = 2.0f, kUnderlineWidth = 58.0f;
			inline constexpr float kUnderlineTop = 10.0f, kUnderlineBottom = 13.0f;
			inline constexpr float kSubtitleTop = 24.0f;
			inline constexpr float kLaunchingTop = 46.0f;

			inline constexpr float kGridPitch = 56.0f;
			inline constexpr float kGridAlpha = 0.16f;
			inline constexpr float kWashHeight = 240.0f;
			inline constexpr float kWashWidthFrac = 0.55f;
			inline constexpr float kWashAlpha = 0.05f;
			inline constexpr float kHairline = 2.0f;
			inline constexpr float kSlashInset = 96.0f, kSlashTop = 44.0f;
			inline constexpr float kSlashStride = 18.0f, kSlashRun = 14.0f, kSlashDrop = 26.0f;
			inline constexpr float kSlashThickness = 3.0f;
		} // namespace m

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
			TextSized(drawList, dp(m::kSectionLabel), Add(pos, dp(10.0f, -1.0f)), kMuted, label);
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
			const float corner = dp(m::kCardCorner);

			drawList->AddRectFilled(start, end, ToU32(lit ? kPanelHi : kPanel), corner);
			drawList->AddRect(start, end, ToU32(lit ? WithAlpha(kAccent, 0.5f) : kStroke), corner, 0, dp(m::kCardBorder));
			if (selected && !missing)
			{
				drawList->AddRectFilled(start, ImVec2(end.x, start.y + dp(m::kSelectedBar)), ToU32(kAccent), corner, ImDrawFlags_RoundCornersTop);
			}
			if (lit)
			{
				const float bleed = dp(m::kBracketBleed);
				CornerBrackets(drawList, Add(start, ImVec2(-bleed, -bleed)), Add(end, ImVec2(bleed, bleed)), dp(m::kBracketArm), dp(m::kBracketThickness), kAccent);
			}

			const float thumbHeight = std::floor(cardSize.x * 9.0f / 16.0f);
			const ImVec2 thumbMin = Add(start, dp(m::kThumbInset, m::kThumbInset));
			const ImVec2 thumbMax(end.x - dp(m::kThumbInset), start.y + thumbHeight);
			if (previewTextureId != 0 && !missing)
			{
				drawList->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(previewTextureId)), thumbMin, thumbMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255), corner, ImDrawFlags_RoundCornersTop);
			}
			else
			{
				drawList->AddRectFilled(thumbMin, thumbMax, ToU32(kBg), corner, ImDrawFlags_RoundCornersTop);
				const ImVec2 iconSize = MeasureSized(dp(m::kPlaceholderIcon), ICON_FA_CUBE);
				const ImVec2 iconPos((thumbMin.x + thumbMax.x - iconSize.x) * 0.5f, (thumbMin.y + thumbMax.y - iconSize.y) * 0.5f);
				TextSized(drawList, dp(m::kPlaceholderIcon), iconPos, missing ? kFaint : WithAlpha(kAccent, 0.5f), ICON_FA_CUBE);
			}
			drawList->AddLine(ImVec2(thumbMin.x, thumbMax.y), ImVec2(thumbMax.x, thumbMax.y), ToU32(kStroke), 1.0f);

			if (!modifiedLabel.empty() && !missing)
			{
				const ImVec2 badgeSize = MeasureSized(dp(m::kBadge), modifiedLabel.c_str());
				const ImVec2 badgePos(thumbMin.x + dp(m::kBadgeInset), thumbMax.y - badgeSize.y - dp(m::kBadgeInset));
				ScrimBehind(drawList, badgePos, badgeSize, dp(m::kChipPadX, m::kChipPadY), WithAlpha(kBg, 0.7f), dp(m::kChipCorner));
				TextSized(drawList, dp(m::kBadge), badgePos, kMuted, modifiedLabel.c_str());
			}

			const float textX = start.x + dp(m::kCardTextInset);
			TextSized(drawList, dp(m::kCardTitle), ImVec2(textX, thumbMax.y + dp(m::kCardTitleTop)), missing ? kMuted : kText, project.name.c_str());
			const std::string path = DisplayPath(project.root);
			ImGui::PushClipRect(ImVec2(textX, thumbMax.y + dp(m::kCardPathClipTop)), Sub(end, dp(m::kCardTextInset, m::kCardPathBottomInset)), true);
			TextSized(drawList, dp(m::kBodyPath), ImVec2(textX, thumbMax.y + dp(m::kCardPathTextTop)), missing ? kFaint : kMuted, path.c_str());
			ImGui::PopClipRect();

			if (missing)
			{
				const char* tag = "MISSING";
				const ImVec2 tagSize = MeasureSized(dp(m::kMissing), tag);
				const ImVec2 tp(thumbMax.x - tagSize.x - dp(m::kChipInset), thumbMin.y + dp(m::kChipTop));
				ScrimBehind(drawList, tp, tagSize, dp(m::kChipPadX, m::kChipPadY), WithAlpha(kBg, 0.82f), dp(m::kChipCorner));
				TextSized(drawList, dp(m::kMissing), tp, kError, tag);
				if (hovered)
				{
					ImGui::SetTooltip("ProjectSettings.toml no longer exists at\n%s", path.c_str());
				}
			}
			else if (hovered)
			{
				const char* hint = "OPEN";
				const ImVec2 hintSize = MeasureSized(dp(m::kChip), hint);
				const ImVec2 hp(thumbMax.x - hintSize.x - dp(m::kChipInset), thumbMin.y + dp(m::kChipTop));
				ScrimBehind(drawList, hp, hintSize, dp(m::kOpenPadX, m::kOpenPadY), WithAlpha(kAccent, 0.9f), dp(m::kChipCorner));
				TextSized(drawList, dp(m::kChip), hp, kOnAccent, hint);
				ImGui::SetTooltip("%s", path.c_str());
			}
			ImGui::PopID();
			return pressed;
		}

		void PushInputStyles(const Px& dp)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, dp(m::kInputRounding));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, dp(m::kInputPadX, m::kInputPadY));
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

			const float pitch = dp(m::kGridPitch);
			const ImU32 gridColor = ToU32(WithAlpha(kStroke, m::kGridAlpha));
			for (float x = min.x + pitch; x < max.x; x += pitch)
			{
				dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), gridColor, 1.0f);
			}
			for (float y = min.y + pitch; y < max.y; y += pitch)
			{
				dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), gridColor, 1.0f);
			}

			const ImVec2 washMax(min.x + (max.x - min.x) * m::kWashWidthFrac, min.y + dp(m::kWashHeight));
			dl->AddRectFilledMultiColor(min, washMax, ToU32(WithAlpha(kAccent, m::kWashAlpha)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)));

			dl->AddRectFilledMultiColor(ImVec2(min.x, max.y - dp(m::kHairline)), max, ToU32(WithAlpha(kAccent, 0.85f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.0f)), ToU32(WithAlpha(kAccent, 0.85f)));

			const float slashBaseX = max.x - dp(m::kSlashInset);
			const float slashY = min.y + dp(m::kSlashTop);
			for (int i = 0; i < 3; ++i)
			{
				const float x = slashBaseX + static_cast<float>(i) * dp(m::kSlashStride);
				dl->AddLine(ImVec2(x + dp(m::kSlashRun), slashY), ImVec2(x, slashY + dp(m::kSlashDrop)), ToU32(WithAlpha(kAccent, 0.65f - static_cast<float>(i) * 0.2f)), dp(m::kSlashThickness));
			}
		}

		void PaintHeader(const Frame& f, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowState& state)
		{
			ImDrawList* dl = f.drawList;
			const Px& dp = f.dp;
			const float wordmark = f.wordmarkSize;

			const bool hasLogo = model.logoTextureId != 0;
			const float logoSize = wordmark + dp(m::kLogoExtra);
			if (hasLogo)
			{
				dl->AddImage(ImTextureRef(static_cast<ImTextureID>(model.logoTextureId)), f.contentMin, Add(f.contentMin, ImVec2(logoSize, logoSize)), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ToU32(kAccent));
			}

			const ImVec2 wordmarkPos = Add(f.contentMin, ImVec2(hasLogo ? logoSize + dp(m::kLogoGap) : 0.0f, 0.0f));
			const ImVec2 aetherSize = MeasureSized(wordmark, "AETHER");
			TextSized(dl, wordmark, wordmarkPos, kText, "AETHER");
			TextSized(dl, wordmark, Add(wordmarkPos, ImVec2(aetherSize.x, 0.0f)), kAccent, "CORE");
			const ImVec2 coreSize = MeasureSized(wordmark, "CORE");

			const ImVec2 tagText = MeasureSized(dp(m::kChip), "EDITOR");
			const ImVec2 tagMin = Add(wordmarkPos, ImVec2(aetherSize.x + coreSize.x + dp(m::kTagGap), dp(m::kTagTop)));
			const ImVec2 tagMax = Add(tagMin, Add(tagText, dp(m::kTagPadX * 2.0f, m::kTagPadY * 2.0f)));
			dl->AddRect(tagMin, tagMax, ToU32(WithAlpha(kAccent, 0.5f)), dp(m::kChipCorner), 0, dp(m::kCardBorder));
			TextSized(dl, dp(m::kChip), Add(tagMin, dp(m::kTagPadX, m::kTagPadY)), kAccentHi, "EDITOR");

			dl->AddRectFilled(Add(wordmarkPos, ImVec2(dp(m::kUnderlineInset), wordmark + dp(m::kUnderlineTop))), Add(wordmarkPos, ImVec2(dp(m::kUnderlineWidth), wordmark + dp(m::kUnderlineBottom))), ToU32(kAccent));
			TextSized(dl, dp(m::kSubtitle), Add(wordmarkPos, ImVec2(dp(m::kUnderlineInset), wordmark + dp(m::kSubtitleTop))), kMuted, "Select a project to begin.");
			if (state.launching)
			{
				TextSized(dl, dp(m::kSubtitle), Add(wordmarkPos, ImVec2(dp(m::kUnderlineInset), wordmark + dp(m::kLaunchingTop))), kAccentHi, "Starting editor…");
			}
		}

		void DrawProjectDialog(ProjectLauncherWindowState& state, const ProjectLauncherWindowActions& actions, const Px& dp)
		{
			if (state.dialog == ProjectLauncherDialog::None)
			{
				return;
			}

			const bool opening = state.dialog == ProjectLauncherDialog::Open;
			const char* popupName = opening ? "Open Project" : "Create Project";
			ImGui::OpenPopup(popupName);
			ImGui::SetNextWindowSize(dp(520.0f, 0.0f), ImGuiCond_Appearing);
			if (!ImGui::BeginPopupModal(popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			{
				return;
			}

			PushInputStyles(dp);
			const float width = ImGui::GetContentRegionAvail().x;
			const float browseSize = ImGui::GetFrameHeight();
			bool submit = false;
			if (opening)
			{
				ImGui::TextUnformatted("Open an existing AetherCore project");
				ImGui::TextDisabled("Choose its ProjectSettings.toml file.");
				ImGui::Spacing();
				ImGui::SetNextItemWidth(width - browseSize - dp(m::kBrowseGap));
				submit = ImGui::InputTextWithHint("##openProjectPath", "Path to ProjectSettings.toml...", state.openPath.data(), state.openPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine(0.0f, dp(m::kBrowseGap));
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
				ImGui::SetNextItemWidth(width - browseSize - dp(m::kBrowseGap));
				submit = ImGui::InputTextWithHint("##newProjectPath", "Project folder...", state.newPath.data(), state.newPath.size(), ImGuiInputTextFlags_EnterReturnsTrue) || submit;
				ImGui::SameLine(0.0f, dp(m::kBrowseGap));
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
			ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - cancelWidth - dp(116.0f));
			if (OutlineButton("Cancel", ImVec2(cancelWidth, dp(m::kButtonStd))))
			{
				state.dialog = ProjectLauncherDialog::None;
				state.error.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine(0.0f, dp(m::kBrowseGap));
			ImGui::BeginDisabled(incomplete);
			const char* actionLabel = opening ? ICON_FA_FOLDER_OPEN "  Open" : ICON_FA_PLUS "  Create";
			submit = PrimaryButton(actionLabel, ImVec2(dp(108.0f), dp(m::kButtonStd))) || submit;
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
			if (leftMax.y - leftMin.y <= dp(m::kGridMinHeight) || leftMax.x - leftMin.x <= dp(m::kGridMinWidth))
			{
				return;
			}

			const float gridWidth = leftMax.x - leftMin.x;
			SectionLabel(dl, leftMin, "RECENT PROJECTS", dp);
			const float newWidth = dp(132.0f);
			const float openWidth = dp(112.0f);
			const float actionGap = dp(m::kBrowseGap);
			ImGui::SetCursorScreenPos(ImVec2(leftMax.x - newWidth - actionGap - openWidth, leftMin.y - dp(8.0f)));
			if (OutlineButton(ICON_FA_FOLDER_OPEN "  Open", ImVec2(openWidth, dp(m::kButtonStd))))
			{
				state.dialog = ProjectLauncherDialog::Open;
				state.error.clear();
			}
			ImGui::SameLine(0.0f, actionGap);
			if (PrimaryButton(ICON_FA_PLUS "  New Project", ImVec2(newWidth, dp(m::kButtonStd))))
			{
				state.dialog = ProjectLauncherDialog::Create;
				state.error.clear();
			}

			float gridStart = leftMin.y + dp(m::kLabelToGrid);
			if (!state.error.empty())
			{
				TextSized(dl, dp(m::kBodyPath), ImVec2(leftMin.x, gridStart), kError, state.error.c_str());
				gridStart += dp(m::kFieldStride);
			}

			const float bleed = dp(m::kGridBleed);
			ImGui::SetCursorScreenPos(ImVec2(leftMin.x - bleed, gridStart));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(bleed, bleed));
			ImGui::BeginChild("##launcherRecentProjects", ImVec2(gridWidth + bleed * 2.0f, leftMax.y - gridStart + bleed), ImGuiChildFlags_AlwaysUseWindowPadding);

			if (model.recentProjects.empty())
			{
				const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
				ImDrawList* childList = ImGui::GetWindowDrawList();
				TextSized(childList, dp(m::kEmptyTitle), Add(emptyPos, dp(m::kEmptyTextX, m::kEmptyTitleY)), kMuted, "Nothing here yet.");
				TextSized(childList, dp(m::kEmptyBody), Add(emptyPos, dp(m::kEmptyTextX, m::kEmptyBodyY)), kMuted, "Open an existing project or create a new one to get started.");
			}
			else
			{
				const float gridGap = dp(m::kGridGap);
				const float avail = ImGui::GetContentRegionAvail().x;
				const int columns = std::max(1, static_cast<int>((avail + gridGap) / (dp(m::kCardMinWidth) + gridGap)));
				const float cardWidth = std::floor((avail - gridGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
				const float cardHeight = std::floor(cardWidth * 9.0f / 16.0f) + dp(m::kCardTextBlock);
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
			TextSized(f.drawList, f.dp(m::kFooter), ImVec2(f.contentMin.x, f.contentMax.y - f.dp(m::kFooter + 4.0f)), kMuted, "AetherCore Editor");
			const char* label = "Open last project on startup";
			const float controlWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize(label).x;
			ImGui::SetCursorScreenPos(ImVec2(f.contentMax.x - controlWidth, f.contentMax.y - f.dp(m::kFooter + 8.0f)));
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

			const float marginX = std::clamp(width * 0.05f, dp(m::kMarginMin), dp(m::kMarginMax));
			f.contentWidth = std::min(width - marginX * 2.0f, dp(m::kContentWidthCap));
			f.contentX = windowMin.x + (width - f.contentWidth) * 0.5f;
			f.contentMin = ImVec2(f.contentX, windowMin.y + std::clamp(height * 0.06f, dp(m::kBandTopMin), dp(m::kBandTopMax)));
			f.contentMax = ImVec2(f.contentX + f.contentWidth, windowMax.y - std::clamp(height * 0.05f, dp(m::kBandBottomMin), dp(m::kBandBottomMax)));

			f.wordmarkSize = std::clamp(f.contentWidth * m::kWordmarkScale, dp(m::kWordmarkMin), dp(m::kWordmarkMax));

			f.gridTop = f.contentMin.y + f.wordmarkSize + dp(m::kColumnsTopGap);
			f.gridBottom = f.contentMax.y - dp(m::kFooterBand);
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
		DrawProjectDialog(state, actions, f.dp);

		ImGui::End();
	}
} // namespace aether::app
