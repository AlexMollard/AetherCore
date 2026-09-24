#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

#include <imgui.h>

#include "assets/AssetDatabase.hpp"
#include "debug/EditorChrome.hpp"
#include "EditorDragDrop.hpp"
#include "Icons.hpp"
#include "SceneSelection.hpp"
#include "layers/AppLayer.hpp"

namespace aether::editor::spriteui
{
	enum class AssetRole
	{
		Texture,
		Atlas,
		Animation,
		AsepriteJson,
	};

	struct AssetSlotChange
	{
		bool changed = false;
		std::string path;
	};

	[[nodiscard]] inline std::string Lower(std::string_view value)
	{
		std::string result(value);
		std::ranges::transform(result, result.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return result;
	}

	[[nodiscard]] inline bool EndsWith(std::string_view value, std::string_view suffix)
	{
		const std::string lowerValue = Lower(value);
		const std::string lowerSuffix = Lower(suffix);
		return lowerValue.size() >= lowerSuffix.size() && lowerValue.ends_with(lowerSuffix);
	}

	[[nodiscard]] inline bool MatchesRole(AssetRole role, std::string_view path, dragdrop::FileKind kind = dragdrop::FileKind::Unknown)
	{
		switch (role)
		{
			case AssetRole::Texture:
				return kind == dragdrop::FileKind::Texture || EndsWith(path, ".png") || EndsWith(path, ".jpg") || EndsWith(path, ".jpeg") || EndsWith(path, ".bmp") || EndsWith(path, ".tga") || EndsWith(path, ".hdr") || EndsWith(path, ".exr");
			case AssetRole::Atlas:
				return EndsWith(path, ".spriteatlas.toml");
			case AssetRole::Animation:
				return EndsWith(path, ".spriteanim.toml");
			case AssetRole::AsepriteJson:
				return EndsWith(path, ".json");
		}
		return false;
	}

	[[nodiscard]] inline AssetType RoleAssetType(AssetRole role)
	{
		switch (role)
		{
			case AssetRole::Texture:
				return AssetType::Texture;
			case AssetRole::Atlas:
				return AssetType::SpriteAtlas;
			case AssetRole::Animation:
				return AssetType::SpriteAnimation;
			case AssetRole::AsepriteJson:
				return AssetType::Unknown;
		}
		return AssetType::Unknown;
	}

	[[nodiscard]] inline std::string DisplayName(std::string_view path)
	{
		if (path.empty())
		{
			return {};
		}
		const std::string filename = std::filesystem::path(path).filename().string();
		return filename.empty() ? std::string(path) : filename;
	}

	[[nodiscard]] inline std::string CompanionPath(std::string_view source, std::string_view suffix)
	{
		std::string result(source);
		for (const std::string_view compoundSuffix: {std::string_view(".spriteatlas.toml"), std::string_view(".spriteanim.toml")})
		{
			if (EndsWith(result, compoundSuffix))
			{
				result.resize(result.size() - compoundSuffix.size());
				result.append(suffix);
				return result;
			}
		}
		const std::size_t slash = result.find_last_of("/\\");
		const std::size_t dot = result.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		{
			result.resize(dot);
		}
		result.append(suffix);
		return result;
	}

	inline AssetSlotChange DrawAssetSlot(app::LayerContext& context, const char* id, const char* icon, const char* title, std::string_view currentPath, AssetRole role, const char* emptyHint, bool allowClear = false)
	{
		AssetSlotChange result;
		ImGui::PushID(id);

		auto* selection = context.TryGet<SceneSelection>();
		const bool selectedMatches = selection != nullptr && selection->HasAsset() && MatchesRole(role, selection->SelectedAsset().path);
		const bool hasCurrent = !currentPath.empty();
		const AssetType assetType = RoleAssetType(role);
		auto* database = context.TryGet<AssetDatabase>();

		const float rowStart = ImGui::GetCursorPosX();
		ImGui::TextColored(chrome::kAccentHi, "%s", icon);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", title);
		ImGui::SameLine(rowStart + 128.0f);

		const float clearWidth = allowClear && hasCurrent ? 28.0f + ImGui::GetStyle().ItemSpacing.x : 0.0f;
		const float selectedWidth = 112.0f;
		const float fieldWidth = std::max(160.0f, ImGui::GetContentRegionAvail().x - selectedWidth - clearWidth - ImGui::GetStyle().ItemSpacing.x);
		const std::string fieldLabel = std::string(hasCurrent ? DisplayName(currentPath) : emptyHint) + "##assetField";
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.02f, 0.5f));
		if (ImGui::Button(fieldLabel.c_str(), ImVec2(fieldWidth, 0.0f)) && assetType != AssetType::Unknown && database != nullptr)
		{
			ImGui::OpenPopup("##assetPicker");
		}
		ImGui::PopStyleVar();
		if (ImGui::IsItemHovered())
		{
			if (hasCurrent)
			{
				ImGui::SetTooltip("%s\nDrop a compatible asset to replace it.", std::string(currentPath).c_str());
			}
			else
			{
				ImGui::SetTooltip("Click to browse or drop a compatible asset here.");
			}
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 fieldMin = ImGui::GetItemRectMin();
		const ImVec2 fieldMax = ImGui::GetItemRectMax();
		if (const ImGuiPayload* active = ImGui::GetDragDropPayload(); active != nullptr && active->IsDataType(dragdrop::kFilePayload) && active->DataSize == sizeof(dragdrop::FilePayload))
		{
			const auto* file = static_cast<const dragdrop::FilePayload*>(active->Data);
			if (MatchesRole(role, file->path, file->kind))
			{
				drawList->AddRect(fieldMin, fieldMax, chrome::U32(chrome::kDropTarget), ImGui::GetStyle().FrameRounding, 0, 2.0f);
			}
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect); payload != nullptr && payload->DataSize == sizeof(dragdrop::FilePayload))
			{
				const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
				if (MatchesRole(role, file->path, file->kind))
				{
					result.changed = true;
					result.path = file->path;
				}
			}
			ImGui::EndDragDropTarget();
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(!selectedMatches);
		if (chrome::OutlineButton("Use Selected", ImVec2(selectedWidth, 0.0f)))
		{
			result.changed = true;
			result.path = selection->SelectedAsset().path;
		}
		ImGui::EndDisabled();
		if (allowClear && hasCurrent)
		{
			ImGui::SameLine();
			if (chrome::GhostIconButton(ICON_FA_XMARK, "##clearAsset", ImVec2(28.0f, 0.0f)))
			{
				result.changed = true;
				result.path.clear();
			}
		}

