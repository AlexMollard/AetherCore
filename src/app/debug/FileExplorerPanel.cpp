#include "debug/FileExplorerPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>

#	include <shellapi.h>
#endif

#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/OpenInEditor.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/ModelImport.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/FileUtil.hpp"
#include "material/MaterialSerializer.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr double kAutoRescanSeconds = 4.0;
		constexpr int kMaxScanDepth = 24;

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

		dragdrop::FileKind InferFileKind(const std::filesystem::path& path)
		{
			return dragdrop::ClassifyFile(path);
		}

		dragdrop::FileKind FromSelectionKind(SceneSelection::AssetKind kind)
		{
			switch (kind)
			{
				case SceneSelection::AssetKind::Model:
					return dragdrop::FileKind::Model;
				case SceneSelection::AssetKind::Material:
					return dragdrop::FileKind::Material;
				case SceneSelection::AssetKind::Texture:
					return dragdrop::FileKind::Texture;
				case SceneSelection::AssetKind::Script:
					return dragdrop::FileKind::Script;
				case SceneSelection::AssetKind::Prefab:
					return dragdrop::FileKind::Prefab;
				case SceneSelection::AssetKind::Scene:
					return dragdrop::FileKind::Scene;
				case SceneSelection::AssetKind::None:
				case SceneSelection::AssetKind::File:
				default:
					return dragdrop::FileKind::Unknown;
			}
		}

		// (semantic identity in a quiet tint, never generic-editor primaries).
		const char* KindIcon(dragdrop::FileKind kind, bool isScript)
		{
			if (isScript)
			{
				return ICON_FA_CODE;
			}
			switch (kind)
			{
				case dragdrop::FileKind::Model:
					return ICON_FA_CUBE;
				case dragdrop::FileKind::Texture:
					return ICON_FA_IMAGE;
				case dragdrop::FileKind::Prefab:
					return ICON_FA_BOX_OPEN;
				case dragdrop::FileKind::Scene:
					return ICON_FA_FILM;
				case dragdrop::FileKind::Material:
					return ICON_FA_PALETTE;
				case dragdrop::FileKind::Script:
					return ICON_FA_CODE;
				case dragdrop::FileKind::Shader:
					return ICON_FA_WAND_MAGIC_SPARKLES;
				case dragdrop::FileKind::Unknown:
				default:
					return ICON_FA_FILE;
			}
		}

		ImVec4 KindTint(dragdrop::FileKind kind, bool isScript)
		{
			if (isScript || kind == dragdrop::FileKind::Script)
			{
				return chrome::kAccentHi;
			}
			switch (kind)
			{
				case dragdrop::FileKind::Model:
					return ImVec4(0.62f, 0.84f, 0.60f, 1.0f);
				case dragdrop::FileKind::Texture:
					return ImVec4(0.74f, 0.68f, 0.58f, 1.0f);
				case dragdrop::FileKind::Prefab:
					return ImVec4(0.88f, 0.72f, 0.45f, 1.0f);
				case dragdrop::FileKind::Scene:
					return ImVec4(0.78f, 0.62f, 0.92f, 1.0f);
				case dragdrop::FileKind::Material:
					return ImVec4(0.92f, 0.62f, 0.55f, 1.0f);
				case dragdrop::FileKind::Script:
					return chrome::kAccentHi;
				case dragdrop::FileKind::Shader:
					return ImVec4(0.48f, 0.80f, 0.96f, 1.0f);
				case dragdrop::FileKind::Unknown:
				default:
					return chrome::kFaint;
			}
		}

		bool IsTextPreviewable(const std::filesystem::path& path)
		{
			std::string ext = path.extension().generic_string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return ext == ".cs" || ext == ".toml" || ext == ".md" || ext == ".txt" || ext == ".json" || ext == ".xml" || ext == ".csproj" || ext == ".slang" || ext == ".slangh" || ext == ".hlsl" || ext == ".glsl" || ext == ".ini";
		}

		const char* KindLabel(dragdrop::FileKind kind)
		{
			switch (kind)
			{
				case dragdrop::FileKind::Model:
					return "MODEL";
				case dragdrop::FileKind::Script:
					return "SCRIPT";
				case dragdrop::FileKind::Texture:
					return "TEXTURE";
				case dragdrop::FileKind::Prefab:
					return "PREFAB";
				case dragdrop::FileKind::Scene:
					return "SCENE";
				case dragdrop::FileKind::Material:
					return "MATERIAL";
				case dragdrop::FileKind::Shader:
					return "SHADER";
				case dragdrop::FileKind::Unknown:
				default:
					return "FILE";
			}
		}

		std::string FormatSize(std::uint64_t bytes)
		{
			char buf[32];
			if (bytes < 1024ull)
			{
				std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
			}
			else if (bytes < 1024ull * 1024ull)
			{
				std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
			}
			else
			{
				std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
			}
			return buf;
		}

		bool IsIgnoredDirectory(const std::string& name)
		{
			return name == "bin" || name == "obj" || name == ".git" || name == ".vs" || name == ".idea" || name == ".vscode";
		}

		void OpenInOS(const std::filesystem::path& path)
		{
#ifdef _WIN32
			ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			std::system(("xdg-open \"" + path.string() + "\" &").c_str());
#endif
		}

		void ShowInOSExplorer(const std::filesystem::path& path)
		{
#ifdef _WIN32
			const std::string args = "/select,\"" + path.string() + "\"";
			ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#else
			std::system(("xdg-open \"" + path.parent_path().string() + "\" &").c_str());
#endif
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

		bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needleLower)
		{
			if (needleLower.empty())
			{
				return true;
			}
			if (haystack.size() < needleLower.size())
			{
				return false;
			}
			for (std::size_t i = 0; i + needleLower.size() <= haystack.size(); ++i)
			{
				std::size_t j = 0;
				for (; j < needleLower.size(); ++j)
				{
					if (static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j]))) != needleLower[j])
					{
						break;
					}
				}
				if (j == needleLower.size())
				{
					return true;
				}
			}
			return false;
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
			return std::find(std::begin(kKeywords), std::end(kKeywords), name) == std::end(kKeywords);
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
				error = "Could not create the script directory.";
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

	void FileExplorerPanel::OnAttach(app::LayerContext& context)
	{
		RefreshRoot(context);
	}

	void FileExplorerPanel::OnDetach(app::LayerContext& context)
	{
		ReleaseThumbnails(context);
	}

	void FileExplorerPanel::RefreshRoot(app::LayerContext& context)
	{
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			m_root.clear();
			m_scriptRoot.clear();
			m_projectName.clear();
			m_rootAvailable = false;
			m_tree = Entry{};
			ReleaseThumbnails(context);
			m_currentDir.clear();
			return;
		}

		m_root = project->root;
		m_scriptRoot = project->scriptsDir;
		m_projectName = project->name.empty() ? project->root.filename().generic_string() : project->name;
		std::error_code ec;
		m_rootAvailable = std::filesystem::exists(m_root, ec) && std::filesystem::is_directory(m_root, ec);
		m_createDir = m_root;
		ReleaseThumbnails(context);
		m_currentDir = m_root;
		m_treeDirty = true;
	}

	void FileExplorerPanel::RescanTree()
	{
		AE_PROFILE_ZONE();
		m_tree = Entry{};
		m_tree.path = m_root;
		m_tree.name = m_projectName;
		m_tree.isDirectory = true;
		m_fileCount = 0;
		m_dirCount = 0;
		m_scanError.clear();
		if (m_rootAvailable)
		{
			ScanDirectory(m_root, m_tree, 0);
		}
		m_lastScanTime = ImGui::GetTime();
		m_treeDirty = false;
	}

	void FileExplorerPanel::ScanDirectory(const std::filesystem::path& dir, Entry& out, const int depth)
	{
		if (depth > kMaxScanDepth)
		{
			return;
		}

		std::error_code ec;
		std::vector<std::filesystem::directory_entry> dirs;
		std::vector<std::filesystem::directory_entry> files;
		for (const auto& entry: std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec))
		{
			if (ec)
			{
				break;
			}
			std::error_code typeEc;
			if (entry.is_directory(typeEc))
			{
				if (!IsIgnoredDirectory(entry.path().filename().generic_string()))
				{
					dirs.push_back(entry);
				}
			}
			else if (entry.is_regular_file(typeEc))
			{
				files.push_back(entry);
			}
		}
		if (ec)
		{
			m_scanError = "Some entries could not be read (" + ec.message() + ").";
		}

		const auto byNameNoCase = [](const auto& a, const auto& b)
		{
			const std::string an = a.path().filename().generic_string();
			const std::string bn = b.path().filename().generic_string();
			return std::lexicographical_compare(an.begin(), an.end(), bn.begin(), bn.end(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
		};
		std::sort(dirs.begin(), dirs.end(), byNameNoCase);
		std::sort(files.begin(), files.end(), byNameNoCase);

		out.children.reserve(dirs.size() + files.size());
		for (const auto& child: dirs)
		{
			Entry e;
			e.path = child.path();
			e.name = child.path().filename().generic_string();
			e.isDirectory = true;
			++m_dirCount;
			ScanDirectory(child.path(), e, depth + 1);
			out.children.push_back(std::move(e));
		}
		for (const auto& file: files)
		{
			Entry e;
			e.path = file.path();
			e.name = file.path().filename().generic_string();
			e.kind = InferFileKind(e.path);
			std::error_code sizeEc;
			e.sizeBytes = file.file_size(sizeEc);

			e.payloadPath = ToUtf8Path(e.path);
			if (e.kind != dragdrop::FileKind::Script && !m_root.empty())
			{
				std::error_code relEc;
				const std::filesystem::path relative = std::filesystem::relative(e.path, m_root, relEc);
				if (!relEc && IsSubpath(relative))
				{
					e.payloadPath = "project://" + relative.generic_string();
				}
			}
			++m_fileCount;
			out.children.push_back(std::move(e));
		}
	}

	void FileExplorerPanel::BeginRename(const Entry& entry)
	{
		m_renameTarget = entry.path;
		std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", entry.name.c_str());
		m_renameFocusPending = true;
	}

	namespace
	{
		// Whole quoted paths only, so a reference to "Rock_Wet" is not counted as one to
		// "Rock" - the same rule the rewrite uses, kept here so the two cannot disagree.
		int CountQuotedRefs(const std::string& text, const std::string& vfs)
		{
			if (vfs.empty())
			{
				return 0;
			}
			int found = 0;
			for (std::size_t at = text.find(vfs); at != std::string::npos; at = text.find(vfs, at + vfs.size()))
			{
				const std::size_t after = at + vfs.size();
				if (after < text.size() && (text[after] == '\'' || text[after] == '"'))
				{
					++found;
				}
			}
			return found;
		}
	} // namespace

	std::string FileExplorerPanel::VfsPathFor(const std::filesystem::path& path) const
	{
		std::error_code ec;
		const std::filesystem::path rel = std::filesystem::relative(path, m_root, ec);
		if (ec || rel.empty() || rel.generic_string().starts_with(".."))
		{
			return {};
		}
		return "project://" + rel.generic_string();
	}

	int FileExplorerPanel::CountAssetReferences(const std::filesystem::path& target, const bool isDirectory) const
	{
		std::vector<std::string> paths;
		std::error_code ec;
		if (isDirectory)
		{
			for (const auto& entry: std::filesystem::recursive_directory_iterator(target, ec))
			{
				if (ec)
				{
					break;
				}
				if (entry.is_regular_file(ec))
				{
					if (std::string vfs = VfsPathFor(entry.path()); !vfs.empty())
					{
						paths.push_back(std::move(vfs));
					}
				}
			}
		}
		else if (std::string vfs = VfsPathFor(target); !vfs.empty())
		{
			paths.push_back(std::move(vfs));
		}
		if (paths.empty())
		{
			return 0;
		}

		int references = 0;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(m_root, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".toml")
			{
				continue;
			}
			// A file inside the doomed directory referring to a sibling is going away too.
			if (isDirectory && entry.path().generic_string().starts_with(target.generic_string()))
			{
				continue;
			}
			const auto text = io::file_util::ReadText(entry.path());
			if (!text)
			{
				continue;
			}
			for (const std::string& vfs: paths)
			{
				references += CountQuotedRefs(*text, vfs);
			}
		}
		return references;
	}

	int FileExplorerPanel::RetargetAssetReferences(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
	{
		// Scenes, prefabs and materials refer to assets by "project://" string. Renaming only
		// the file left all of them pointing at nothing: the scene still loaded, but an entity
		// inheriting from the asset fell back to defaults with only a log line to say so.
		std::error_code ec;
		const std::filesystem::path oldRel = std::filesystem::relative(oldPath, m_root, ec);
		const std::filesystem::path newRel = std::filesystem::relative(newPath, m_root, ec);
		if (ec || oldRel.empty() || newRel.empty())
		{
			return 0;
		}
		const std::string oldVfs = "project://" + oldRel.generic_string();
		const std::string newVfs = "project://" + newRel.generic_string();
		if (oldVfs == newVfs)
		{
			return 0;
		}

		int rewritten = 0;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(m_root, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".toml")
			{
				continue;
			}
			const auto text = io::file_util::ReadText(entry.path());
			if (!text)
			{
				continue;
			}
			std::string updated = *text;
			bool changed = false;
			for (std::size_t at = updated.find(oldVfs); at != std::string::npos; at = updated.find(oldVfs, at + newVfs.size()))
			{
				// Only a whole quoted path: without this, renaming "Rock" would also rewrite a
				// reference to "Rock_Wet" that merely starts with the same text.
				const std::size_t after = at + oldVfs.size();
				if (after >= updated.size() || (updated[after] != '\'' && updated[after] != '"'))
				{
					continue;
				}
				updated.replace(at, oldVfs.size(), newVfs);
				changed = true;
			}
			if (!changed || !io::file_util::WriteText(entry.path(), updated))
			{
				continue;
			}
			++rewritten;
			// The cooked sibling still holds the old path and is preferred over the .toml, so
			// leaving it behind would undo the rewrite on the next load.
			for (const std::string_view cooked: {".scene.bin", ".prefab.bin"})
			{
				const std::string name = entry.path().filename().generic_string();
				const std::string_view suffix = cooked == ".scene.bin" ? ".scene.toml" : ".prefab.toml";
				if (!name.ends_with(suffix))
				{
					continue;
				}
				std::filesystem::path bin = entry.path();
				bin.replace_filename(name.substr(0, name.size() - suffix.size()) + std::string(cooked));
				std::error_code removeEc;
				std::filesystem::remove(bin, removeEc);
			}
		}
		if (rewritten > 0)
		{
			AE_INFO(LogCategory::App, "Renamed '{}' -> '{}'; updated references in {} file(s).", oldVfs, newVfs, rewritten);
		}
		return rewritten;
	}

	bool FileExplorerPanel::ApplyRename(const std::filesystem::path& target, std::string_view newName)
	{
		const std::string trimmed = TrimCopy(newName);
		if (trimmed.empty() || trimmed.contains('/') || trimmed.contains('\\'))
		{
			m_opError = "Enter a plain file name (no path separators).";
			return false;
		}
		const std::filesystem::path newPath = target.parent_path() / trimmed;
		if (newPath == target)
		{
			return true;
		}
		std::error_code ec;
		if (std::filesystem::exists(newPath, ec))
		{
			m_opError = "Something with that name already exists.";
			return false;
		}
		std::filesystem::rename(target, newPath, ec);
		if (ec)
		{
			m_opError = "Rename failed (" + ec.message() + ").";
			return false;
		}
		// After the move, so a failed rename cannot leave references pointing at a file that
		// was never renamed.
		RetargetAssetReferences(target, newPath);
		m_opError.clear();
		m_selectedPath = ToUtf8Path(newPath);
		m_treeDirty = true;
		return true;
	}

	bool FileExplorerPanel::DuplicateEntry(const std::filesystem::path& target)
	{
		const std::filesystem::path parent = target.parent_path();
		const std::string stem = target.stem().generic_string();
		const std::string ext = target.extension().generic_string();
		std::filesystem::path copyPath;
		for (int i = 1; i <= 32; ++i)
		{
			const std::string suffix = (i == 1) ? " Copy" : (" Copy " + std::to_string(i));
			std::string copyName = stem;
			copyName += suffix;
			copyName += ext;
			copyPath = parent / copyName;
			std::error_code existsEc;
			if (!std::filesystem::exists(copyPath, existsEc))
			{
				break;
			}
			copyPath.clear();
		}
		if (copyPath.empty())
		{
			m_opError = "Too many copies already exist.";
			return false;
		}
		std::error_code ec;
		std::filesystem::copy_file(target, copyPath, ec);
		if (ec)
		{
			m_opError = "Duplicate failed (" + ec.message() + ").";
			return false;
		}
		m_opError.clear();
		m_selectedPath = ToUtf8Path(copyPath);
		m_treeDirty = true;
		return true;
	}

	bool FileExplorerPanel::DeleteEntry(const std::filesystem::path& target, const bool isDirectory)
	{
		std::error_code ec;
		if (isDirectory)
		{
			std::filesystem::remove_all(target, ec);
		}
		else
		{
			std::filesystem::remove(target, ec);
		}
		if (ec)
		{
			m_opError = "Delete failed (" + ec.message() + ").";
			return false;
		}
		m_opError.clear();
		if (m_selectedPath == ToUtf8Path(target))
		{
			m_selectedPath.clear();
		}
		m_treeDirty = true;
		return true;
	}

	void FileExplorerPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (const auto* project = context.TryGet<app::EditorProjectContext>(); project == nullptr || !project->IsLoaded() || project->root != m_root)
		{
			RefreshRoot(context);
		}

		ImGui::Begin("File Explorer", VisiblePtr());

		char stat[64]{};
		if (m_rootAvailable)
		{
			std::snprintf(stat, sizeof(stat), "%d FILES \xC2\xB7 %d FOLDERS", m_fileCount, m_dirCount);
		}
		chrome::PanelHeader("PROJECT FILES", stat);

		if (!m_rootAvailable)
		{
			ImGui::TextDisabled("Open a project to browse its files.");
			ImGui::End();
			return;
		}

		if (auto* selection = context.TryGet<SceneSelection>(); selection != nullptr && selection->HasAsset())
		{
			const auto& asset = selection->SelectedAsset();
			if (asset.path != m_lastAdoptedAsset)
			{
				m_lastAdoptedAsset = asset.path;
				if (asset.path != m_selectedPath && asset.path != m_selectedPayloadPath)
				{
					m_selectedPath = asset.path;
					m_selectedPayloadPath = asset.path;
					m_selectedKind = FromSelectionKind(asset.kind);
					m_selectedIsDirectory = false;
				}
			}
		}

		if (!m_treeDirty && ImGui::GetTime() - m_lastScanTime > kAutoRescanSeconds)
		{
			m_treeDirty = true;
		}
		if (m_treeDirty)
		{
			RescanTree();
		}

		m_thumbnailLoadsThisFrame = 0;

		DrawToolbar(context);

		if (!m_opError.empty() || !m_scanError.empty())
		{
			ImGui::TextColored(chrome::C(colors::Error), "%s", !m_opError.empty() ? m_opError.c_str() : m_scanError.c_str());
		}

		ImGui::PushStyleColor(ImGuiCol_Header, chrome::kSelectionBg);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, chrome::kHoverBg);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, chrome::WithAlpha(chrome::kAccent, 0.35f));

		// Folders on the left, the open folder's contents on the right. A resizable table is
		// the splitter: ImGui already remembers the column width per user, so there is no
		// bespoke drag handle or saved setting to maintain.
		const ImGuiTableFlags splitFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
		ImGui::BeginChild("##feRows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		if (m_search[0] != '\0')
		{
			// A search is about finding a file anywhere, so it takes the whole pane and shows
			// matches from the entire project rather than only the open folder.
			DrawSearchResults(context, m_tree);
		}
		// A table cell does not know its own height while it is being filled, so a child sized
		// 0 inside one collapses to nothing. Both panes are given the height measured before
		// the table instead.
		else if (const float paneH = ImGui::GetContentRegionAvail().y; ImGui::BeginTable("##feSplit", 2, splitFlags))
		{
			ImGui::TableSetupColumn("##folders", ImGuiTableColumnFlags_WidthFixed, 190.0f);
			ImGui::TableSetupColumn("##contents", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::BeginChild("##feFolders", ImVec2(0.0f, paneH));
			DrawFolderTree(context, m_tree);
			ImGui::EndChild();

			ImGui::TableSetColumnIndex(1);
			const float contentsTop = ImGui::GetCursorPosY();
			DrawBreadcrumb(context);
			// Right-aligned from what is left in this cell. GetContentRegionMax is relative to
			// the window, not the table cell, so using it here put the controls off-screen.
			const float controlsW = m_viewMode == ViewMode::Grid ? 172.0f : 40.0f;
			const float slack = ImGui::GetContentRegionAvail().x - controlsW;
			ImGui::SameLine(0.0f, std::max(4.0f, slack));
			if (chrome::GhostButton(m_viewMode == ViewMode::Grid ? ICON_FA_LIST : ICON_FA_TABLE_CELLS_LARGE))
			{
				m_viewMode = m_viewMode == ViewMode::Grid ? ViewMode::List : ViewMode::Grid;
			}
			ImGui::SetItemTooltip(m_viewMode == ViewMode::Grid ? "Switch to list view" : "Switch to grid view");
			if (m_viewMode == ViewMode::Grid)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(110.0f);
				ImGui::SliderFloat("##feTileSize", &m_tileSize, 56.0f, 160.0f, "%.0f px");
				ImGui::SetItemTooltip("Thumbnail size");
			}
			const float used = ImGui::GetCursorPosY() - contentsTop + ImGui::GetTextLineHeightWithSpacing();
			ImGui::BeginChild("##feContents", ImVec2(0.0f, std::max(40.0f, paneH - used)));
			DrawFolderContents(context);
			ImGui::EndChild();

			ImGui::EndTable();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor(3);

		DrawPendingPopups(context);
		ImGui::End();
	}

	void FileExplorerPanel::DrawToolbar(app::LayerContext& context)
	{
		(void) context;
		if (m_openNewPopup)
		{
			ImGui::OpenPopup("##feNew");
			m_openNewPopup = false;
		}
		if (chrome::OutlineButton(ICON_FA_PLUS " New"))
		{
			if (m_createDir.empty())
			{
				m_createDir = m_root;
			}
			ImGui::OpenPopup("##feNew");
		}
		ImGui::SetItemTooltip("Create a script, material or folder (in the selected folder)");

		if (ImGui::BeginPopup("##feNew"))
		{
			chrome::SectionTag("CREATE");
			std::error_code relEc;
			const std::filesystem::path rel = std::filesystem::relative(m_createDir, m_root, relEc);
			ImGui::TextDisabled("in %s", (!relEc && IsSubpath(rel)) ? rel.generic_string().c_str() : m_projectName.c_str());
			ImGui::Spacing();

			ImGui::SetNextItemWidth(220.0f);
			const bool scriptEntered = ImGui::InputTextWithHint("##feScript", "Script class name...", m_newScriptNameBuf, sizeof(m_newScriptNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const std::string typeName = TrimCopy(m_newScriptNameBuf);
			const bool validName = IsValidCSharpIdentifier(typeName);
			ImGui::BeginDisabled(!validName);
			if (chrome::GhostButton(ICON_FA_CODE " Script", ImVec2(0.0f, 0.0f), chrome::kAccentHi) || (scriptEntered && validName))
			{
				std::string error;
				const std::filesystem::path dir = m_scriptRoot.empty() ? m_root / "scripts" : m_scriptRoot;
				if (CreateGameScriptFile(dir, typeName, error))
				{
					m_newScriptNameBuf[0] = '\0';
					m_opError.clear();
					m_treeDirty = true;
					ImGui::CloseCurrentPopup();
				}
				else
				{
					m_opError = error;
				}
			}
			ImGui::EndDisabled();
			if (!validName && !typeName.empty())
			{
				ImGui::TextDisabled("Use a C# class name, e.g. PlayerMotor");
			}

			ImGui::SetNextItemWidth(220.0f);
			const bool materialEntered = ImGui::InputTextWithHint("##feMaterial", "Material name...", m_newMaterialNameBuf, sizeof(m_newMaterialNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const std::string materialName = TrimCopy(m_newMaterialNameBuf);
			const bool validMaterial = !materialName.empty() && !materialName.contains('/') && !materialName.contains('\\');
			ImGui::BeginDisabled(!validMaterial);
			if (chrome::GhostButton(ICON_FA_PALETTE " Material") || (materialEntered && validMaterial))
			{
				// A default MaterialAsset, written through the same serializer the inspector
				// saves with, so a new material and an edited one are the same file shape.
				const std::filesystem::path file = m_createDir / (materialName + ".material.toml");
				if (io::file_util::Exists(file))
				{
					m_opError = "A material of that name is already here.";
				}
				else if (io::file_util::WriteText(file, MaterialSerializer::ToToml(MaterialPresetSpec{})))
				{
					m_newMaterialNameBuf[0] = '\0';
					m_opError.clear();
					m_treeDirty = true;
					ImGui::CloseCurrentPopup();
				}
				else
				{
					m_opError = "Could not create the material.";
				}
			}
			ImGui::EndDisabled();

			ImGui::SetNextItemWidth(220.0f);
			const bool folderEntered = ImGui::InputTextWithHint("##feFolder", "Folder name...", m_newFolderNameBuf, sizeof(m_newFolderNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const std::string folderName = TrimCopy(m_newFolderNameBuf);
			const bool validFolder = !folderName.empty() && !folderName.contains('/') && !folderName.contains('\\');
			ImGui::BeginDisabled(!validFolder);
			if (chrome::GhostButton(ICON_FA_FOLDER " Folder") || (folderEntered && validFolder))
			{
				if (io::file_util::CreateDirectories(m_createDir / folderName))
				{
					m_newFolderNameBuf[0] = '\0';
					m_opError.clear();
					m_treeDirty = true;
					ImGui::CloseCurrentPopup();
				}
				else
				{
					m_opError = "Could not create the folder.";
				}
			}
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		const float btnH = ImGui::GetFrameHeight();
		const float refreshW = std::max(btnH, ImGui::CalcTextSize(ICON_FA_ROTATE).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (chrome::GhostIconButton(ICON_FA_ROTATE, "##feRefresh", ImVec2(refreshW, btnH)))
		{
			m_treeDirty = true;
		}
		ImGui::SetItemTooltip("Rescan (auto-rescans every few seconds)");

		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##feSearch", "Search files...", m_search, sizeof(m_search));
	}

	// Draws the inline rename input when `entry` is the active rename target. Returns
	// true when it consumed the row (the caller pushed an ID and must PopID + return).
	bool FileExplorerPanel::DrawActiveRename(const Entry& entry)
	{
		if (m_renameTarget != entry.path)
		{
			return false;
		}
		ImGui::SetNextItemWidth(-1.0f);
		if (m_renameFocusPending)
		{
			ImGui::SetKeyboardFocusHere();
			m_renameFocusPending = false;
		}
		if (ImGui::InputText("##feRename", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			ApplyRename(m_renameTarget, m_renameBuf);
			m_renameTarget.clear();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()))
		{
			m_renameTarget.clear();
		}
		return true;
	}

	void FileExplorerPanel::DrawFileRow(app::LayerContext& context, const Entry& entry)
	{
		ImGui::PushID(entry.path.generic_string().c_str());

		if (DrawActiveRename(entry))
		{
			ImGui::PopID();
			return;
		}

		const bool isScript = entry.kind == dragdrop::FileKind::Script;
		const bool selected = m_selectedPath == ToUtf8Path(entry.path);
		ImGui::Selectable("##feRow", selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);

		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 rowMin = ImGui::GetItemRectMin();
			const ImVec2 rowMax = ImGui::GetItemRectMax();
			const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetFontSize()) * 0.5f;
			const float iconX = rowMin.x + 4.0f;
			const float nameX = iconX + ImGui::GetFontSize() * 1.5f;
			drawList->AddText(ImVec2(iconX, textY), chrome::U32(KindTint(entry.kind, isScript)), KindIcon(entry.kind, isScript));
			drawList->AddText(ImVec2(nameX, textY), chrome::U32(chrome::kText), entry.name.c_str());
			const std::string sizeText = FormatSize(entry.sizeBytes);
			const float sizeW = chrome::MeasureSized(12.0f, sizeText.c_str()).x;
			chrome::TextSized(drawList, 12.0f, ImVec2(rowMax.x - sizeW - 8.0f, textY + 2.0f), chrome::kFaint, sizeText.c_str());
			if (selected)
			{
				drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), chrome::U32(chrome::kSelectionBar));
			}
		}

		ApplyEntryInteractions(context, entry);
		ImGui::PopID();
	}

	void FileExplorerPanel::ApplyEntryInteractions(app::LayerContext& context, const Entry& entry)
	{
		const bool isScript = entry.kind == dragdrop::FileKind::Script;

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			m_selectedPath = ToUtf8Path(entry.path);
			m_selectedPayloadPath = entry.payloadPath;
			m_selectedKind = entry.kind;
			m_selectedIsDirectory = false;
			m_createDir = entry.path.parent_path();
			if (auto* selection = context.TryGet<SceneSelection>())
			{
				selection->SelectAsset(ToSelectionKind(entry.kind), isScript ? ToUtf8Path(entry.path) : entry.payloadPath, entry.name);
			}
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			if (isScript)
			{
				OpenInEditor(ToUtf8Path(entry.path), 0);
			}
			else
			{
				OpenInOS(entry.path);
			}
		}

		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
		{
			if (isScript)
			{
				dragdrop::ScriptPayload payload{};
				CopyToPayload(payload.typeName, entry.path.stem().generic_string());
				CopyToPayload(payload.sourcePath, ToUtf8Path(entry.path));
				ImGui::SetDragDropPayload(dragdrop::kScriptPayload, &payload, sizeof(payload));
				DrawPayloadPreview(ICON_FA_CODE, payload.typeName, payload.sourcePath, chrome::U32(chrome::kAccentHi));
			}
			else
			{
				dragdrop::FilePayload payload{};
				payload.kind = entry.kind;
				CopyToPayload(payload.path, entry.payloadPath);
				CopyToPayload(payload.displayName, entry.name);
				ImGui::SetDragDropPayload(dragdrop::kFilePayload, &payload, sizeof(payload));
				DrawPayloadPreview(KindIcon(entry.kind, false), entry.name.c_str(), payload.path, chrome::U32(KindTint(entry.kind, false)));
			}
			ImGui::EndDragDropSource();
		}

		DrawRowContextMenu(context, entry);
	}

	FileExplorerPanel::Entry* FileExplorerPanel::FindDirectory(Entry& node, const std::filesystem::path& dir)
	{
		if (node.isDirectory && node.path == dir)
		{
			return &node;
		}
		for (Entry& child: node.children)
		{
			if (!child.isDirectory)
			{
				continue;
			}
			if (Entry* found = FindDirectory(child, dir))
			{
				return found;
			}
		}
		return nullptr;
	}

	void FileExplorerPanel::OpenDirectory(app::LayerContext& context, const std::filesystem::path& dir)
	{
		if (m_currentDir == dir)
		{
			return;
		}
		// Thumbnails are scoped to the folder on screen. Keeping every folder ever visited
		// would grow without bound in a big project, and a folder's worth reloads cheaply.
		ReleaseThumbnails(context);
		m_currentDir = dir;
		m_createDir = dir;
	}

	void FileExplorerPanel::DrawFolderTree(app::LayerContext& context, Entry& entry)
	{
		ImGui::PushID(entry.path.generic_string().c_str());

		if (DrawActiveRename(entry))
		{
			ImGui::PopID();
			return;
		}

		bool hasSubdirs = false;
		for (const Entry& child: entry.children)
		{
			if (child.isDirectory)
			{
				hasSubdirs = true;
				break;
			}
		}

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap;
		if (!hasSubdirs)
		{
			flags |= ImGuiTreeNodeFlags_Leaf;
		}
		if (m_currentDir == entry.path)
		{
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		const bool open = ImGui::TreeNodeEx("##dir", flags);

		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 rowMin = ImGui::GetItemRectMin();
			const ImVec2 rowMax = ImGui::GetItemRectMax();
			const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetFontSize()) * 0.5f;
			const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing();
			const float nameX = iconX + ImGui::GetFontSize() * 1.5f;
			drawList->AddText(ImVec2(iconX, textY), chrome::U32(chrome::WithAlpha(chrome::kAccent, 0.85f)), open ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER);
			drawList->AddText(ImVec2(nameX, textY), chrome::U32(chrome::kText), entry.name.c_str());
			if (m_currentDir == entry.path)
			{
				drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), chrome::U32(chrome::kSelectionBar));
			}
		}

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			OpenDirectory(context, entry.path);
			m_selectedPath = ToUtf8Path(entry.path);
			m_selectedIsDirectory = true;
		}
		DrawRowContextMenu(context, entry);

		if (open)
		{
			for (Entry& child: entry.children)
			{
				if (child.isDirectory)
				{
					DrawFolderTree(context, child);
				}
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	void FileExplorerPanel::DrawBreadcrumb(app::LayerContext& context)
	{
		// The path back to the project root, each step clickable, so getting out of a deep
		// folder does not mean hunting for it again in the tree.
		std::vector<std::filesystem::path> crumbs;
		for (std::filesystem::path p = m_currentDir; !p.empty(); p = p.parent_path())
		{
			crumbs.push_back(p);
			if (p == m_root || p.parent_path() == p)
			{
				break;
			}
		}
		std::reverse(crumbs.begin(), crumbs.end());

		for (std::size_t i = 0; i < crumbs.size(); ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine(0.0f, 4.0f);
				ImGui::TextDisabled("/");
				ImGui::SameLine(0.0f, 4.0f);
			}
			const std::string label = crumbs[i] == m_root ? m_projectName : crumbs[i].filename().generic_string();
			ImGui::PushID(static_cast<int>(i));
			const bool last = i + 1 == crumbs.size();
			if (chrome::GhostButton(label.c_str(), ImVec2(0.0f, 0.0f), last ? chrome::kText : chrome::kMuted))
			{
				OpenDirectory(context, crumbs[i]);
			}
			ImGui::PopID();
		}
	}

	const FileExplorerPanel::Thumbnail* FileExplorerPanel::ThumbnailFor(app::LayerContext& context, const Entry& entry)
	{
		const bool isTexture = entry.kind == dragdrop::FileKind::Texture;
		const bool isMaterial = entry.kind == dragdrop::FileKind::Material;
		if (!isTexture && !isMaterial)
		{
			return nullptr;
		}
		const std::string key = ToUtf8Path(entry.path);
		if (const auto it = m_thumbnails.find(key); it != m_thumbnails.end())
		{
			return &it->second;
		}

		// Budgeted: a folder of hundreds of assets fills in over several frames rather than
		// decoding all of them on the frame it is opened.
		constexpr int kMaxLoadsPerFrame = 4;
		if (m_thumbnailLoadsThisFrame >= kMaxLoadsPerFrame)
		{
			return nullptr;
		}

		auto* assets = context.TryGet<AssetManager>();
		auto* imgui = context.TryGet<aether::ImguiSubsystem>();
		if (assets == nullptr || imgui == nullptr)
		{
			return nullptr;
		}
		++m_thumbnailLoadsThisFrame;

		Thumbnail thumb{};
		thumb.resolved = true;
		std::string texturePath = entry.payloadPath.empty() ? key : entry.payloadPath;

		if (isMaterial)
		{
			// A material shows the surface it actually describes: its albedo map, or failing
			// that its base colour. A palette glyph on every material tells you nothing about
			// which material it is.
			texturePath.clear();
			if (const auto text = io::file_util::ReadText(entry.path); text)
			{
				bool parsed = false;
				const MaterialPresetSpec spec = MaterialSerializer::Parse(key, *text, &parsed);
				if (parsed)
				{
					texturePath = spec.albedoPath;
					const glm::vec4 base = spec.material.baseColorFactor;
					thumb.swatch = IM_COL32(static_cast<int>(std::clamp(base.x, 0.0f, 1.0f) * 255.0f),
					        static_cast<int>(std::clamp(base.y, 0.0f, 1.0f) * 255.0f),
					        static_cast<int>(std::clamp(base.z, 0.0f, 1.0f) * 255.0f),
					        255);
					thumb.hasSwatch = true;
				}
			}
			// The importer writes texture names relative to the material's own folder, not as
			// virtual paths. Acquire asserts on anything without a scheme, so resolve those
			// against the material's directory and drop whatever still is not addressable.
			if (!texturePath.empty() && texturePath.find("://") == std::string::npos)
			{
				texturePath = VfsPathFor(entry.path.parent_path() / texturePath);
			}
			if (texturePath.empty() || texturePath.find("://") == std::string::npos)
			{
				m_thumbnails.emplace(key, thumb);
				return &m_thumbnails.at(key);
			}
		}

		auto& textures = assets->GetTextureRegistry();
		thumb.texture = textures.Acquire(texturePath);
		const std::uint32_t slot = textures.ResolveSlot(thumb.texture);
		const std::uint32_t fallbackSlot = textures.ResolveSlot(textures.DefaultHandle());
		if (!thumb.texture.IsValid() || slot == 0xFFFFFFFFu || slot == fallbackSlot)
		{
			// A texture that will not resolve is remembered as such, so the walk below is not
			// repeated for it every time the folder is drawn.
			if (thumb.texture.IsValid())
			{
				textures.Release(thumb.texture);
				thumb.texture = {};
			}
			m_thumbnails.emplace(key, thumb);
			return &m_thumbnails.at(key);
		}
		for (const gpu::DebugTextureInfo& info: gpu::ResourceRegistry::ListDebugTextures())
		{
			if (info.hasBindlessSampled && info.bindlessSampledSlot == slot && info.view != nullptr)
			{
				const ImTextureID id = imgui->RegisterTexture(info.view, gpu::ImageLayout::ShaderReadOnly);
				if (id != ImTextureID_Invalid)
				{
					thumb.imguiId = static_cast<std::uint64_t>(id);
				}
				break;
			}
		}
		if (thumb.imguiId == 0)
		{
			textures.Release(thumb.texture);
			thumb.texture = {};
		}
		m_thumbnails.emplace(key, thumb);
		return &m_thumbnails.at(key);
	}
	void FileExplorerPanel::ReleaseThumbnails(app::LayerContext& context)
	{
		auto* assets = context.TryGet<AssetManager>();
		auto* imgui = context.TryGet<aether::ImguiSubsystem>();
		for (auto& [key, thumb]: m_thumbnails)
		{
			(void) key;
			if (thumb.imguiId != 0 && imgui != nullptr)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(thumb.imguiId));
			}
			if (thumb.texture.IsValid() && assets != nullptr)
			{
				assets->GetTextureRegistry().Release(thumb.texture);
			}
		}
		m_thumbnails.clear();
	}

	void FileExplorerPanel::DrawFileTile(app::LayerContext& context, const Entry& entry, const float tileSize)
	{
		ImGui::PushID(entry.path.generic_string().c_str());

		const bool isScript = entry.kind == dragdrop::FileKind::Script;
		const bool selected = m_selectedPath == ToUtf8Path(entry.path);
		const float labelH = ImGui::GetFontSize() * 1.9f;
		const ImVec2 tile(tileSize, tileSize + labelH);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##tile", tile);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 tileMax(origin.x + tile.x, origin.y + tile.y);
		const ImVec2 artMin(origin.x + 4.0f, origin.y + 4.0f);
		const ImVec2 artMax(tileMax.x - 4.0f, origin.y + tileSize - 4.0f);

		if (selected || ImGui::IsItemHovered())
		{
			drawList->AddRectFilled(origin, tileMax, chrome::U32(selected ? chrome::kSelectionBg : chrome::kHoverBg), 5.0f);
		}
		if (selected)
		{
			drawList->AddRect(origin, tileMax, chrome::U32(chrome::kAccent), 5.0f, 0, 1.0f);
		}

		const Thumbnail* thumb = entry.isDirectory ? nullptr : ThumbnailFor(context, entry);
		const auto drawKindIcon = [&](const char* icon, const ImVec4& tint, const float scale)
		{
			const float iconSize = tileSize * scale;
			const ImVec2 iconExtent = chrome::MeasureSized(iconSize, icon);
			const ImVec2 centre((artMin.x + artMax.x) * 0.5f, (artMin.y + artMax.y) * 0.5f);
			chrome::TextSized(drawList, iconSize, ImVec2(centre.x - iconExtent.x * 0.5f, centre.y - iconExtent.y * 0.5f), tint, icon);
		};

		if (entry.isDirectory)
		{
			drawKindIcon(ICON_FA_FOLDER, chrome::WithAlpha(chrome::kAccent, 0.9f), 0.44f);
		}
		else if (thumb != nullptr && thumb->imguiId != 0)
		{
			drawList->AddImageRounded(static_cast<ImTextureID>(thumb->imguiId), artMin, artMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, 4.0f);
		}
		else if (thumb != nullptr && thumb->hasSwatch)
		{
			// A material with no map: its base colour fills the tile, with the kind icon on
			// top so it still reads as a material rather than a coloured square.
			drawList->AddRectFilled(artMin, artMax, thumb->swatch, 4.0f);
			drawList->AddRect(artMin, artMax, chrome::U32(chrome::WithAlpha(chrome::kText, 0.25f)), 4.0f, 0, 1.0f);
			drawKindIcon(KindIcon(entry.kind, isScript), chrome::WithAlpha(chrome::kText, 0.55f), 0.3f);
		}
		else
		{
			// Everything that is not a picture still gets a face: a large tinted kind icon,
			// which is what makes a grid scannable at all.
			drawKindIcon(KindIcon(entry.kind, isScript), KindTint(entry.kind, isScript), 0.42f);
		}

		// A grid tile is far narrower than a filename, and hard-clipping made
		// "X.material.toml" and "X.materialgraph.toml" render identically. Dropping the
		// middle keeps both the head and the tail that tell them apart.
		const float nameSize = ImGui::GetFontSize() * 0.85f;
		const float nameRoom = tile.x - 6.0f;
		std::string label = entry.name;
		if (chrome::MeasureSized(nameSize, label.c_str()).x > nameRoom && label.size() > 4)
		{
			std::size_t head = label.size();
			while (head > 2)
			{
				--head;
				const std::size_t tail = head / 2;
				std::string candidate = label.substr(0, head - tail) + "\xE2\x80\xA6" + label.substr(label.size() - tail);
				if (chrome::MeasureSized(nameSize, candidate.c_str()).x <= nameRoom)
				{
					label = candidate;
					break;
				}
			}
		}
		const ImVec2 nameExtent = chrome::MeasureSized(nameSize, label.c_str());
		const float nameX = std::max(origin.x + 3.0f, (origin.x + tileMax.x) * 0.5f - nameExtent.x * 0.5f);
		chrome::TextSized(drawList, nameSize, ImVec2(nameX, artMax.y + 4.0f), selected ? chrome::kText : chrome::kMuted, label.c_str());

		if (entry.isDirectory)
		{
			ImGui::SetItemTooltip("%s", entry.name.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				m_selectedPath = ToUtf8Path(entry.path);
				m_selectedIsDirectory = true;
				m_createDir = entry.path;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				m_pendingOpenDir = entry.path;
			}
			DrawRowContextMenu(context, entry);
		}
		else
		{
			ImGui::SetItemTooltip("%s\n%s", entry.name.c_str(), FormatSize(entry.sizeBytes).c_str());
			ApplyEntryInteractions(context, entry);
		}
		ImGui::PopID();
	}
	void FileExplorerPanel::DrawFolderContents(app::LayerContext& context)
	{
		Entry* dir = FindDirectory(m_tree, m_currentDir);
		if (dir == nullptr)
		{
			// The folder went away underneath us (deleted, or the project changed).
			OpenDirectory(context, m_root);
			dir = FindDirectory(m_tree, m_currentDir);
		}
		if (dir == nullptr)
		{
			ImGui::TextDisabled("This folder is no longer available.");
			return;
		}

		m_pendingOpenDir.clear();
		int shown = 0;
		if (m_viewMode == ViewMode::Grid)
		{
			const float avail = ImGui::GetContentRegionAvail().x;
			const float stride = m_tileSize + ImGui::GetStyle().ItemSpacing.x;
			const int columns = std::max(1, static_cast<int>(avail / std::max(1.0f, stride)));
			int column = 0;
			// Folders first, so entering one is possible from the grid itself rather than only
			// from the tree on the left.
			for (const bool wantDirectories: {true, false})
			{
				for (const Entry& child: dir->children)
				{
					if (child.isDirectory != wantDirectories)
					{
						continue;
					}
					if (column > 0)
					{
						ImGui::SameLine();
					}
					DrawFileTile(context, child, m_tileSize);
					++shown;
					column = (column + 1) % columns;
				}
			}
		}
		else
		{
			for (const Entry& child: dir->children)
			{
				if (!child.isDirectory)
				{
					DrawFileRow(context, child);
					++shown;
				}
			}
		}

		if (shown == 0)
		{
			ImGui::TextDisabled("This folder is empty.");
		}
		if (!m_pendingOpenDir.empty())
		{
			OpenDirectory(context, m_pendingOpenDir);
			m_pendingOpenDir.clear();
		}
	}

	void FileExplorerPanel::DrawSearchResults(app::LayerContext& context, const Entry& entry)
	{
		std::string needle = TrimCopy(m_search);
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		int matches = 0;
		const auto walk = [&](const auto& self, const Entry& node) -> void
		{
			for (const Entry& child: node.children)
			{
				if (child.isDirectory)
				{
					self(self, child);
					continue;
				}
				if (!ContainsCaseInsensitive(child.name, needle))
				{
					continue;
				}
				++matches;
				DrawFileRow(context, child);
				std::error_code relEc;
				const std::filesystem::path rel = std::filesystem::relative(child.path.parent_path(), m_root, relEc);
				if (!relEc)
				{
					ImGui::Indent(ImGui::GetFontSize() * 1.5f);
					ImGui::PushStyleColor(ImGuiCol_Text, chrome::kFaint);
					ImGui::TextUnformatted(IsSubpath(rel) ? rel.generic_string().c_str() : "/");
					ImGui::PopStyleColor();
					ImGui::Unindent(ImGui::GetFontSize() * 1.5f);
				}
			}
		};
		walk(walk, entry);

		if (matches == 0)
		{
			ImGui::TextDisabled("No files match '%s'.", m_search);
		}
	}

	bool FileExplorerPanel::DrawRowContextMenu(app::LayerContext& context, const Entry& entry)
	{
		bool mutated = false;
		if (ImGui::BeginPopupContextItem("##fectx"))
		{
			m_selectedPath = ToUtf8Path(entry.path);
			// into the viewport. Handles baking + the multi-primitive/skinned layout.
			if (entry.kind == dragdrop::FileKind::Model && ImGui::MenuItem(ICON_FA_PLUS "  Add to Scene"))
			{
				const std::string vfs = entry.payloadPath.starts_with("project://") ? entry.payloadPath : ToUtf8Path(entry.path);
				std::string error;
				const Entity root = editor::ImportModelIntoScene(context.Get<World>(), context.services, vfs, glm::mat4(1.0f), std::string{}, error);
				if (root.IsValid())
				{
					if (auto* selection = context.TryGet<SceneSelection>())
					{
						selection->Select(root);
					}
					mutated = true;
				}
				else
				{
					AE_WARN(LogCategory::App, "Add to Scene failed for '{}': {}", vfs, error);
				}
			}
			if (ImGui::MenuItem(entry.kind == dragdrop::FileKind::Script ? ICON_FA_CODE "  Open in editor" : ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "  Open"))
			{
				if (entry.kind == dragdrop::FileKind::Script)
				{
					OpenInEditor(ToUtf8Path(entry.path), 0);
				}
				else
				{
					OpenInOS(entry.path);
				}
			}
			if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in Explorer"))
			{
				ShowInOSExplorer(entry.path);
			}
			if (ImGui::MenuItem(ICON_FA_COPY "  Copy path"))
			{
				ImGui::SetClipboardText(ToUtf8Path(entry.path).c_str());
			}
			if (!entry.isDirectory && entry.payloadPath.starts_with("project://") && ImGui::MenuItem(ICON_FA_LINK "  Copy project:// path"))
			{
				ImGui::SetClipboardText(entry.payloadPath.c_str());
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_FA_PEN "  Rename"))
			{
				BeginRename(entry);
			}
			if (!entry.isDirectory && ImGui::MenuItem(ICON_FA_CLONE "  Duplicate"))
			{
				mutated = DuplicateEntry(entry.path);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::C(colors::Error));
			if (ImGui::MenuItem(ICON_FA_TRASH "  Delete..."))
			{
				m_deleteTarget = entry.path;
				m_deleteIsDirectory = entry.isDirectory;
				m_deleteReferenceCount = CountAssetReferences(entry.path, entry.isDirectory);
				m_openDeletePopup = true;
			}
			ImGui::PopStyleColor();
			if (entry.isDirectory)
			{
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_PLUS "  New here..."))
				{
					m_createDir = entry.path;
					m_openNewPopup = true;
				}
			}
			ImGui::EndPopup();
		}
		return mutated;
	}

	void FileExplorerPanel::DrawPendingPopups(app::LayerContext& context)
	{
		(void) context;
		if (m_openDeletePopup)
		{
			ImGui::OpenPopup("Delete?");
			m_openDeletePopup = false;
		}
		if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const std::string name = m_deleteTarget.filename().generic_string();
			ImGui::Text("Delete '%s'%s?", name.c_str(), m_deleteIsDirectory ? " and everything in it" : "");
			ImGui::TextDisabled("This cannot be undone.");
			if (m_deleteReferenceCount > 0)
			{
				// Unlike a rename, a delete has nowhere to repoint these: whatever used the
				// asset silently falls back to defaults, with only a log line to say why.
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, chrome::C(colors::Orange));
				ImGui::TextWrapped("%s  %d reference%s to this still exist and will break.", ICON_FA_TRIANGLE_EXCLAMATION, m_deleteReferenceCount, m_deleteReferenceCount == 1 ? "" : "s");
				ImGui::PopStyleColor();
			}
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Button, chrome::WithAlpha(chrome::C(colors::Error), 0.22f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chrome::WithAlpha(chrome::C(colors::Error), 0.65f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, chrome::C(colors::Error));
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kText);
			if (ImGui::Button(ICON_FA_TRASH " Delete", ImVec2(120.0f, 0.0f)))
			{
				DeleteEntry(m_deleteTarget, m_deleteIsDirectory);
				m_deleteTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopStyleColor(4);
			ImGui::SameLine();
			if (chrome::GhostButton("Cancel", ImVec2(120.0f, 0.0f)))
			{
				m_deleteTarget.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

} // namespace aether::editor
