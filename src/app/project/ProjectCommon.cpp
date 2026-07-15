#include "project/ProjectCommon.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <system_error>

#ifdef _WIN32
#	include <Windows.h>
#	include <shobjidl.h>
#	undef CopyFile
#endif

#include "io/FileUtil.hpp"
#include "utils/AetherExceptions.hpp"
#include "utils/Logger.hpp"
#include "utils/TextIni.hpp"
#include "utils/TomlConfig.hpp"

using namespace std::string_view_literals;

namespace aether::app::project
{
	namespace
	{
		constexpr std::string_view kProjectFileName = "ProjectSettings.toml";

		std::filesystem::path ResolveProjectPath(const std::filesystem::path& root, std::string_view value, std::string_view fallback)
		{
			std::filesystem::path path = value.empty() ? std::filesystem::path(fallback) : std::filesystem::path(std::string(value));
			if (path.is_relative())
			{
				path = root / path;
			}
			return NormalizePath(std::move(path));
		}

		std::string EscapeTomlString(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				if (c == '\\' || c == '"')
				{
					out += '\\';
				}
				out += c;
			}
			return out;
		}

		std::string EscapeXmlAttribute(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				switch (c)
				{
					case '&':
						out += "&amp;";
						break;
					case '<':
						out += "&lt;";
						break;
					case '>':
						out += "&gt;";
						break;
					case '"':
						out += "&quot;";
						break;
					case '\'':
						out += "&apos;";
						break;
					default:
						out += c;
						break;
				}
			}
			return out;
		}

		std::filesystem::path AbsolutePath(const std::filesystem::path& path)
		{
			if (path.empty())
			{
				return {};
			}
			std::error_code ec;
			const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
			return ec ? path.lexically_normal() : absolute.lexically_normal();
		}

		std::filesystem::path ManagedSdkProjectPath()
		{
#ifdef AETHER_MANAGED_SDK_PROJECT
			return AbsolutePath(AETHER_MANAGED_SDK_PROJECT);
#else
			return {};
#endif
		}

		bool SeedProjectTemplateFiles(const std::filesystem::path& root, ProjectTemplate projectTemplate, std::string& error)
		{
			const std::filesystem::path scriptsProject = root / "scripts" / "AetherGame.csproj";
			if (!io::file_util::Exists(scriptsProject))
			{
				if (auto writeResult = io::file_util::WriteText(scriptsProject, MakeProjectScriptCsprojText(ManagedSdkProjectPath())); !writeResult)
				{
					error = "Could not write project scripts file: " + writeResult.error().message;
					return false;
				}
			}

#ifdef AETHER_SCENES_SOURCE_DIR
			const std::filesystem::path seedScene = root / "scenes" / "default.scene.toml";
			if (!io::file_util::Exists(seedScene))
			{
				if (auto dirResult = io::file_util::CreateDirectories(seedScene.parent_path()); !dirResult)
				{
					error = "Could not create project folder: " + dirResult.error().message;
					return false;
				}
				const std::string_view sourceScene = projectTemplate == ProjectTemplate::Blank2D ? "default2d.scene.toml" : "default.scene.toml";
				if (auto copyResult = io::file_util::CopyFile(std::filesystem::path(AETHER_SCENES_SOURCE_DIR) / sourceScene, seedScene); !copyResult)
				{
					error = "Could not copy project template scene: " + copyResult.error().message;
					return false;
				}
			}
#endif
			return true;
		}
	} // namespace

	std::filesystem::path NormalizePath(std::filesystem::path path)
	{
		std::error_code ec;
		if (path.empty())
		{
			return {};
		}
		path = std::filesystem::absolute(path, ec);
		if (ec)
		{
			return path.lexically_normal();
		}
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
		return ec ? path.lexically_normal() : canonical;
	}

	std::string DisplayPath(const std::filesystem::path& path)
	{
		return path.empty() ? std::string{} : path.lexically_normal().string();
	}

	std::filesystem::path ProjectFilePath(const std::filesystem::path& root)
	{
		return root / kProjectFileName;
	}

	std::filesystem::path ResolveProjectRoot(std::filesystem::path path)
	{
		path = NormalizePath(std::move(path));
		if (path.empty())
		{
			return {};
		}
		if (path.filename() == kProjectFileName)
		{
			path = path.parent_path();
		}
		return path;
	}

	bool HasProjectDescriptor(const std::filesystem::path& root)
	{
		return io::file_util::Exists(ProjectFilePath(root));
	}

	std::string FallbackProjectName(const std::filesystem::path& root)
	{
		const std::string name = root.filename().string();
		return name.empty() ? "Aether Project" : name;
	}

	std::filesystem::path PreviewImagePath(const std::filesystem::path& root)
	{
		return root / ".aether" / "preview.png";
	}

	std::string LastModifiedLabel(const std::filesystem::path& root)
	{
		namespace fs = std::filesystem;
		fs::file_time_type newest{};
		bool any = false;
		for (const fs::path& candidate: {ProjectFilePath(root), PreviewImagePath(root)})
		{
			std::error_code ec;
			const fs::file_time_type time = fs::last_write_time(candidate, ec);
			if (!ec && (!any || time > newest))
			{
				newest = time;
				any = true;
			}
		}
		if (!any)
		{
			return {};
		}

		const auto when = std::chrono::clock_cast<std::chrono::system_clock>(newest);
		const long long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - when).count();
		if (secs < 60)
		{
			return "just now";
		}
		const long long minutes = secs / 60;
		if (minutes < 60)
		{
			return std::format("{} min ago", minutes);
		}
		const long long hours = minutes / 60;
		if (hours < 24)
		{
			return std::format("{} hr ago", hours);
		}
		const long long days = hours / 24;
		if (days < 7)
		{
			return std::format("{} day{} ago", days, days == 1 ? "" : "s");
		}
		if (days < 30)
		{
			const long long weeks = days / 7;
			return std::format("{} week{} ago", weeks, weeks == 1 ? "" : "s");
		}
		return std::format("{:%b %d, %Y}", std::chrono::floor<std::chrono::days>(when));
	}

	Expected<EditorProjectContext> ReadProjectDescriptor(const std::filesystem::path& root)
	{
		EditorProjectContext project;
		project.root = NormalizePath(root);
		project.projectFile = ProjectFilePath(project.root);
		project.name = FallbackProjectName(project.root);
		project.assetsDir = ResolveProjectPath(project.root, {}, "assets");
		project.scenesDir = ResolveProjectPath(project.root, {}, "scenes");
		project.prefabsDir = ResolveProjectPath(project.root, {}, "assets/prefabs");
		project.scriptsDir = ResolveProjectPath(project.root, {}, "scripts");

		auto descriptorText = io::file_util::ReadText(ProjectFilePath(root));
		if (!descriptorText)
		{
			AE_UNEXPECTED(AetherError::Engine("No project descriptor found."));
		}

		std::string assetsPath;
		std::string scenesPath;
		std::string prefabsPath;
		std::string scriptsPath;
		try
		{
			text::ParseToml(*descriptorText,
			        [&](const text::IniEntry& entry)
			        {
				        if (entry.fullKey == "project.name")
				        {
					        project.name = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "project.kind")
				        {
					        project.kind = text::StripQuotes(entry.value) == "2d" ? ProjectKind::Scene2D : ProjectKind::Scene3D;
				        }
				        else if (entry.fullKey == "paths.assets")
				        {
					        assetsPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.scenes")
				        {
					        scenesPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.prefabs")
				        {
					        prefabsPath = text::StripQuotes(entry.value);
				        }
				        else if (entry.fullKey == "paths.scripts")
				        {
					        scriptsPath = text::StripQuotes(entry.value);
				        }
			        });
		}
		catch (...)
		{
			return project;
		}

		if (project.name.empty())
		{
			project.name = FallbackProjectName(project.root);
		}
		project.assetsDir = ResolveProjectPath(project.root, assetsPath, "assets");
		project.scenesDir = ResolveProjectPath(project.root, scenesPath, "scenes");
		const std::string inferredPrefabsPath = !prefabsPath.empty() ? prefabsPath : (!assetsPath.empty() ? assetsPath + "/prefabs" : std::string{});
		project.prefabsDir = ResolveProjectPath(project.root, inferredPrefabsPath, "assets/prefabs");
		project.scriptsDir = ResolveProjectPath(project.root, scriptsPath, "scripts");
		return project;
	}

	std::string ReadProjectName(const std::filesystem::path& root)
	{
		auto result = ReadProjectDescriptor(root);
		return result.has_value() ? result->name : FallbackProjectName(root);
	}

	std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject)
	{
		const std::filesystem::path sdkProject = AbsolutePath(managedSdkProject);
		return "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
		       "\n"
		       "  <!--\n"
		       "    Project-owned game scripts. The editor builds this assembly at runtime\n"
		       "    from the open project, then loads AetherGame.dll through the collectible\n"
		       "    scripting context.\n"
		       "\n"
		       "    AetherCore is compile-only because the engine already loads the SDK assembly.\n"
		       "    This reference is intentionally absolute so projects created outside the\n"
		       "    engine checkout can still compile from their own scripts folder.\n"
		       "  -->\n"
		       "  <PropertyGroup>\n"
		       "    <AssemblyName>AetherGame</AssemblyName>\n"
		       "    <RootNamespace>AetherGame</RootNamespace>\n"
		       "    <TargetFramework>net10.0</TargetFramework>\n"
		       "    <Nullable>enable</Nullable>\n"
		       "    <LangVersion>latest</LangVersion>\n"
		       "    <ImplicitUsings>disable</ImplicitUsings>\n"
		       "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
		       "  </PropertyGroup>\n"
		       "\n"
		       "  <ItemGroup>\n"
		       "    <ProjectReference Include=\""
		       + EscapeXmlAttribute(sdkProject.generic_string())
		       + "\"\n"
		         "                      Private=\"false\"\n"
		         "                      ExcludeAssets=\"runtime\" />\n"
		         "  </ItemGroup>\n"
		         "\n"
		         "</Project>\n";
	}

	bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error, ProjectTemplate projectTemplate)
	{
		if (auto dirResult = io::file_util::CreateDirectories(root); !dirResult)
		{
			error = "Could not create project directory: " + dirResult.error().message;
			return false;
		}

		for (const std::string_view dir: {"assets"sv, "assets/models"sv, "assets/materials"sv, "assets/textures"sv, "assets/animations"sv, "assets/prefabs"sv, "data"sv, "scenes"sv, "scripts"sv})
		{
			if (auto dirResult = io::file_util::CreateDirectories(root / std::filesystem::path(dir)); !dirResult)
			{
				error = "Could not create project folder: " + dirResult.error().message;
				return false;
			}
		}

		const bool is2D = ProjectKindForTemplate(projectTemplate) == ProjectKind::Scene2D;
		const std::string descriptor = "# AetherCore project file.\n\n"
		                               "[project]\nversion = 2\nname = \""
		                               + EscapeTomlString(name) + "\"\nkind = \"" + (is2D ? "2d" : "3d") + "\"\ntemplate = \"" + (is2D ? "blank_2d" : "blank_3d")
		                               + "\"\n\n"
		                                 "[paths]\nassets = \"assets\"\nscenes = \"scenes\"\nprefabs = \"assets/prefabs\"\nscripts = \"scripts\"\n\n"
		                                 "[app]\nstartupScene = \"default\"\n\n"
		                                 "[publish]\nplatformName = \"Windows\"\nproductName = \""
		                               + EscapeTomlString(name) + "\"\n";
		if (auto writeResult = io::file_util::WriteText(ProjectFilePath(root), descriptor); !writeResult)
		{
			error = "Could not write ProjectSettings.toml.";
			return false;
		}
		return SeedProjectTemplateFiles(root, projectTemplate, error);
	}