		if (ImGui::BeginPopup("##assetPicker"))
		{
			bool any = false;
			database->ForEach(assetType,
			        [&](AssetId assetId, const AssetDatabase::Entry& entry)
			        {
				        any = true;
				        ImGui::PushID(entry.source.path.c_str());
				        if (ImGui::Selectable(entry.displayName.c_str()))
				        {
					        AssetSource source;
					        if (database->Describe(assetId, source))
					        {
						        result.changed = true;
						        result.path = source.path;
					        }
					        ImGui::CloseCurrentPopup();
				        }
				        if (ImGui::IsItemHovered())
				        {
					        ImGui::SetTooltip("%s", entry.source.path.c_str());
				        }
				        ImGui::PopID();
			        });
			if (!any)
			{
				ImGui::TextDisabled("No matching assets are catalogued yet.");
			}
			ImGui::EndPopup();
		}

		ImGui::PopID();
		return result;
	}

	inline void DrawStatus(std::string_view message, bool error)
	{
		if (message.empty())
		{
			return;
		}
		const ImVec4 tint = error ? chrome::kError : chrome::kSuccess;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::WithAlpha(tint, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_Border, chrome::WithAlpha(tint, 0.45f));
		ImGui::BeginChild("##spriteStatus", ImVec2(0.0f, 38.0f), ImGuiChildFlags_Borders);
		ImGui::TextColored(tint, "%s  %s", error ? "!" : "OK", std::string(message).c_str());
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
	}

	inline void DrawEmptyState(const char* icon, const char* title, const char* detail)
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::kPanel);
		ImGui::PushStyleColor(ImGuiCol_Border, chrome::WithAlpha(chrome::kStroke, 0.70f));
		ImGui::BeginChild("##spriteEmpty", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders);
		ImGui::Dummy(ImVec2(0.0f, 24.0f));
		const float titleWidth = ImGui::CalcTextSize(title).x + ImGui::CalcTextSize(icon).x + 8.0f;
		ImGui::SetCursorPosX(std::max(8.0f, (ImGui::GetContentRegionAvail().x - titleWidth) * 0.5f));
		ImGui::TextColored(chrome::kAccentHi, "%s", icon);
		ImGui::SameLine();
		ImGui::TextUnformatted(title);
		ImGui::SetCursorPosX(24.0f);
		ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 24.0f);
		ImGui::TextColored(chrome::kMuted, "%s", detail);
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
	}
} // namespace aether::editor::spriteui
