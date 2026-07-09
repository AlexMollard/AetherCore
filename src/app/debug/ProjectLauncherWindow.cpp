#include "debug/ProjectLauncherWindow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

#include "debug/Icons.hpp"

namespace aether::app
{
	namespace
	{
		constexpr ImVec4 kInk{0.06f, 0.055f, 0.05f, 1.0f};
		constexpr ImVec4 kPanel{0.105f, 0.095f, 0.085f, 0.96f};
		constexpr ImVec4 kPanelSoft{0.15f, 0.13f, 0.105f, 0.82f};
		constexpr ImVec4 kAmber{0.95f, 0.55f, 0.18f, 1.0f};
		constexpr ImVec4 kGold{1.0f, 0.76f, 0.36f, 1.0f};
		constexpr ImVec4 kTeal{0.20f, 0.72f, 0.72f, 1.0f};
		constexpr ImVec4 kSteel{0.36f, 0.45f, 0.55f, 1.0f};
		constexpr ImVec4 kText{0.88f, 0.84f, 0.76f, 1.0f};
		constexpr ImVec4 kMuted{0.58f, 0.54f, 0.48f, 1.0f};
		constexpr ImVec4 kError{0.96f, 0.36f, 0.32f, 1.0f};

		[[nodiscard]] ImU32 ToU32(const ImVec4& color)
		{
			return ImGui::ColorConvertFloat4ToU32(color);
		}

		[[nodiscard]] ImVec2 Add(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x + b.x, a.y + b.y);
		}