#ifdef _WIN32
	std::optional<std::filesystem::path> PickProjectFolder()
	{
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool uninitialize = SUCCEEDED(coInit);

		IFileDialog* dialog = nullptr;
		HRESULT const hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		if (FAILED(hr) || dialog == nullptr)
		{
			if (uninitialize)
			{
				CoUninitialize();
			}
			return std::nullopt;
		}

		DWORD options = 0;
		if (SUCCEEDED(dialog->GetOptions(&options)))
		{
			dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		}
		dialog->SetTitle(L"Select AetherCore Project Folder");

		std::optional<std::filesystem::path> selected;
		if (SUCCEEDED(dialog->Show(nullptr)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
			{
				PWSTR rawPath = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr)
				{
					selected = std::filesystem::path(rawPath);
					CoTaskMemFree(rawPath);
				}
				item->Release();
			}
		}

		dialog->Release();
		if (uninitialize)
		{
			CoUninitialize();
		}
		return selected;
	}

	std::optional<std::filesystem::path> PickProjectFile()
	{
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool uninitialize = SUCCEEDED(coInit);

		IFileDialog* dialog = nullptr;
		HRESULT const hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
		if (FAILED(hr) || dialog == nullptr)
		{
			if (uninitialize)
			{
				CoUninitialize();
			}
			return std::nullopt;
		}

		DWORD options = 0;
		if (SUCCEEDED(dialog->GetOptions(&options)))
		{
			dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
		}
		const COMDLG_FILTERSPEC filters[] = {{L"AetherCore project", L"ProjectSettings.toml"}, {L"TOML files", L"*.toml"}};
		dialog->SetFileTypes(2, filters);
		dialog->SetFileName(L"ProjectSettings.toml");
		dialog->SetTitle(L"Select ProjectSettings.toml");

		std::optional<std::filesystem::path> selected;
		if (SUCCEEDED(dialog->Show(nullptr)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
			{
				PWSTR rawPath = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr)
				{
					selected = std::filesystem::path(rawPath);
					CoTaskMemFree(rawPath);
				}
				item->Release();
			}
		}

		dialog->Release();
		if (uninitialize)
		{
			CoUninitialize();
		}
		return selected;
	}
