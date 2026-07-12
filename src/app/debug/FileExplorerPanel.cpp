#include "debug/FileExplorerPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "io/FileUtil.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/EditorProjectContext.hpp"
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

		bool IsSubpath(const std::filesystem::path& relativePath)
		{
			const std::string generic = relativePath.generic_string();
			return !generic.empty() && !relativePath.is_absolute() && generic != "." && !generic.starts_with("../") && generic != "..";
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
			if (ext == ".mesh" || ext == ".gltf" || ext == ".glb")
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
			if (!io::file_util::CreateDirectories(scriptDir))
			{
				error = "Could not create script directory.";
				return false;
			}

			const std::filesystem::path outPath = scriptDir / (std::string(typeName) + ".cs");
			if (io::file_util::Exists(outPath))
			{
				error = "A script file with that name already exists.";
				return false;
			}

			if (!io::file_util::WriteText(outPath, BuildScriptTemplate(typeName)))
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
			drawList->AddRectFilled(min, max, chrome::U32(chrome::kDragGhostBg), 5.0f);
			drawList->AddRect(min, max, chrome::U32(chrome::kDragGhostBorder), 5.0f, 0, 1.0f);
			drawList->AddText(textPos, iconColor, icon);
			drawList->AddText(ImVec2(titleX, textPos.y), ImGui::GetColorU32(ImGuiCol_Text), title);
			if (hasDetail)
			{
				drawList->AddText(ImVec2(titleX, textPos.y + titleSize.y + lineGap), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail);
			}
		}
	} // namespace

	void FileExplorerPanel::OnAttach(LayerContext& context)
	{
		RefreshRoot(context);
	}

	void FileExplorerPanel::RefreshRoot(LayerContext& context)
	{
		const auto* project = context.TryGet<EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			m_root.clear();
			m_scriptRoot.clear();
			m_projectName.clear();
			m_rootAvailable = false;
			return;
		}

		m_root = project->root;
		m_scriptRoot = project->scriptsDir;
		m_projectName = project->name.empty() ? project->root.filename().generic_string() : project->name;
		std::error_code ec;
		m_rootAvailable = std::filesystem::exists(m_root, ec) && std::filesystem::is_directory(m_root, ec);
	}

	void FileExplorerPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (const auto* project = context.TryGet<EditorProjectContext>(); project == nullptr || !project->IsLoaded() || project->root != m_root)
		{
			RefreshRoot(context);
		}

		ImGui::Begin("File Explorer", VisiblePtr());
		if (ImGui::SmallButton(ICON_FA_ROTATE "##refreshFiles"))
		{
			RefreshRoot(context);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_rootAvailable ? ToUtf8Path(m_root).c_str() : "Open a project to browse files");

		ImGui::Separator();
		if (!m_rootAvailable)
		{
			ImGui::TextDisabled("No project is open.");
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
			if (CreateGameScriptFile(m_scriptRoot.empty() ? m_root / "scripts" : m_scriptRoot, typeName, m_newScriptError))
			{
				m_newScriptNameBuf[0] = '\0';
				RefreshRoot(context);
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

		const std::string label = depth == 0 ? std::string(ICON_FA_FOLDER_OPEN "  ") + (m_projectName.empty() ? dir.filename().generic_string() : m_projectName) : std::string(ICON_FA_FOLDER_OPEN "  ") + dir.filename().generic_string();
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
		std::string payloadPath = pathText;
		if (!isScript && !m_root.empty())
		{
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, m_root, ec);
			if (!ec && IsSubpath(relative))
			{
				payloadPath = "project://" + relative.generic_string();
			}
		}
		if (ImGui::Selectable(label.c_str(), false))
		{
			if (auto* selection = context.TryGet<SceneSelection>())
			{
				selection->SelectAsset(ToSelectionKind(kind), isScript ? pathText : payloadPath, name);
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
				DrawPayloadPreview(ICON_FA_CODE, payload.typeName, payload.sourcePath, chrome::U32(chrome::kAccentHi));
			}
			else
			{
				dragdrop::FilePayload payload{};
				payload.kind = kind;
				CopyToPayload(payload.path, payloadPath);
				CopyToPayload(payload.displayName, name);
				ImGui::SetDragDropPayload(dragdrop::kFilePayload, &payload, sizeof(payload));
				DrawPayloadPreview(ICON_FA_IMAGE, name.c_str(), payload.path, chrome::U32(chrome::kMuted));
			}
			ImGui::EndDragDropSource();
		}
	}
} // namespace aether::app