		[[nodiscard]] ImVec2 Sub(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x - b.x, a.y - b.y);
		}

		[[nodiscard]] ImVec4 WithAlpha(ImVec4 color, float alpha)
		{
			color.w = alpha;
			return color;
		}

		[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		template <std::size_t N>
		void CopyToBuffer(std::array<char, N>& buffer, const std::filesystem::path& path)
		{
			std::snprintf(buffer.data(), buffer.size(), "%s", DisplayPath(path).c_str());
		}

		void TextAt(ImDrawList* drawList, ImVec2 pos, ImU32 color, const char* text)
		{
			drawList->AddText(pos, color, text);
		}

		void DrawLauncherBackground(ImDrawList* drawList, const ImVec2 min, const ImVec2 max)
		{
			drawList->AddRectFilledMultiColor(min, max, ToU32(kInk), ToU32(ImVec4{0.055f, 0.075f, 0.078f, 1.0f}), ToU32(ImVec4{0.13f, 0.095f, 0.065f, 1.0f}), ToU32(ImVec4{0.08f, 0.055f, 0.045f, 1.0f}));

			const float width = max.x - min.x;
			const float height = max.y - min.y;
			const ImVec2 horizonA{min.x, min.y + height * 0.64f};
			const ImVec2 horizonB{max.x, min.y + height * 0.82f};
			drawList->AddRectFilledMultiColor(horizonA, horizonB, ToU32(WithAlpha(kAmber, 0.0f)), ToU32(WithAlpha(kTeal, 0.04f)), ToU32(WithAlpha(kAmber, 0.10f)), ToU32(WithAlpha(kGold, 0.08f)));

			for (int i = 0; i < 9; ++i)
			{
				const float t = static_cast<float>(i) / 8.0f;
				const float x = min.x + width * (0.10f + t * 0.76f);
				const float peak = min.y + height * (0.23f + 0.10f * std::sin(t * 7.0f));
				const float base = min.y + height * (0.72f + 0.06f * std::cos(t * 5.0f));
				drawList->AddLine(ImVec2{x, peak}, ImVec2{x - width * 0.16f, base}, ToU32(WithAlpha(kSteel, 0.13f)), 2.0f);
				drawList->AddLine(ImVec2{x, peak}, ImVec2{x + width * 0.13f, base}, ToU32(WithAlpha(kAmber, 0.10f)), 2.0f);
			}

			drawList->AddCircleFilled(ImVec2{min.x + width * 0.73f, min.y + height * 0.25f}, height * 0.11f, ToU32(WithAlpha(kGold, 0.10f)), 64);
			drawList->AddCircle(ImVec2{min.x + width * 0.73f, min.y + height * 0.25f}, height * 0.11f, ToU32(WithAlpha(kGold, 0.22f)), 64, 2.0f);
		}

		void DrawPanel(ImDrawList* drawList, const ImVec2 min, const ImVec2 max, const ImVec4& fill, const ImVec4& border)
		{
			drawList->AddRectFilled(min, max, ToU32(fill), 8.0f);
			drawList->AddRect(min, max, ToU32(border), 8.0f, 0, 1.2f);
		}

		bool ActionButton(const char* label, const ImVec2 size, const ImVec4& base, const ImVec4& hover)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, base);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{base.x * 0.92f, base.y * 0.92f, base.z * 0.92f, base.w});
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.08f, 0.07f, 0.055f, 1.0f});
			const bool pressed = ImGui::Button(label, size);
			ImGui::PopStyleColor(4);
			return pressed;
		}

		bool GhostButton(const char* label, const ImVec2 size)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0f, 0.0f, 0.0f, 0.20f});
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.95f, 0.55f, 0.18f, 0.18f});
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.95f, 0.55f, 0.18f, 0.26f});
			const bool pressed = ImGui::Button(label, size);
			ImGui::PopStyleColor(3);
			return pressed;
		}

		bool DrawRecentProjectRow(const EditorProjectContext& project, bool selected)
		{
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			const float height = 66.0f;
			ImGui::PushID(DisplayPath(project.root).c_str());
			const bool pressed = ImGui::InvisibleButton("##recentProject", ImVec2(width, height));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 end = Add(start, ImVec2(width, height));
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec4 fill = selected ? ImVec4{0.95f, 0.55f, 0.18f, 0.20f} : (hovered ? ImVec4{0.95f, 0.55f, 0.18f, 0.12f} : ImVec4{0.02f, 0.02f, 0.02f, 0.22f});
			DrawPanel(drawList, start, end, fill, selected ? WithAlpha(kGold, 0.64f) : WithAlpha(kSteel, hovered ? 0.42f : 0.22f));

			drawList->AddRectFilled(Add(start, ImVec2(12.0f, 12.0f)), Add(start, ImVec2(42.0f, 42.0f)), ToU32(WithAlpha(selected ? kGold : kTeal, 0.88f)), 5.0f);
			TextAt(drawList, Add(start, ImVec2(20.0f, 17.0f)), ToU32(ImVec4{0.08f, 0.07f, 0.055f, 1.0f}), ICON_FA_CUBE);
			TextAt(drawList, Add(start, ImVec2(52.0f, 10.0f)), ToU32(kText), project.name.c_str());

			const std::string path = DisplayPath(project.root);
			const char* pathText = path.c_str();
			ImGui::PushClipRect(Add(start, ImVec2(52.0f, 34.0f)), Sub(end, ImVec2(12.0f, 10.0f)), true);
			TextAt(drawList, Add(start, ImVec2(52.0f, 36.0f)), ToU32(kMuted), pathText);
			ImGui::PopClipRect();

			if (hovered)
			{
				ImGui::SetTooltip("%s", pathText);
			}
			ImGui::PopID();
			return pressed;
		}

		void DrawInputLabel(const char* label)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();
		}
	}

	void ProjectLauncherWindow::Draw(ProjectLauncherWindowState& state, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowActions& actions)
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
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
		const float margin = std::clamp(width * 0.045f, 34.0f, 72.0f);
		const ImVec2 contentMin = Add(windowMin, ImVec2(margin, margin));
		const ImVec2 contentMax = Sub(windowMax, ImVec2(margin, margin));

		TextAt(drawList, contentMin, ToU32(kGold), ICON_FA_CUBE "  AETHERCORE");
		TextAt(drawList, Add(contentMin, ImVec2(0.0f, 28.0f)), ToU32(kMuted), "Project Command Center");

		if (model.projectLoaded)
		{
			ImGui::SetCursorScreenPos(ImVec2(contentMax.x - 150.0f, contentMin.y));
			if (GhostButton("Back to Editor", ImVec2(150.0f, 34.0f)) && actions.closeLauncher)
			{
				actions.closeLauncher();
			}
		}

		const float leftWidth = std::clamp(width * 0.31f, 310.0f, 430.0f);
		const float rightWidth = std::clamp(width * 0.34f, 390.0f, 520.0f);
		const ImVec2 leftMin{contentMin.x, contentMin.y + 82.0f};
		const ImVec2 leftMax{leftMin.x + leftWidth, contentMax.y};
		const ImVec2 rightMin{contentMax.x - rightWidth, contentMin.y + 82.0f};
		const ImVec2 rightMax{contentMax.x, contentMax.y};

		const ImVec2 featureMin{leftMax.x + 24.0f, contentMin.y + height * 0.19f};
		const ImVec2 featureMax{rightMin.x - 24.0f, contentMax.y - height * 0.12f};
		if (featureMax.x > featureMin.x + 180.0f)
		{
			DrawPanel(drawList, featureMin, featureMax, ImVec4{0.05f, 0.045f, 0.04f, 0.42f}, WithAlpha(kGold, 0.22f));
			drawList->AddRectFilledMultiColor(Add(featureMin, ImVec2(1.0f, 1.0f)), Sub(featureMax, ImVec2(1.0f, 1.0f)), ToU32(WithAlpha(kAmber, 0.10f)), ToU32(WithAlpha(kTeal, 0.08f)), ToU32(WithAlpha(kSteel, 0.10f)), ToU32(WithAlpha(kGold, 0.04f)));
			TextAt(drawList, Add(featureMin, ImVec2(24.0f, 24.0f)), ToU32(kGold), "BUILD WORLDS");
			TextAt(drawList, Add(featureMin, ImVec2(24.0f, 52.0f)), ToU32(kText), "Create, open, and ship Aether projects from one dedicated hub.");
			TextAt(drawList, Add(featureMin, ImVec2(24.0f, 88.0f)), ToU32(kMuted), ICON_FA_FOLDER_OPEN "  Project-aware paths");
			TextAt(drawList, Add(featureMin, ImVec2(24.0f, 116.0f)), ToU32(kMuted), ICON_FA_CODE "  Per-project scripts");
			TextAt(drawList, Add(featureMin, ImVec2(24.0f, 144.0f)), ToU32(kMuted), ICON_FA_ROCKET "  Runtime publishing pipeline");
		}

		DrawPanel(drawList, leftMin, leftMax, kPanel, WithAlpha(kGold, 0.28f));
		ImGui::SetCursorScreenPos(Add(leftMin, ImVec2(18.0f, 16.0f)));
		ImGui::PushStyleColor(ImGuiCol_Text, kGold);
		ImGui::TextUnformatted("RECENT REALMS");
		ImGui::PopStyleColor();
		ImGui::SetCursorScreenPos(Add(leftMin, ImVec2(18.0f, 48.0f)));
		ImGui::BeginChild("##launcherRecentProjects", ImVec2(leftWidth - 36.0f, leftMax.y - leftMin.y - 64.0f), false);
		if (model.recentProjects.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextWrapped("No recent projects yet. Open an existing project or create a new one.");
			ImGui::PopStyleColor();
		}
		for (const EditorProjectContext& project: model.recentProjects)
		{
			const bool selected = model.currentProject != nullptr && model.currentProject->root == project.root;
			if (DrawRecentProjectRow(project, selected) && actions.openProject)
			{
				actions.openProject(project.root);
			}
			ImGui::Dummy(ImVec2(1.0f, 8.0f));
		}
		ImGui::EndChild();

		DrawPanel(drawList, rightMin, rightMax, kPanel, WithAlpha(kTeal, 0.32f));
		ImGui::SetCursorScreenPos(Add(rightMin, ImVec2(20.0f, 18.0f)));
		ImGui::PushStyleColor(ImGuiCol_Text, kGold);
		ImGui::TextUnformatted("PROJECT GATEWAY");
		ImGui::PopStyleColor();

		ImGui::SetCursorScreenPos(Add(rightMin, ImVec2(20.0f, 54.0f)));
		if (model.hasCurrentProject && model.currentProject != nullptr)
		{
			const ImVec2 currentMin = ImGui::GetCursorScreenPos();
			DrawPanel(drawList, currentMin, Add(currentMin, ImVec2(rightWidth - 40.0f, 82.0f)), kPanelSoft, WithAlpha(kGold, 0.20f));
			ImGui::SetCursorScreenPos(Add(currentMin, ImVec2(16.0f, 14.0f)));
			ImGui::BeginGroup();
			ImGui::PushStyleColor(ImGuiCol_Text, kText);
			ImGui::TextUnformatted(model.currentProject->name.c_str());
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextWrapped("%s", DisplayPath(model.currentProject->root).c_str());
			ImGui::PopStyleColor();
			ImGui::EndGroup();
			ImGui::SetCursorScreenPos(Add(rightMin, ImVec2(20.0f, 148.0f)));
			if (ActionButton(ICON_FA_PLAY "  Continue", ImVec2(174.0f, 38.0f), kGold, ImVec4{1.0f, 0.82f, 0.42f, 1.0f}) && actions.openProject)
			{
				actions.openProject(model.currentProject->root);
			}
			ImGui::SameLine();
			if (ImGui::Checkbox("Open last project", &state.openLastProject) && actions.saveSettings)
			{
				actions.saveSettings();
			}
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextWrapped("Choose a project to enter the editor.");
			ImGui::PopStyleColor();
		}

		const float formsTop = model.hasCurrentProject ? 210.0f : 98.0f;
		const ImVec2 formsMin = Add(rightMin, ImVec2(20.0f, formsTop));
		ImGui::SetCursorScreenPos(formsMin);
		ImGui::BeginChild("##launcherForms", ImVec2(rightWidth - 40.0f, std::max(180.0f, rightMax.y - formsMin.y - 24.0f)), false);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.02f, 0.02f, 0.02f, 0.38f});
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{0.95f, 0.55f, 0.18f, 0.10f});
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4{0.95f, 0.55f, 0.18f, 0.14f});

		DrawInputLabel("OPEN EXISTING");
		ImGui::SetNextItemWidth(-44.0f);
		ImGui::InputTextWithHint("##openProjectPath", "Project folder...", state.openPath.data(), state.openPath.size());
		ImGui::SameLine();
		if (GhostButton(ICON_FA_FOLDER_OPEN "##browseOpen", ImVec2(36.0f, 0.0f)) && actions.browseFolder)
		{
			if (const auto folder = actions.browseFolder())
			{
				CopyToBuffer(state.openPath, *folder);
			}
		}
		if (ActionButton(ICON_FA_FOLDER_OPEN "  Open Project", ImVec2(-1.0f, 38.0f), kAmber, ImVec4{1.0f, 0.64f, 0.24f, 1.0f}) && actions.openProject)
		{
			actions.openProject(std::filesystem::path(state.openPath.data()));
		}

		ImGui::Dummy(ImVec2(1.0f, 18.0f));
		DrawInputLabel("CREATE NEW");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##newProjectName", "Project name...", state.newName.data(), state.newName.size());
		ImGui::SetNextItemWidth(-44.0f);
		ImGui::InputTextWithHint("##newProjectPath", "Project folder...", state.newPath.data(), state.newPath.size());
		ImGui::SameLine();
		if (GhostButton(ICON_FA_FOLDER_OPEN "##browseNew", ImVec2(36.0f, 0.0f)) && actions.browseFolder)
		{
			if (const auto folder = actions.browseFolder())
			{
				CopyToBuffer(state.newPath, *folder);
			}
		}
		if (ActionButton(ICON_FA_PLUS "  Create Project", ImVec2(-1.0f, 38.0f), kTeal, ImVec4{0.27f, 0.84f, 0.84f, 1.0f}) && actions.createProject)
		{
			actions.createProject(std::filesystem::path(state.newPath.data()), state.newName.data());
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);

		if (!state.error.empty())
		{
			ImGui::Dummy(ImVec2(1.0f, 14.0f));
			ImGui::PushStyleColor(ImGuiCol_Text, kError);
			ImGui::TextWrapped("%s", state.error.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();

		ImGui::SetCursorScreenPos(ImVec2(contentMin.x, contentMax.y - 26.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
		ImGui::TextUnformatted("AetherCore Editor");
		ImGui::SameLine();
		ImGui::TextUnformatted("   " ICON_FA_ROCKET " ship-ready project pipeline");
		ImGui::PopStyleColor();

		ImGui::End();
	}
} // namespace aether::app