#else
	std::optional<std::filesystem::path> PickProjectFolder()
	{
		return std::nullopt;
	}

	std::optional<std::filesystem::path> PickProjectFile()
	{
		return std::nullopt;
	}
#endif

	std::vector<EditorProjectContext> LoadRecentProjects(TomlConfig& config)
	{
		std::vector<EditorProjectContext> recents;
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			EditorProjectContext project;
			project.root = NormalizePath(config.GetString(key + ".path"));
			project.name = config.GetString(key + ".name");
			if (project.root.empty())
			{
				continue;
			}
			if (project.name.empty())
			{
				project.name = ReadProjectName(project.root);
			}
			if (std::ranges::none_of(recents, [&](const EditorProjectContext& existing) { return NormalizePath(existing.root) == project.root; }))
			{
				recents.push_back(std::move(project));
			}
		}
		return recents;
	}

	void SaveRecentProjects(TomlConfig& config, std::span<const EditorProjectContext> recents)
	{
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			if (i < static_cast<int>(recents.size()))
			{
				config.Set(key + ".path", DisplayPath(recents[static_cast<std::size_t>(i)].root));
				config.Set(key + ".name", recents[static_cast<std::size_t>(i)].name);
			}
			else
			{
				config.Set(key + ".path", std::string_view{});
				config.Set(key + ".name", std::string_view{});
			}
		}
	}
} // namespace aether::app::project
