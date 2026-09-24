#include "AetherCore.hpp"
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
#include "EditorDragDrop.hpp"
#include "debug/EditorShortcuts.hpp"
#include "Icons.hpp"
#include "debug/OpenInEditor.hpp"
#include "SceneSelection.hpp"
#include "editor/ModelImport.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/TomlConfig.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/FileUtil.hpp"
#include "material/MaterialSerializer.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "rendering/ModelPreviewService.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
		// Frames an asset is given to become resident before its thumbnail settles for what
	// it can get. Generous: a cold texture can take a while, and the cost of waiting is
	// an icon, while the cost of giving up early is a permanently wrong thumbnail.
	constexpr int kMaxThumbnailRetries = 600;
	// How long to wait for the bake pass to report it drew a slot before giving up on it.
	constexpr int kBakeTimeoutFrames = 240;
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
		m_kindVisible.fill(true);
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
			std::error_code timeEc;
			if (const auto written = file.last_write_time(timeEc); !timeEc)
			{
				e.writeTime = written.time_since_epoch().count();
			}

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

	bool FileExplorerPanel::IsSelected(const std::string& path) const
	{
		return std::find(m_selectedPaths.begin(), m_selectedPaths.end(), path) != m_selectedPaths.end();
	}

	std::vector<std::string> FileExplorerPanel::VisibleOrder() const
	{
		std::vector<std::string> order;
		const Entry* dir = nullptr;
		const auto find = [&](const auto& self, const Entry& node) -> const Entry*
		{
			if (node.isDirectory && node.path == m_currentDir)
			{
				return &node;
			}
			for (const Entry& child: node.children)
			{
				if (!child.isDirectory)
				{
					continue;
				}
				if (const Entry* hit = self(self, child))
				{
					return hit;
				}
			}
			return nullptr;
		};
		dir = find(find, m_tree);
		if (dir == nullptr)
		{
			return order;
		}
		// The same ordering the grid draws with, so a shift-range covers what the eye sees
		// between the two clicks and the arrow keys move the way the tiles are laid out.
		for (const Entry* child: SortedChildren(*dir))
		{
			order.push_back(ToUtf8Path(child->path));
		}
		return order;
	}

	void FileExplorerPanel::ClickSelect(const Entry& entry, const bool ctrl, const bool shift)
	{
		const std::string path = ToUtf8Path(entry.path);
		if (shift && !m_selectedPath.empty())
		{
			const std::vector<std::string> order = VisibleOrder();
			const auto from = std::find(order.begin(), order.end(), m_selectedPath);
			const auto to = std::find(order.begin(), order.end(), path);
			if (from != order.end() && to != order.end())
			{
				auto first = from;
				auto last = to;
				if (first > last)
				{
					std::swap(first, last);
				}
				m_selectedPaths.assign(first, last + 1);
				return;
			}
		}
		if (ctrl)
		{
			if (const auto it = std::find(m_selectedPaths.begin(), m_selectedPaths.end(), path); it != m_selectedPaths.end())
			{
				m_selectedPaths.erase(it);
				return;
			}
			m_selectedPaths.push_back(path);
			return;
		}
		m_selectedPaths.assign(1, path);
	}

	std::filesystem::path FileExplorerPanel::PhysicalPathFor(const std::string_view vfsPath) const
	{
		constexpr std::string_view kPrefix = "project://";
		if (!vfsPath.starts_with(kPrefix) || m_root.empty())
		{
			return {};
		}
		return m_root / std::filesystem::path(std::string(vfsPath.substr(kPrefix.size())));
	}

	bool FileExplorerPanel::MoveEntry(const std::filesystem::path& source, const std::filesystem::path& destDir)
	{
		m_opError.clear();
		if (source.empty() || destDir.empty())
		{
			return false;
		}
		std::error_code ec;
		if (!std::filesystem::exists(source, ec))
		{
			m_opError = "That file is no longer there.";
			return false;
		}
		if (source.parent_path() == destDir)
		{
			// Already where it was dropped: not an error, just nothing to do.
			return false;
		}
		// A folder cannot be moved inside itself, which would otherwise detach the whole
		// subtree from the project.
		if (std::filesystem::is_directory(source, ec))
		{
			const std::filesystem::path rel = std::filesystem::relative(destDir, source, ec);
			if (!ec && IsSubpath(rel))
			{
				m_opError = "A folder cannot be moved into itself.";
				return false;
			}
		}
		const std::filesystem::path target = destDir / source.filename();
		if (std::filesystem::exists(target, ec))
		{
			m_opError = target.filename().generic_string() + " already exists there.";
			return false;
		}
		std::filesystem::rename(source, target, ec);
		if (ec)
		{
			m_opError = "Could not move " + source.filename().generic_string() + ": " + ec.message();
			return false;
		}
		// Everything that referred to the old "project://" path has to follow it, exactly as
		// it does for a rename - otherwise the move silently unhooks the asset from every
		// scene, prefab and material that used it.
		RetargetAssetReferences(source, target);
		if (m_selectedPath == ToUtf8Path(source))
		{
			m_selectedPath = ToUtf8Path(target);
			m_createDir = target.parent_path();
		}
		m_treeDirty = true;
		return true;
	}

	bool FileExplorerPanel::AcceptFileDropIntoFolder(app::LayerContext& context, const std::filesystem::path& destDir)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return false;
		}
		bool moved = false;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload); payload != nullptr && payload->DataSize == sizeof(dragdrop::FilePayload))
		{
			const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
			const std::filesystem::path dragged = PhysicalPathFor(file->path);
			// Dragging one of several selected assets moves all of them, which is what the
			// highlight promises. Dragging an unselected one moves just that one.
			if (!dragged.empty() && IsSelected(ToUtf8Path(dragged)) && m_selectedPaths.size() > 1)
			{
				for (const std::string& path: m_selectedPaths)
				{
					moved = MoveEntry(std::filesystem::path(path), destDir) || moved;
				}
				m_selectedPaths.clear();
			}
			else
			{
				moved = MoveEntry(dragged, destDir);
			}
		}
		else if (const ImGuiPayload* scriptPayload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload); scriptPayload != nullptr && scriptPayload->DataSize == sizeof(dragdrop::ScriptPayload))
		{
			// A script's payload already carries its physical path, because a script is
			// dragged to be attached rather than to be resolved through the VFS.
			const auto* script = static_cast<const dragdrop::ScriptPayload*>(scriptPayload->Data);
			moved = MoveEntry(std::filesystem::path(script->sourcePath), destDir);
		}
		ImGui::EndDragDropTarget();
		if (moved)
		{
			// Thumbnails are keyed by path, so the moved asset's entry is now stale.
			ReleaseThumbnails(context);
		}
		return moved;
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

	void FileExplorerPanel::BeginDelete(const Entry& entry)
	{
		m_deleteTargets.clear();
		const std::string path = ToUtf8Path(entry.path);

		// Deleting one of several selected removes all of them, matching what a drag does.
		// Deleting an unselected one removes just it.
		if (IsSelected(path) && m_selectedPaths.size() > 1)
		{
			for (const std::string& selected: m_selectedPaths)
			{
				if (const Entry* found = FindEntryByPath(selected))
				{
					m_deleteTargets.push_back({found->path, found->isDirectory});
				}
			}
		}
		if (m_deleteTargets.empty())
		{
			m_deleteTargets.push_back({entry.path, entry.isDirectory});
		}

		// Counted once, when the dialog opens: each of these walks every project file.
		m_deleteReferenceCount = 0;
		for (const DeleteTarget& target: m_deleteTargets)
		{
			m_deleteReferenceCount += CountAssetReferences(target.path, target.isDirectory);
		}
		m_openDeletePopup = true;
	}

	bool FileExplorerPanel::DeleteEntry(const std::filesystem::path& target, const bool isDirectory)
	{
		// Try the recycle bin first. A confirmed delete is still a delete the user can regret,
		// and every other application on the machine leaves it recoverable; a permanent
		// remove_all on a folder took the whole subtree with no way back.
		if (io::file_util::MoveToTrash(target))
		{
			m_opError.clear();
			if (m_selectedPath == ToUtf8Path(target))
			{
				m_selectedPath.clear();
			}
			m_treeDirty = true;
			return true;
		}

		// No trash on this platform, or the shell refused. Fall back to the permanent delete
		// rather than leaving the file there after the user confirmed.
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

	void FileExplorerPanel::LoadSettings(TomlConfig& config, app::LayerContext& context)
	{
		(void) context;
		// The open FOLDER is deliberately not restored: it is a project path, and these
		// settings outlive the project they were written in.
		m_viewMode = config.GetFloat("fileexplorer.viewmode", 0.0f) >= 0.5f ? ViewMode::List : ViewMode::Grid;
		m_tileSize = std::clamp(config.GetFloat("fileexplorer.tilesize", m_tileSize), 56.0f, 160.0f);
		const int sort = std::clamp(static_cast<int>(config.GetFloat("fileexplorer.sortmode", 0.0f)), 0, static_cast<int>(SortMode::Modified));
		m_sortMode = static_cast<SortMode>(sort);
		m_sortDescending = config.GetBool("fileexplorer.sortdescending", m_sortDescending);
	}

	void FileExplorerPanel::SaveSettings(TomlConfig& config, app::LayerContext& context) const
	{
		(void) context;
		config.Set("fileexplorer.viewmode", m_viewMode == ViewMode::List ? 1.0f : 0.0f);
		config.Set("fileexplorer.tilesize", m_tileSize);
		config.Set("fileexplorer.sortmode", static_cast<float>(static_cast<int>(m_sortMode)));
		config.Set("fileexplorer.sortdescending", m_sortDescending);
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

		// Another panel asking us to show a folder. Consuming it here (rather than each panel
		// opening the OS file manager) is what keeps this the only asset browser.
		if (auto* selection = context.TryGet<SceneSelection>(); selection != nullptr)
		{
			if (std::string reveal = selection->ConsumeRevealRequest(); !reveal.empty())
			{
				const std::filesystem::path revealPath(reveal);
				std::error_code revealEc;
				if (std::filesystem::is_directory(revealPath, revealEc))
				{
					m_treeDirty = true;
					RescanTree();
					OpenDirectory(context, revealPath);
					SetVisible(true);
					ImGui::SetWindowFocus("File Explorer");
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
			if (!m_renameAfterScan.empty())
			{
				if (const Entry* fresh = FindEntryByPath(ToUtf8Path(m_renameAfterScan)))
				{
					BeginRename(*fresh);
					OpenDirectory(context, m_renameAfterScan.parent_path());
					m_selectedPath = ToUtf8Path(m_renameAfterScan);
					m_selectedPaths.assign(1, m_selectedPath);
				}
				m_renameAfterScan.clear();
			}
		}

		m_thumbnailLoadsThisFrame = 0;

		DrawToolbar(context);

		if (!m_opError.empty() || !m_scanError.empty())
		{
			ImGui::TextColored(chrome::kError, "%s", !m_opError.empty() ? m_opError.c_str() : m_scanError.c_str());
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
			// Straight after the breadcrumb, NOT right-aligned. Both attempts at right-aligning
			// pushed the controls out of the table cell and ImGui clipped them away entirely -
			// there is no reliable cell-relative width here, since GetContentRegionAvail and
			// GetContentRegionMax are both window-relative inside a cell. A visible control
			// beats a tidily placed one that disappears when the pane is narrow.
			ImGui::SameLine(0.0f, 16.0f);
			if (chrome::GhostButton(m_viewMode == ViewMode::Grid ? ICON_FA_LIST : ICON_FA_TABLE_CELLS_LARGE))
			{
				m_viewMode = m_viewMode == ViewMode::Grid ? ViewMode::List : ViewMode::Grid;
			}
			ImGui::SetItemTooltip(m_viewMode == ViewMode::Grid ? "Switch to list view" : "Switch to grid view");
			ImGui::SameLine();
			DrawSortMenu();
			ImGui::SameLine();
			DrawFilterMenu();
			if (m_viewMode == ViewMode::Grid)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(110.0f);
				ImGui::SliderFloat("##feTileSize", &m_tileSize, 56.0f, 160.0f, "%.0f px");
				ImGui::SetItemTooltip("Thumbnail size");
			}
			// The status line gets a row of its own reserved here, or the contents child would
			// take the height and push it out of the cell.
			const float statusH = ImGui::GetTextLineHeightWithSpacing();
			const float used = ImGui::GetCursorPosY() - contentsTop + ImGui::GetTextLineHeightWithSpacing() + statusH;
			ImGui::BeginChild("##feContents", ImVec2(0.0f, std::max(40.0f, paneH - used)));
			DrawFolderContents(context);
			HandleContentsSelectionGestures(context);
			HandleContentsShortcuts(context);
			ImGui::EndChild();
			DrawContentsStatus();

			ImGui::EndTable();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor(3);

		PumpMaterialThumbnailBakes(context);

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
		ImGui::SetItemTooltip("Create a script, material or folder in the open folder");
		// Anchored under the button that opened it, left edges aligned, so it reads as that
		// button's menu rather than a window that appeared next to the toolbar.
		const ImVec2 newButtonMin = ImGui::GetItemRectMin();
		const ImVec2 newButtonMax = ImGui::GetItemRectMax();
		ImGui::SetNextWindowPos(ImVec2(newButtonMin.x, newButtonMax.y + 4.0f), ImGuiCond_Always);
		if (ImGui::BeginPopup("##feNew"))
		{
			DrawCreateMenuItems(m_createDir.empty() ? m_root : m_createDir);
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
		const bool selected = IsSelected(ToUtf8Path(entry.path));
		ImGui::Selectable("##feRow", selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);

		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 rowMin = ImGui::GetItemRectMin();
			const ImVec2 rowMax = ImGui::GetItemRectMax();
			const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetFontSize()) * 0.5f;
			const float iconX = rowMin.x + 4.0f;
			const float nameX = iconX + ImGui::GetFontSize() * 1.5f;
			const char* icon = entry.isDirectory ? ICON_FA_FOLDER : KindIcon(entry.kind, isScript);
			const ImVec4 tint = entry.isDirectory ? chrome::WithAlpha(chrome::kAccent, 0.9f) : KindTint(entry.kind, isScript);
			drawList->AddText(ImVec2(iconX, textY), chrome::U32(tint), icon);
			drawList->AddText(ImVec2(nameX, textY), chrome::U32(chrome::kText), entry.name.c_str());
			const std::string trailing = entry.isDirectory ? FolderCountText(entry) : FormatSize(entry.sizeBytes);
			const float trailingW = chrome::MeasureSized(12.0f, trailing.c_str()).x;
			chrome::TextSized(drawList, 12.0f, ImVec2(rowMax.x - trailingW - 8.0f, textY + 2.0f), chrome::kFaint, trailing.c_str());
			if (selected)
			{
				drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), chrome::U32(chrome::kSelectionBar));
			}
		}

		if (entry.isDirectory)
		{
			// Same gestures the grid gives a folder, so the two views behave alike.
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				const ImGuiIO& io = ImGui::GetIO();
				ClickSelect(entry, io.KeyCtrl, io.KeyShift);
				m_selectedPath = ToUtf8Path(entry.path);
				m_selectedIsDirectory = true;
				m_createDir = entry.path;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				m_pendingOpenDir = entry.path;
			}
			AcceptFileDropIntoFolder(context, entry.path);
			DrawRowContextMenu(context, entry);
			ImGui::PopID();
			return;
		}

		ApplyEntryInteractions(context, entry);
		ImGui::PopID();
	}

	void FileExplorerPanel::ApplyEntryInteractions(app::LayerContext& context, const Entry& entry)
	{
		const bool isScript = entry.kind == dragdrop::FileKind::Script;

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			const ImGuiIO& io = ImGui::GetIO();
			ClickSelect(entry, io.KeyCtrl, io.KeyShift);
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
		m_selectedPaths.clear();
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
			// The same count the contents pane shows, so the tree agrees with the grid.
			const std::string countText = FolderCountText(entry);
			const float countW = chrome::MeasureSized(12.0f, countText.c_str()).x;
			chrome::TextSized(drawList, 12.0f, ImVec2(rowMax.x - countW - 6.0f, textY + 2.0f), chrome::kFaint, countText.c_str());
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
		AcceptFileDropIntoFolder(context, entry.path);
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
		const bool isModel = entry.kind == dragdrop::FileKind::Model;
		if (!isTexture && !isMaterial && !isModel)
		{
			return nullptr;
		}
		if (isModel)
		{
			// A model has nothing to resolve here - it is rendered by the baker, not sampled
			// from a file. The entry exists so the bake pump has somewhere to record its slot.
			const std::string modelKey = ToUtf8Path(entry.path);
			Thumbnail& modelThumb = m_thumbnails[modelKey];
			if (modelThumb.stamp != entry.writeTime)
			{
				InvalidateThumbnail(context, modelThumb);
				modelThumb.stamp = entry.writeTime;
				modelThumb.resolved = true;
			}
			return &modelThumb;
		}
		const std::string key = ToUtf8Path(entry.path);
		if (const auto it = m_thumbnails.find(key); it != m_thumbnails.end())
		{
			if (it->second.stamp == entry.writeTime)
			{
				return &it->second;
			}
			// The asset changed on disk - saving a material, recompiling its graph - so what
			// was built from the old contents is stale.
			InvalidateThumbnail(context, it->second);
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

		Thumbnail& thumb = m_thumbnails[key];
		InvalidateThumbnail(context, thumb);
		thumb.resolved = true;
		thumb.stamp = entry.writeTime;
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
				return &thumb;
			}
		}

		auto& textures = assets->GetTextureRegistry();
		thumb.texture = textures.Acquire(texturePath);
		const std::uint32_t slot = textures.ResolveSlot(thumb.texture);
		const std::uint32_t fallbackSlot = textures.ResolveSlot(textures.DefaultHandle());
		if (!thumb.texture.IsValid() || slot == 0xFFFFFFFFu || slot == fallbackSlot)
		{
			if (thumb.texture.IsValid())
			{
				textures.Release(thumb.texture);
				thumb.texture = {};
			}
			if (thumb.retries < kMaxThumbnailRetries)
			{
				// Not resident yet. Clearing the stamp is what makes the next frame try again
				// instead of treating this empty entry as the answer.
				++thumb.retries;
				thumb.stamp = 0;
				thumb.resolved = false;
			}
			return &thumb;
		}
		thumb.retries = 0;
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
		return &thumb;
	}
	void FileExplorerPanel::PumpMaterialThumbnailBakes(app::LayerContext& context)
	{
		auto* rendering = context.TryGet<aether::RenderingSubsystem>();
		auto* assets = context.TryGet<AssetManager>();
		auto* imgui = context.TryGet<aether::ImguiSubsystem>();
		auto* primitives = context.TryGet<PrimitiveMeshes>();
		if (rendering == nullptr || assets == nullptr || imgui == nullptr || primitives == nullptr)
		{
			return;
		}
		// A thumbnail bake advances one frame at a time and finishes only when the render pass
		// reports it drew the slot, so an idle-throttled editor starves it: at 10 fps each
		// thumbnail takes ten times as long, and the 240-frame give-up becomes 24 seconds
		// instead of 2. Hold the editor awake for as long as one is outstanding.
		const auto keepAwake = [&context]()
		{
			if (auto* engine = context.TryGet<AetherCore>())
			{
				engine->RequestActivity();
			}
		};
		if (!m_bakeInFlight.empty())
		{
			keepAwake();
		}

		ModelPreviewService& baker = rendering->GetMaterialThumbnailBaker();
		if (m_atlasImGuiId == 0 && baker.GetColorView() != nullptr)
		{
			const ImTextureID id = imgui->RegisterTexture(baker.GetColorView(), gpu::ImageLayout::ShaderReadOnly);
			if (id != ImTextureID_Invalid)
			{
				m_atlasImGuiId = static_cast<std::uint64_t>(id);
				m_atlasColumns = static_cast<int>(baker.GetAtlasColumns());
			}
		}

		// A bake takes one frame: the sphere is submitted here and drawn later in the same
		// frame, so its image is only readable once that frame has gone through the graph.
		if (!m_bakeInFlight.empty())
		{
			const auto it = m_thumbnails.find(m_bakeInFlight);
			const int slot = it != m_thumbnails.end() ? it->second.atlasSlot : -1;
			// Wait for the PASS to say it drew THIS request, not just this slot NUMBER.
			// Counting frames is not enough: a frame where the preview had nothing to
			// submit draws nothing, and showing the slot anyway displays undefined memory
			// as a black square. Matching the slot number alone is not enough either: slot
			// numbers are reused across unrelated bakes once ReleaseThumbnails resets
			// m_nextAtlasSlot, and LastDrawnSlot() answers "was slot N EVER drawn", not "was
			// slot N drawn FOR THIS REQUEST" - traced (by code reading) as the mechanism
			// behind a model tile showing a completely different, earlier-baked model's
			// content under its own correct filename (see ModelPreviewService::SetBakeSlot's
			// own comment). LastDrawnGeneration() is what actually answers that.
			if (slot >= 0 && baker.LastDrawnSlot() == slot && baker.LastDrawnGeneration() == m_bakeInFlightGeneration)
			{
				it->second.bakeReady = true;
				// Idle until the next material is picked, so nothing is drawn over it.
				(void) baker.SetBakeSlot(-1);
				m_bakeInFlight.clear();
			}
			else if (ImGui::GetFrameCount() > m_bakeStartedFrame + kBakeTimeoutFrames)
			{
				// It never drew. Leave bakeReady false so the tile keeps its icon or colour
				// rather than showing whatever happens to be in that square.
				(void) baker.SetBakeSlot(-1);
				m_bakeInFlight.clear();
			}
			else
			{
				return;
			}
		}

		const Entry* dir = FindDirectory(m_tree, m_currentDir);
		if (dir == nullptr)
		{
			return;
		}
		constexpr int kMaxDeferredPerFrame = 2;
		int deferred = 0;
		for (const Entry& child: dir->children)
		{
			const bool bakeableModel = child.kind == dragdrop::FileKind::Model && !BakedMeshFor(child).empty();
			if (child.isDirectory || (child.kind != dragdrop::FileKind::Material && !bakeableModel))
			{
				continue;
			}
			const std::string key = ToUtf8Path(child.path);
			const auto it = m_thumbnails.find(key);
			if (it == m_thumbnails.end() || it->second.bakeAttempted)
			{
				continue;
			}
			Thumbnail& thumb = it->second;
			thumb.bakeAttempted = true;

			// The atlas is finite. Past the last slot the remaining materials keep their
			// albedo or colour, which is a graceful stop rather than recycling a slot out
			// from under a tile that is on screen.
			if (thumb.atlasSlot < 0 && m_nextAtlasSlot >= static_cast<int>(baker.GetSlotCount()))
			{
				return;
			}
			if (bakeableModel)
			{
				// Shown first, then captured once its textures are up - the same rule the
				// materials follow, for the same reason.
				std::string modelError;
				if (!baker.ShowModel(*assets, BakedMeshFor(child), modelError))
				{
					m_opError = modelError;
					continue;
				}
				if (!baker.TexturesResident(assets->GetTextureRegistry()) && thumb.retries < kMaxThumbnailRetries)
				{
					++thumb.retries;
					thumb.bakeAttempted = false;
					if (++deferred >= kMaxDeferredPerFrame)
					{
						return;
					}
					continue;
				}
				thumb.retries = 0;
				if (thumb.atlasSlot < 0)
				{
					thumb.atlasSlot = m_nextAtlasSlot++;
				}
				m_bakeInFlightGeneration = baker.SetBakeSlot(thumb.atlasSlot);
				m_bakeInFlight = key;
				m_bakeStartedFrame = ImGui::GetFrameCount();
				return;
			}

			const std::string vfs = child.payloadPath.empty() ? VfsPathFor(child.path) : child.payloadPath;
			if (vfs.empty())
			{
				return;
			}
			const auto material = assets->LoadMaterialPreset(vfs);
			if (!material)
			{
				return;
			}
			// Baking before the material's albedo is resident produces a plain default-looking
			// sphere, and a one-shot bake would keep that forever - which is why editing and
			// saving the material was the only way to get a correct thumbnail.
			auto& bakeTextures = assets->GetTextureRegistry();
			const std::uint32_t fallbackSlot = bakeTextures.ResolveSlot(bakeTextures.DefaultHandle());
			const auto pending = [&](const TextureHandle h) { return h.IsValid() && bakeTextures.ResolveSlot(h) == fallbackSlot; };
			const bool texturesPending = pending(material->albedoTex) || pending(material->normalTex) || pending(material->metallicRoughnessTex)
			        || pending(material->occlusionTex) || pending(material->emissiveTex);
			if (texturesPending && thumb.retries < kMaxThumbnailRetries)
			{
				// Leave bakeAttempted false so a later frame tries again. Acquiring here is
				// what keeps the upload moving, so retrying is also what makes it finish.
				//
				// `continue`, NOT return: one material waiting on a slow texture must not hold
				// up every other thumbnail in the folder behind it.
				++thumb.retries;
				thumb.bakeAttempted = false;
				for (const TextureHandle h: {material->albedoTex, material->normalTex, material->metallicRoughnessTex, material->occlusionTex, material->emissiveTex})
				{
					if (h.IsValid())
					{
						bakeTextures.Release(h);
					}
				}
				if (++deferred >= kMaxDeferredPerFrame)
				{
					// Each deferral re-reads and re-parses the material, so only a couple are
					// worth doing per frame while the uploads catch up.
					return;
				}
				continue;
			}
			thumb.retries = 0;

			std::string error;
			if (!baker.ShowMaterialOnMesh(*assets, primitives->Get(PrimitiveMesh::Sphere), *material, error))
			{
				return;
			}
			if (thumb.atlasSlot < 0)
			{
				thumb.atlasSlot = m_nextAtlasSlot++;
			}
			m_bakeInFlightGeneration = baker.SetBakeSlot(thumb.atlasSlot);
			m_bakeInFlight = key;
			m_bakeStartedFrame = ImGui::GetFrameCount();
			// This frame started it; without this the editor could idle before the next one.
			keepAwake();

			// ShowMaterialOnMesh takes its own references; these are this function's.
			auto& textures = assets->GetTextureRegistry();
			for (const TextureHandle h: {material->albedoTex, material->normalTex, material->metallicRoughnessTex, material->occlusionTex, material->emissiveTex})
			{
				if (h.IsValid())
				{
					textures.Release(h);
				}
			}
			return;
		}
	}

	void FileExplorerPanel::InvalidateThumbnail(app::LayerContext& context, Thumbnail& thumb)
	{
		if (thumb.imguiId != 0)
		{
			if (auto* imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(thumb.imguiId));
			}
			thumb.imguiId = 0;
		}
		if (thumb.texture.IsValid())
		{
			if (auto* assets = context.TryGet<AssetManager>())
			{
				assets->GetTextureRegistry().Release(thumb.texture);
			}
			thumb.texture = {};
		}
		thumb.hasSwatch = false;
		thumb.swatch = 0;
		thumb.resolved = false;
		// atlasSlot survives on purpose: the re-bake draws over the same square.
		thumb.bakeAttempted = false;
		thumb.bakeReady = false;
	}

	std::string FileExplorerPanel::FolderCountText(const Entry& entry)
	{
		// A folder has no size, so its item count takes that column instead. Empty says so in
		// words: a blank where a number belongs reads as missing information.
		const std::size_t count = entry.children.size();
		if (count == 0)
		{
			return "empty";
		}
		return std::to_string(count) + (count == 1 ? " item" : " items");
	}

	std::string FileExplorerPanel::BakedMeshFor(const Entry& entry) const
	{
		if (entry.kind != dragdrop::FileKind::Model)
		{
			return {};
		}
		const std::string ext = entry.path.extension().generic_string();
		if (ext == ".mesh")
		{
			return entry.payloadPath.empty() ? VfsPathFor(entry.path) : entry.payloadPath;
		}
		// A raw glTF is only renderable once baked. Use the sibling .mesh when the importer has
		// already produced one, and otherwise leave the tile with its icon: opening a folder
		// must never kick off a model bake.
		const std::filesystem::path baked = entry.path.parent_path() / (entry.path.stem().generic_string() + ".mesh");
		std::error_code ec;
		if (!std::filesystem::exists(baked, ec))
		{
			return {};
		}
		return VfsPathFor(baked);
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
		// Every slot is free again once the thumbnails referencing them are gone.
		m_nextAtlasSlot = 0;
		if (auto* rendering = context.TryGet<aether::RenderingSubsystem>())
		{
			(void) rendering->GetMaterialThumbnailBaker().SetBakeSlot(-1);
		}
		m_bakeInFlight.clear();
	}

	void FileExplorerPanel::DrawFileTile(app::LayerContext& context, const Entry& entry, const float tileSize)
	{
		ImGui::PushID(entry.path.generic_string().c_str());

		// The grid needs the rename field too. Only the list row drew it, so renaming from
		// the context menu (or F2) in the default view set a target that nothing rendered -
		// the rename simply never appeared.
		if (m_renameTarget == entry.path)
		{
			ImGui::BeginGroup();
			ImGui::PushItemWidth(tileSize);
			const bool handled = DrawActiveRename(entry);
			ImGui::PopItemWidth();
			ImGui::EndGroup();
			if (handled)
			{
				ImGui::PopID();
				return;
			}
		}

		const bool isScript = entry.kind == dragdrop::FileKind::Script;
		const bool selected = IsSelected(ToUtf8Path(entry.path));
		const float labelH = ImGui::GetFontSize() * 1.9f;
		const ImVec2 tile(tileSize, tileSize + labelH);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##tile", tile);
		m_frameTiles.emplace_back(ToUtf8Path(entry.path), ImRect(origin, ImVec2(origin.x + tile.x, origin.y + tile.y)));

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
			// Under the icon: how much is in there, which is the one thing a folder tile can
			// say about itself that its name does not.
			const std::string countText = FolderCountText(entry);
			const float countSize = ImGui::GetFontSize() * 0.8f;
			const ImVec2 countExtent = chrome::MeasureSized(countSize, countText.c_str());
			chrome::TextSized(drawList,
			        countSize,
			        ImVec2((artMin.x + artMax.x) * 0.5f - countExtent.x * 0.5f, artMax.y - countExtent.y - 2.0f),
			        chrome::kFaint,
			        countText.c_str());
		}
		else if (thumb != nullptr && thumb->bakeReady && thumb->atlasSlot >= 0 && m_atlasImGuiId != 0)
		{
			const float cols = static_cast<float>(m_atlasColumns);
			const float col = static_cast<float>(thumb->atlasSlot % m_atlasColumns);
			const float row = static_cast<float>(thumb->atlasSlot / m_atlasColumns);
			const ImVec2 uv0(col / cols, row / cols);
			const ImVec2 uv1((col + 1.0f) / cols, (row + 1.0f) / cols);
			drawList->AddImageRounded(static_cast<ImTextureID>(m_atlasImGuiId), artMin, artMax, uv0, uv1, IM_COL32_WHITE, 4.0f);
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
		// ".toml" is on every material and every graph, so it is the first thing to go: it
		// costs five characters and distinguishes nothing. What is left ("...material" vs
		// "...materialgraph") is exactly what tells two neighbouring files apart.
		if (label.size() > 5 && label.compare(label.size() - 5, 5, ".toml") == 0)
		{
			label.erase(label.size() - 5);
		}
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
			ImGui::SetItemTooltip("%s\n%s", entry.name.c_str(), FolderCountText(entry).c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				// Through the same selection rules as a file. Setting m_selectedPath alone left
				// the folder out of m_selectedPaths, which is what the highlight reads - so a
				// clicked folder looked unselected.
				const ImGuiIO& io = ImGui::GetIO();
				ClickSelect(entry, io.KeyCtrl, io.KeyShift);
				m_selectedPath = ToUtf8Path(entry.path);
				m_selectedIsDirectory = true;
				m_createDir = entry.path;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				m_pendingOpenDir = entry.path;
			}
			AcceptFileDropIntoFolder(context, entry.path);
			DrawRowContextMenu(context, entry);
		}
		else
		{
				ImGui::SetItemTooltip("%s\n%s", entry.name.c_str(), FormatSize(entry.sizeBytes).c_str());
			ApplyEntryInteractions(context, entry);
		}
		ImGui::PopID();
	}
	const FileExplorerPanel::Entry* FileExplorerPanel::FindEntryByPath(const std::string& path) const
	{
		const auto walk = [&](const auto& self, const Entry& node) -> const Entry*
		{
			for (const Entry& child: node.children)
			{
				if (ToUtf8Path(child.path) == path)
				{
					return &child;
				}
				if (child.isDirectory)
				{
					if (const Entry* hit = self(self, child))
					{
						return hit;
					}
				}
			}
			return nullptr;
		};
		return walk(walk, m_tree);
	}

	void FileExplorerPanel::SelectOnly(app::LayerContext& context, const Entry& entry)
	{
		m_selectedPath = ToUtf8Path(entry.path);
		m_selectedPaths.assign(1, m_selectedPath);
		m_selectedPayloadPath = entry.payloadPath;
		m_selectedKind = entry.kind;
		m_selectedIsDirectory = entry.isDirectory;
		m_createDir = entry.isDirectory ? entry.path : entry.path.parent_path();
		if (!entry.isDirectory)
		{
			if (auto* selection = context.TryGet<SceneSelection>())
			{
				const bool isScript = entry.kind == dragdrop::FileKind::Script;
				selection->SelectAsset(ToSelectionKind(entry.kind), isScript ? m_selectedPath : entry.payloadPath, entry.name);
			}
		}
	}

	void FileExplorerPanel::HandleContentsSelectionGestures(app::LayerContext& context)
	{
		(void) context;
		if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
		{
			// Still finish a band that started here even if the cursor has left the pane.
			if (!m_marqueeActive)
			{
				return;
			}
		}

		const ImGuiIO& io = ImGui::GetIO();
		const bool overTile = ImGui::IsAnyItemHovered();

		// Press on empty space: start a band, and drop the previous selection unless the user
		// is adding to it. Clicking off a selection used to leave everything highlighted, so
		// there was no way to select nothing.
		if (!m_marqueeActive && !overTile && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_marqueeActive = true;
			m_marqueeAnchor = io.MousePos;
			if (!io.KeyCtrl && !io.KeyShift)
			{
				m_selectedPaths.clear();
				m_selectedPath.clear();
			}
		}

		if (!m_marqueeActive)
		{
			return;
		}

		const ImVec2 cursor = io.MousePos;
		const ImRect band(std::min(m_marqueeAnchor.x, cursor.x),
		        std::min(m_marqueeAnchor.y, cursor.y),
		        std::max(m_marqueeAnchor.x, cursor.x),
		        std::max(m_marqueeAnchor.y, cursor.y));

		// Only once it is a drag rather than a click, so a plain click on empty space just
		// clears the selection without flashing a band.
		const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
		if (dragging)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(band.Min, band.Max, chrome::U32(chrome::WithAlpha(chrome::kAccent, 0.16f)));
			drawList->AddRect(band.Min, band.Max, chrome::U32(chrome::kAccent));

			// Rebuilt every frame from the band, so shrinking it deselects again.
			std::vector<std::string> covered;
			for (const auto& [path, rect]: m_frameTiles)
			{
				if (rect.Overlaps(band))
				{
					covered.push_back(path);
				}
			}
			m_selectedPaths = covered;
			if (!covered.empty())
			{
				m_selectedPath = covered.back();
			}
		}

		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			m_marqueeActive = false;
		}
	}

	void FileExplorerPanel::HandleContentsShortcuts(app::LayerContext& context)
	{
		// Only while this panel has the keyboard and nothing is being typed into - a rename
		// field or the search box must keep its own Delete and Enter.
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		// Recorded even when a rename field is open: the scene must not act on Delete just
		// because this panel declined it.
		m_ownsEditingKeys = focused;
		if (!focused || ImGui::GetIO().WantTextInput || !m_renameTarget.empty())
		{
			return;
		}

		const std::vector<std::string> order = VisibleOrder();
		if (!order.empty())
		{
			const auto at = std::find(order.begin(), order.end(), m_selectedPath);
			int index = at == order.end() ? -1 : static_cast<int>(at - order.begin());
			const int columns = m_viewMode == ViewMode::Grid ? std::max(1, m_gridColumns) : 1;
			int moved = index;
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && m_viewMode == ViewMode::Grid)
			{
				moved = index + 1;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && m_viewMode == ViewMode::Grid)
			{
				moved = index - 1;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			{
				moved = index + columns;
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			{
				moved = index - columns;
			}
			// From nothing selected, any arrow starts at the first entry.
			if (index < 0 && moved != index)
			{
				moved = 0;
			}
			if (moved != index && moved >= 0 && moved < static_cast<int>(order.size()))
			{
				if (const Entry* entry = FindEntryByPath(order[static_cast<std::size_t>(moved)]))
				{
					SelectOnly(context, *entry);
				}
			}
		}

		const ImGuiIO& shortcutIo = ImGui::GetIO();
		if (shortcutIo.KeyCtrl && ImGui::IsKeyPressed(shortcuts::kAssetsSelectAll.key))
		{
			// Everything the folder is showing, in the order it is shown.
			m_selectedPaths = order;
			if (!m_selectedPaths.empty())
			{
				m_selectedPath = m_selectedPaths.back();
			}
			return;
		}
		if (shortcutIo.KeyCtrl && ImGui::IsKeyPressed(shortcuts::kAssetsDuplicate.key))
		{
			// Every selected file, matching what delete and drag already do. Folders are
			// skipped: DuplicateEntry copies a file, and silently doing nothing for a folder
			// is better than half-copying one.
			bool duplicated = false;
			for (const std::string& path: m_selectedPaths)
			{
				if (const Entry* found = FindEntryByPath(path); found != nullptr && !found->isDirectory)
				{
					duplicated = DuplicateEntry(found->path) || duplicated;
				}
			}
			if (duplicated)
			{
				m_treeDirty = true;
			}
			return;
		}

		if (m_selectedPath.empty())
		{
			return;
		}
		const Entry* selected = FindEntryByPath(m_selectedPath);
		if (selected == nullptr)
		{
			return;
		}

		if (ImGui::IsKeyPressed(shortcuts::kAssetsRename.key))
		{
			BeginRename(*selected);
		}
		else if (ImGui::IsKeyPressed(shortcuts::kAssetsDelete.key))
		{
			// Straight to the same confirmation the menu opens, reference count and all -
			// never a silent delete on a keypress.
			BeginDelete(*selected);
		}
		else if (ImGui::IsKeyPressed(shortcuts::kAssetsOpen.key) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
		{
			if (selected->isDirectory)
			{
				m_pendingOpenDir = selected->path;
			}
			else if (selected->kind == dragdrop::FileKind::Script)
			{
				OpenInEditor(ToUtf8Path(selected->path), 0);
			}
			else
			{
				OpenInOS(selected->path);
			}
		}
		else if (ImGui::IsKeyPressed(shortcuts::kAssetsUp.key) && m_currentDir != m_root)
		{
			m_pendingOpenDir = m_currentDir.parent_path();
		}
	}

	std::vector<const FileExplorerPanel::Entry*> FileExplorerPanel::SortedChildren(const Entry& dir) const
	{
		std::vector<const Entry*> ordered;
		ordered.reserve(dir.children.size());
		for (const Entry& child: dir.children)
		{
			// Folders are never filtered out - hiding the way back out of a folder is not a
			// filter, it is a trap.
			if (!child.isDirectory && !KindVisible(child.kind))
			{
				continue;
			}
			ordered.push_back(&child);
		}

		const auto lowerName = [](const Entry& e)
		{
			std::string out = e.name;
			std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		};

		std::stable_sort(ordered.begin(),
		        ordered.end(),
		        [&](const Entry* a, const Entry* b)
		        {
			        // Folders first, always, and never reversed: they are how you move around,
			        // and burying them at the bottom of a descending size order helps nobody.
			        if (a->isDirectory != b->isDirectory)
			        {
				        return a->isDirectory;
			        }

			        bool less = false;
			        switch (m_sortMode)
			        {
				        case SortMode::Type:
					        // Kind first, then name inside a kind - a type sort that leaves each
					        // group in arbitrary order is barely a sort.
					        if (a->kind != b->kind)
					        {
						        less = static_cast<int>(a->kind) < static_cast<int>(b->kind);
						        break;
					        }
					        less = lowerName(*a) < lowerName(*b);
					        break;
				        case SortMode::Size:
					        if (a->sizeBytes != b->sizeBytes)
					        {
						        less = a->sizeBytes < b->sizeBytes;
						        break;
					        }
					        less = lowerName(*a) < lowerName(*b);
					        break;
				        case SortMode::Modified:
					        if (a->writeTime != b->writeTime)
					        {
						        less = a->writeTime < b->writeTime;
						        break;
					        }
					        less = lowerName(*a) < lowerName(*b);
					        break;
				        case SortMode::Name:
				        default:
					        less = lowerName(*a) < lowerName(*b);
					        break;
			        }
			        return m_sortDescending ? !less : less;
		        });
		return ordered;
	}

	bool FileExplorerPanel::KindVisible(const dragdrop::FileKind kind) const
	{
		const auto index = static_cast<std::size_t>(kind);
		// Default-constructed flags are false, so an untouched panel would show nothing at
		// all. Treat "never configured" as "show everything".
		if (index >= m_kindVisible.size())
		{
			return true;
		}
		return m_kindVisible[index];
	}

	bool FileExplorerPanel::AnyKindHidden() const
	{
		return std::any_of(m_kindVisible.begin(), m_kindVisible.end(), [](const bool shown) { return !shown; });
	}

	void FileExplorerPanel::DrawContentsStatus()
	{
		std::string status = std::to_string(m_shownCount) + (m_shownCount == 1 ? " item" : " items");

		// The selection is counted from what is actually on screen. A count that includes
		// entries hidden by the filter would explain nothing and worry everyone.
		std::size_t selectedVisible = 0;
		for (const std::string& path: m_selectedPaths)
		{
			if (std::any_of(m_frameTiles.begin(), m_frameTiles.end(), [&](const auto& tile) { return tile.first == path; }))
			{
				++selectedVisible;
			}
		}
		if (m_viewMode == ViewMode::List)
		{
			// The list does not record tile rectangles, so fall back to the whole selection.
			selectedVisible = m_selectedPaths.size();
		}
		if (selectedVisible > 0)
		{
			status += "   \xc2\xb7   " + std::to_string(selectedVisible) + " selected";
			if (m_selectedBytes > 0)
			{
				status += " (" + FormatSize(m_selectedBytes) + ")";
			}
		}
		if (m_hiddenByFilter > 0)
		{
			status += "   \xc2\xb7   " + std::to_string(m_hiddenByFilter) + " hidden";
		}

		ImGui::PushStyleColor(ImGuiCol_Text, chrome::kFaint);
		ImGui::TextUnformatted(status.c_str());
		ImGui::PopStyleColor();
	}

	void FileExplorerPanel::DrawFilterMenu()
	{
		// Tinted while filtering, and the count of what it is hiding is printed in the pane.
		// A filter you cannot see is indistinguishable from a browser that has lost your files.
		const bool filtering = AnyKindHidden();
		if (chrome::GhostButton(ICON_FA_FILTER "##feFilter", ImVec2(0.0f, 0.0f), filtering ? chrome::kAccentHi : chrome::kMuted))
		{
			ImGui::OpenPopup("##feFilterMenu");
		}
		ImGui::SetItemTooltip(filtering ? "Filtering by type" : "Filter by type");
		if (ImGui::BeginPopup("##feFilterMenu"))
		{
			chrome::SectionTag("SHOW");
			const struct
			{
				dragdrop::FileKind kind;
				const char* label;
			} kinds[] = {
			        {dragdrop::FileKind::Model, "Models"},
			        {dragdrop::FileKind::Material, "Materials"},
			        {dragdrop::FileKind::MaterialGraph, "Material graphs"},
			        {dragdrop::FileKind::Texture, "Textures"},
			        {dragdrop::FileKind::Script, "Scripts"},
			        {dragdrop::FileKind::Prefab, "Prefabs"},
			        {dragdrop::FileKind::Scene, "Scenes"},
			        {dragdrop::FileKind::Shader, "Shaders"},
			        {dragdrop::FileKind::Unknown, "Other files"},
			};
			for (const auto& entry: kinds)
			{
				const auto index = static_cast<std::size_t>(entry.kind);
				if (index < m_kindVisible.size())
				{
					ImGui::Checkbox(entry.label, &m_kindVisible[index]);
				}
			}
			ImGui::Separator();
			if (ImGui::Selectable("Show all"))
			{
				m_kindVisible.fill(true);
			}
			ImGui::EndPopup();
		}
	}

	void FileExplorerPanel::DrawSortMenu()
	{
		if (chrome::GhostButton(ICON_FA_ARROW_DOWN_SHORT_WIDE "##feSort"))
		{
			ImGui::OpenPopup("##feSortMenu");
		}
		ImGui::SetItemTooltip("Sort the folder");
		if (ImGui::BeginPopup("##feSortMenu"))
		{
			chrome::SectionTag("SORT BY");
			const struct
			{
				SortMode mode;
				const char* label;
			} modes[] = {
			        {SortMode::Name, "Name"},
			        {SortMode::Type, "Type"},
			        {SortMode::Size, "Size"},
			        {SortMode::Modified, "Date modified"},
			};
			for (const auto& mode: modes)
			{
				// Selectable rather than MenuItem, which does not activate under injected
				// input and so cannot be driven or tested.
				if (ImGui::Selectable(mode.label, m_sortMode == mode.mode))
				{
					m_sortMode = mode.mode;
				}
			}
			ImGui::Separator();
			ImGui::Checkbox("Descending", &m_sortDescending);
			ImGui::EndPopup();
		}
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

		// NOT cleared here. A request can also come from the keyboard, which is handled after
		// this function has run, and clearing on entry wiped it before it could ever apply.
		// The apply below is the only place that should clear it.
		m_frameTiles.clear();
		int shown = 0;
		if (m_viewMode == ViewMode::Grid)
		{
			const float avail = ImGui::GetContentRegionAvail().x;
			const float stride = m_tileSize + ImGui::GetStyle().ItemSpacing.x;
			const int columns = std::max(1, static_cast<int>(avail / std::max(1.0f, stride)));
			m_gridColumns = columns;
			int column = 0;
			for (const Entry* child: SortedChildren(*dir))
			{
				if (column > 0)
				{
					ImGui::SameLine();
				}
				DrawFileTile(context, *child, m_tileSize);
				++shown;
				column = (column + 1) % columns;
			}
		}
		else
		{
			// Folders included. The list used to draw files only, so switching view made every
			// folder vanish and read as though the browser had jumped somewhere else.
			for (const Entry* child: SortedChildren(*dir))
			{
				DrawFileRow(context, *child);
				++shown;
			}
		}

		m_shownCount = shown;
		m_selectedBytes = 0;
		for (const std::string& path: m_selectedPaths)
		{
			if (const Entry* found = FindEntryByPath(path); found != nullptr && !found->isDirectory)
			{
				m_selectedBytes += found->sizeBytes;
			}
		}
		m_hiddenByFilter = 0;
		for (const Entry& child: dir->children)
		{
			if (!child.isDirectory && !KindVisible(child.kind))
			{
				++m_hiddenByFilter;
			}
		}
		if (shown == 0 && m_hiddenByFilter == 0)
		{
			ImGui::TextDisabled("This folder is empty.  Right-click to create something.");
		}
		else if (m_hiddenByFilter > 0)
		{
			// Stated where the files are missing from, not only on the toolbar button.
			ImGui::Spacing();
			ImGui::TextColored(chrome::C(colors::Orange), "%s  %d hidden by the type filter", ICON_FA_FILTER, m_hiddenByFilter);
		}

		// Right-click anywhere the tiles are not: create in the folder being looked at, which
		// is where you already are rather than a toolbar at the other end of the panel.
		if (ImGui::BeginPopupContextWindow("##feCreateHere", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			chrome::SectionTag("CREATE");
			DrawCreateMenuItems(m_currentDir);
			ImGui::EndPopup();
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
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kError);
			if (ImGui::MenuItem(ICON_FA_TRASH "  Delete..."))
			{
				BeginDelete(entry);
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

	void FileExplorerPanel::DrawCreateMenuItems(const std::filesystem::path& dir)
	{
		// A script always goes to the project's scripts directory, wherever you right-clicked,
		// so the menu says where rather than quietly putting it somewhere else.
		const std::filesystem::path scriptDir = m_scriptRoot.empty() ? m_root / "scripts" : m_scriptRoot;
		// Selectable rather than MenuItem: MenuItem carries menu-navigation handling that does
		// not activate under injected input, so these items could not be driven or tested.
		// Closing the popup is the only thing MenuItem was doing for us here.
		const auto item = [](const char* label)
		{
			const bool clicked = ImGui::Selectable(label);
			if (clicked)
			{
				ImGui::CloseCurrentPopup();
			}
			return clicked;
		};

		if (item(ICON_FA_CODE "  C# Script"))
		{
			CreateAsset(NewAssetKind::Script, scriptDir);
		}
		if (!m_scriptRoot.empty())
		{
			std::error_code ec;
			const std::filesystem::path rel = std::filesystem::relative(scriptDir, m_root, ec);
			if (!ec && IsSubpath(rel))
			{
				ImGui::SetItemTooltip("Created in %s", rel.generic_string().c_str());
			}
		}
		if (item(ICON_FA_PALETTE "  Material"))
		{
			CreateAsset(NewAssetKind::Material, dir);
		}
		ImGui::Separator();
		if (item(ICON_FA_FOLDER "  Folder"))
		{
			CreateAsset(NewAssetKind::Folder, dir);
		}
	}

	bool FileExplorerPanel::CreateAsset(const NewAssetKind kind, const std::filesystem::path& dir)
	{
		m_opError.clear();
		if (dir.empty())
		{
			return false;
		}

		// A default name, then the row goes straight into rename so it is named in place -
		// the same gesture as renaming anything else, instead of a dialog that has to be
		// filled in before anything exists.
		const auto uniquePath = [&](const std::string& stem, const std::string& extension)
		{
			for (int suffix = 0; suffix < 1000; ++suffix)
			{
				const std::string name = suffix == 0 ? stem : stem + std::to_string(suffix);
				const std::filesystem::path candidate = dir / (name + extension);
				std::error_code ec;
				if (!std::filesystem::exists(candidate, ec))
				{
					return candidate;
				}
			}
			return dir / (stem + extension);
		};

		std::filesystem::path created;
		switch (kind)
		{
			case NewAssetKind::Script:
			{
				const std::filesystem::path path = uniquePath("NewScript", ".cs");
				std::string error;
				if (!CreateGameScriptFile(dir, path.stem().generic_string(), error))
				{
					m_opError = error;
					return false;
				}
				created = path;
				break;
			}
			case NewAssetKind::Material:
			{
				const std::filesystem::path path = uniquePath("NewMaterial", ".material.toml");
				// Written through the same serializer the inspector saves with, so a new
				// material and an edited one are the same file shape.
				if (!io::file_util::WriteText(path, MaterialSerializer::ToToml(MaterialPresetSpec{})))
				{
					m_opError = "Could not create the material.";
					return false;
				}
				created = path;
				break;
			}
			case NewAssetKind::Folder:
			{
				const std::filesystem::path path = uniquePath("NewFolder", "");
				if (!io::file_util::CreateDirectories(path))
				{
					m_opError = "Could not create the folder.";
					return false;
				}
				created = path;
				break;
			}
		}

		m_treeDirty = true;
		// Renaming has to wait for the rescan: the row does not exist yet.
		m_renameAfterScan = created;
		return true;
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
		if (m_deleteTargets.empty())
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
			if (m_deleteTargets.size() == 1)
			{
				const DeleteTarget& only = m_deleteTargets.front();
				ImGui::Text("Delete '%s'%s?", only.path.filename().generic_string().c_str(), only.isDirectory ? " and everything in it" : "");
			}
			else
			{
				ImGui::Text("Delete these %zu items?", m_deleteTargets.size());
				// Named, not just counted: a number is not enough to check before something
				// that cannot be undone.
				ImGui::Spacing();
				for (std::size_t i = 0; i < m_deleteTargets.size() && i < 8; ++i)
				{
					const DeleteTarget& target = m_deleteTargets[i];
					ImGui::BulletText("%s%s", target.path.filename().generic_string().c_str(), target.isDirectory ? "/" : "");
				}
				if (m_deleteTargets.size() > 8)
				{
					ImGui::TextDisabled("   and %zu more", m_deleteTargets.size() - 8);
				}
				ImGui::Spacing();
			}
			// Says what actually happens. Claiming a delete cannot be undone when it goes to the
			// recycle bin makes people hesitate over something reversible - and the one time it
			// really is permanent, they have learned to discount the warning.
			if (io::file_util::HasTrashSupport())
			{
				ImGui::TextDisabled("Moved to the %s, so you can restore it from there.", std::string(io::file_util::TrashDisplayName()).c_str());
			}
			else
			{
				ImGui::TextDisabled("This cannot be undone.");
			}
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
			ImGui::PushStyleColor(ImGuiCol_Button, chrome::WithAlpha(chrome::kError, 0.22f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chrome::WithAlpha(chrome::kError, 0.65f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, chrome::kError);
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kText);
			if (ImGui::Button(ICON_FA_TRASH " Delete", ImVec2(120.0f, 0.0f)))
			{
				for (const DeleteTarget& target: m_deleteTargets)
				{
					DeleteEntry(target.path, target.isDirectory);
				}
				m_deleteTargets.clear();
				m_selectedPaths.clear();
				m_selectedPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopStyleColor(4);
			ImGui::SameLine();
			if (chrome::GhostButton("Cancel", ImVec2(120.0f, 0.0f)))
			{
				m_deleteTargets.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

} // namespace aether::editor
