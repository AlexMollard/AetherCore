#include "debug/FileExplorerPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	namespace
	{
		std::string ToUtf8Path(const std::filesystem::path& path)
		{
			return path.generic_string();
		}

		template<std::size_t N>
		void CopyToPayload(char (&dst)[N], const std::string& src)
		{
			std::snprintf(dst, N, "%s", src.c_str());
		}

		bool IsCSharpScriptFile(const std::filesystem::path& path)
		{
			return path.extension() == ".cs";
		}

		dragdrop::FileKind InferFileKind(const std::filesystem::path& path)
		{
			std::string ext = path.extension().generic_string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext == ".mesh")
			{
				return dragdrop::FileKind::Model;
			}
			if (ext == ".cs")
			{
				return dragdrop::FileKind::Script;
			}
			if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" || ext == ".texture")
			{
				return dragdrop::FileKind::Texture;
			}
			if (ext == ".toml")
			{
				const std::string generic = path.generic_string();
				if (generic.find(".prefab.toml") != std::string::npos)
				{
					return dragdrop::FileKind::Prefab;
				}
				if (generic.find(".scene.toml") != std::string::npos)
				{
					return dragdrop::FileKind::Scene;
				}
				if (generic.find("/materials/") != std::string::npos || generic.find("\\materials\\") != std::string::npos || path.filename() == "properties.toml")
				{
					return dragdrop::FileKind::Material;
				}
			}
			return dragdrop::FileKind::Unknown;
		}

		SceneSelection::AssetKind ToSelectionKind(dragdrop::FileKind kind)
		{
			switch (kind)
			{
				case dragdrop::FileKind::Model:
					return SceneSelection::AssetKind::Model;
				case dragdrop::FileKind::Material:
					return SceneSelection::AssetKind::Material;
				case dragdrop::FileKind::Texture:
					return SceneSelection::AssetKind::Texture;
				case dragdrop::FileKind::Script:
					return SceneSelection::AssetKind::Script;
				case dragdrop::FileKind::Prefab:
					return SceneSelection::AssetKind::Prefab;
				case dragdrop::FileKind::Scene:
					return SceneSelection::AssetKind::Scene;
				case dragdrop::FileKind::Unknown:
				default:
					return SceneSelection::AssetKind::File;
			}
		}

		std::string TrimCopy(std::string_view text)
		{
			size_t first = 0;
			while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0)
			{
				++first;
			}
			size_t last = text.size();
			while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0)
			{
				--last;
			}
			return std::string(text.substr(first, last - first));
		}

		bool IsValidCSharpIdentifier(std::string_view name)
		{
			if (name.empty())
			{
				return false;
			}
			const auto isAlpha = [](char c)
			{
				return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
			};
			const auto isAlnum = [](char c)
			{
				return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
			};
			if (!isAlpha(name.front()) || !std::all_of(name.begin() + 1, name.end(), isAlnum))
			{
				return false;
			}
			constexpr std::string_view kKeywords[] = {"abstract",
			        "as",
			        "base",
			        "bool",
			        "break",
			        "byte",
			        "case",
			        "catch",
			        "char",
			        "checked",
			        "class",
			        "const",
			        "continue",
			        "decimal",
			        "default",
			        "delegate",
			        "do",
			        "double",
			        "else",
			        "enum",
			        "event",
			        "explicit",
			        "extern",
			        "false",
			        "finally",
			        "fixed",
			        "float",
			        "for",
			        "foreach",
			        "goto",
			        "if",
			        "implicit",
			        "in",
			        "int",
			        "interface",
			        "internal",
			        "is",
			        "lock",
			        "long",
			        "namespace",
			        "new",
			        "null",
			        "object",
			        "operator",
			        "out",
			        "override",
			        "params",
			        "private",
			        "protected",
			        "public",
			        "readonly",
			        "record",
			        "ref",
			        "return",
			        "sbyte",
			        "sealed",
			        "short",
			        "sizeof",
			        "stackalloc",
			        "static",
			        "string",
			        "struct",
			        "switch",
			        "this",
			        "throw",
			        "true",
			        "try",
			        "typeof",
			        "uint",
			        "ulong",
			        "unchecked",
			        "unsafe",
			        "ushort",
			        "using",
			        "virtual",
			        "void",
			        "volatile",
			        "while"};
			return std::find(kKeywords, kKeywords + (sizeof(kKeywords) / sizeof(kKeywords[0])), name) == kKeywords + (sizeof(kKeywords) / sizeof(kKeywords[0]));
		}

		std::string BuildScriptTemplate(std::string_view typeName)
		{
			std::string src;
			src += "using AetherCore;\n\n";
			src += "namespace AetherGame;\n\n";
			src += "public sealed class ";
			src += typeName;
			src += " : EntityScript\n";
			src += "{\n";
			src += "\tpublic override void OnAttach()\n";
			src += "\t{\n";
			src += "\t}\n\n";
			src += "\tpublic override void OnUpdate(float deltaTime)\n";
			src += "\t{\n";
			src += "\t}\n";
			src += "}\n";
			return src;
		}

		bool CreateGameScriptFile(const std::filesystem::path& scriptDir, std::string_view typeName, std::string& error)
		{
			std::error_code ec;
			std::filesystem::create_directories(scriptDir, ec);
			if (ec)
			{
				error = "Could not create script directory: " + ec.message();
				return false;
			}

			const std::filesystem::path outPath = scriptDir / (std::string(typeName) + ".cs");
			if (std::filesystem::exists(outPath))
			{
				error = "A script file with that name already exists.";
				return false;
			}

			std::ofstream out(outPath, std::ios::binary);
			if (!out)
			{
				error = "Could not open the script file for writing.";
				return false;
			}
			out << BuildScriptTemplate(typeName);
			if (!out)
			{
				error = "Could not write the script file.";
				return false;
			}
			return true;
		}

		void DrawPayloadPreview(const char* icon, const char* title, const char* detail, ImU32 iconColor)
		{
			const ImVec2 padding(10.0f, 7.0f);
			const ImVec2 gap(7.0f, 0.0f);
			const ImVec2 mouse = ImGui::GetMousePos();
			const ImVec2 iconSize = ImGui::CalcTextSize(icon);
			const ImVec2 titleSize = ImGui::CalcTextSize(title);
			const ImVec2 detailSize = detail != nullptr && detail[0] != '\0' ? ImGui::CalcTextSize(detail) : ImVec2(0.0f, 0.0f);
			const bool hasDetail = detailSize.x > 0.0f;
			const float lineGap = hasDetail ? 3.0f : 0.0f;
			const float textHeight = titleSize.y + (hasDetail ? detailSize.y + lineGap : 0.0f);
			const float height = std::max(ImGui::GetFrameHeight(), textHeight) + padding.y * 2.0f;
			const float textWidth = std::max(titleSize.x, detailSize.x);
			const float width = padding.x * 2.0f + iconSize.x + gap.x + textWidth;
			const ImVec2 min(mouse.x + 16.0f, mouse.y + 18.0f);
			const ImVec2 max(min.x + width, min.y + height);
			const ImVec2 textPos(min.x + padding.x, min.y + (height - textHeight) * 0.5f);
			const float titleX = textPos.x + iconSize.x + gap.x;

			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			drawList->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f), IM_COL32(0, 0, 0, 95), 5.0f);
			drawList->AddRectFilled(min, max, IM_COL32(31, 34, 40, 238), 5.0f);
			drawList->AddRect(min, max, IM_COL32(105, 170, 255, 185), 5.0f, 0, 1.0f);
			drawList->AddText(textPos, iconColor, icon);
			drawList->AddText(ImVec2(titleX, textPos.y), ImGui::GetColorU32(ImGuiCol_Text), title);
			if (hasDetail)
			{
				drawList->AddText(ImVec2(titleX, textPos.y + titleSize.y + lineGap), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail);
			}
		}
	} // namespace

	void FileExplorerPanel::OnAttach(LayerContext&)
	{
		RefreshRoot();
	}

	void FileExplorerPanel::RefreshRoot()
	{
#if defined(AETHER_GAME_PROJECT)
		m_root = std::filesystem::path(AETHER_GAME_PROJECT).parent_path();
		std::error_code ec;
		m_rootAvailable = std::filesystem::exists(m_root, ec) && std::filesystem::is_directory(m_root, ec);
#else
		m_root.clear();
		m_rootAvailable = false;
#endif
	}

	void FileExplorerPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("File Explorer", VisiblePtr());
		if (ImGui::SmallButton(ICON_FA_ROTATE "##refreshFiles"))
		{
			RefreshRoot();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_rootAvailable ? ToUtf8Path(m_root).c_str() : "Script source unavailable");

		ImGui::Separator();
		if (!m_rootAvailable)
		{
			ImGui::TextDisabled("No source project is available in this build.");
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputTextWithHint("##newScriptName", "New script name", m_newScriptNameBuf, sizeof(m_newScriptNameBuf));
		ImGui::SameLine();
		const std::string typeName = TrimCopy(m_newScriptNameBuf);
		const bool validName = IsValidCSharpIdentifier(typeName);
		ImGui::BeginDisabled(!validName);
		if (ImGui::Button(ICON_FA_PLUS "  Create"))
		{
			m_newScriptError.clear();
			if (CreateGameScriptFile(m_root, typeName, m_newScriptError))
			{
				m_newScriptNameBuf[0] = '\0';
				RefreshRoot();
			}
		}
		ImGui::EndDisabled();
		if (!validName && !typeName.empty())
		{
			ImGui::TextDisabled("Use a C# class name, e.g. PlayerMotor");
		}
		if (!m_newScriptError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", m_newScriptError.c_str());
		}
		ImGui::Separator();

		DrawDirectory(context, m_root, 0);
		ImGui::End();
	}

	void FileExplorerPanel::DrawDirectory(LayerContext& context, const std::filesystem::path& dir, int depth)
	{
		std::error_code ec;
		std::vector<std::filesystem::directory_entry> dirs;
		std::vector<std::filesystem::directory_entry> files;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (ec)
			{
				break;
			}
			if (entry.is_directory(ec))
			{
				dirs.push_back(entry);
			}
			else if (entry.is_regular_file(ec))
			{
				files.push_back(entry);
			}
		}

		const auto byName = [](const auto& a, const auto& b)
		{
			return a.path().filename().generic_string() < b.path().filename().generic_string();
		};
		std::sort(dirs.begin(), dirs.end(), byName);
		std::sort(files.begin(), files.end(), byName);

		const std::string label = depth == 0 ? std::string(ICON_FA_FOLDER_OPEN "  AetherGame") : std::string(ICON_FA_FOLDER_OPEN "  ") + dir.filename().generic_string();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (depth == 0)
		{
			flags |= ImGuiTreeNodeFlags_DefaultOpen;
		}
		const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
		if (!open)
		{
			return;
		}

		for (const auto& child: dirs)
		{
			DrawDirectory(context, child.path(), depth + 1);
		}
		for (const auto& file: files)
		{
			DrawFile(context, file.path());
		}

		ImGui::TreePop();
	}

	void FileExplorerPanel::DrawFile(LayerContext& context, const std::filesystem::path& path)
	{
		const bool isScript = IsCSharpScriptFile(path);
		const dragdrop::FileKind kind = InferFileKind(path);
		const std::string name = path.filename().generic_string();
		const std::string label = std::string(isScript ? ICON_FA_CODE "  " : ICON_FA_IMAGE "  ") + name;
		const std::string pathText = ToUtf8Path(path);
		if (ImGui::Selectable(label.c_str(), false))
		{
			if (auto* selection = context.TryGet<SceneSelection>())
			{
				selection->SelectAsset(ToSelectionKind(kind), pathText, name);
			}
		}

		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
		{
			if (isScript)
			{
				dragdrop::ScriptPayload payload{};
				CopyToPayload(payload.typeName, path.stem().generic_string());
				CopyToPayload(payload.sourcePath, pathText);
				ImGui::SetDragDropPayload(dragdrop::kScriptPayload, &payload, sizeof(payload));
				DrawPayloadPreview(ICON_FA_CODE, payload.typeName, payload.sourcePath, IM_COL32(105, 170, 255, 255));
			}
			else
			{
				dragdrop::FilePayload payload{};
				payload.kind = kind;
				CopyToPayload(payload.path, pathText);
				CopyToPayload(payload.displayName, name);
				ImGui::SetDragDropPayload(dragdrop::kFilePayload, &payload, sizeof(payload));
				DrawPayloadPreview(ICON_FA_IMAGE, name.c_str(), payload.path, IM_COL32(168, 179, 196, 255));
			}
			ImGui::EndDragDropSource();
		}
	}
} // namespace aether::app
