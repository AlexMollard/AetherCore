#include "scripting/CSharpScriptingSubsystem.hpp"

#include <array>
#include <cstdio>
#include <optional>
#include <string>

#include "io/FileUtil.hpp"

#include "utils/Logger.hpp"

namespace aether::app::scripting
{
	namespace
	{
		// Locate the deployed managed assemblies (data/scripts/managed) relative to
		// the working directory.
		std::filesystem::path ResolveManagedDir()
		{
			const auto cwd = std::filesystem::current_path();
			const std::array<std::filesystem::path, 3> candidates = {
			        cwd / "data" / "scripts" / "managed",
			        cwd / ".." / "data" / "scripts" / "managed",
			        cwd / ".." / ".." / "data" / "scripts" / "managed",
			};
			for (const auto& dir: candidates)
			{
				if (io::file_util::Exists(dir / "AetherCore.Interop.dll"))
				{
					return dir;
				}
			}
			return candidates[0];
		}
	} // namespace

	CSharpScriptingSubsystem::CSharpScriptingSubsystem()
	{
		m_managedDir = ResolveManagedDir();
		m_host.SetErrorHandler([this](const std::string& error) { ReportScriptError(error); });

		if (m_host.Initialize(m_managedDir))
		{
			m_scriptsAssemblyPath = (m_managedDir / "AetherGame.dll").string();
			LoadScripts();
		}
	}

	const aether::scripting::ManagedScriptApi* CSharpScriptingSubsystem::Api() const
	{
		return m_host.IsAvailable() ? &m_host.Api() : nullptr;
	}

	int CSharpScriptingSubsystem::LoadScripts()
	{
		const auto* api = Api();
		if (api == nullptr || api->LoadScripts == nullptr)
		{
			return -1;
		}

		const int count = api->LoadScripts(m_scriptsAssemblyPath.c_str());
		RefreshTypeNames();
		if (count < 0)
		{
			AE_WARN(LogCategory::App, "C# scripts failed to load from '{}'", m_scriptsAssemblyPath);
		}
		else
		{
			AE_INFO(LogCategory::App, "C# scripts loaded: {} type(s)", count);
		}
		return count;
	}

	namespace
	{
#if defined(AETHER_GAME_PROJECT) && defined(AETHER_DOTNET_EXE)
		// Runs a shell command, capturing combined stdout+stderr. Returns the
		// process exit code (0 == success), or -1 if the process failed to start.
		int RunCapture(const std::string& command, std::string& output)
		{
#	ifdef _WIN32
			FILE* pipe = _popen(command.c_str(), "r");
#	else
			FILE* pipe = popen(command.c_str(), "r");
#	endif
			if (pipe == nullptr)
			{
				output = "failed to start build process";
				return -1;
			}
			char buffer[512];
			while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
			{
				output += buffer;
			}
#	ifdef _WIN32
			return _pclose(pipe);
#	else
			return pclose(pipe);
#	endif
		}

		bool IsScriptBuildInput(const std::filesystem::path& path)
		{
			if (!path.has_extension())
			{
				return false;
			}

			const std::string extension = path.extension().string();
			return extension == ".cs" || extension == ".csproj" || extension == ".props" || extension == ".targets" || extension == ".json";
		}

		std::optional<std::filesystem::file_time_type> LatestWriteTime(const std::filesystem::path& path)
		{
			std::error_code ec;
			const auto time = std::filesystem::last_write_time(path, ec);
			if (ec)
			{
				return std::nullopt;
			}
			return time;
		}

		bool AccumulateLatestScriptInputTime(const std::filesystem::path& root, std::optional<std::filesystem::file_time_type>& latest)
		{
			const auto rememberLatest = [&latest](const std::filesystem::file_time_type time)
			{
				if (!latest || time > *latest)
				{
					latest = time;
				}
			};

			std::error_code ec;
			if (!std::filesystem::exists(root, ec))
			{
				return false;
			}

			if (std::filesystem::is_regular_file(root, ec))
			{
				if (!IsScriptBuildInput(root))
				{
					return true;
				}
				const auto time = LatestWriteTime(root);
				if (!time)
				{
					return false;
				}
				rememberLatest(*time);
				return true;
			}

			if (!std::filesystem::is_directory(root, ec))
			{
				return true;
			}

			std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
			const std::filesystem::recursive_directory_iterator end;
			for (; !ec && it != end; it.increment(ec))
			{
				const std::filesystem::directory_entry& entry = *it;
				if (entry.is_directory(ec))
				{
					const std::string name = entry.path().filename().string();
					if (name == "bin" || name == "obj" || name == "artifacts" || name == ".vs" || name == ".git")
					{
						it.disable_recursion_pending();
					}
					continue;
				}

				if (!entry.is_regular_file(ec) || !IsScriptBuildInput(entry.path()))
				{
					continue;
				}

				const auto time = LatestWriteTime(entry.path());
				if (!time)
				{
					return false;
				}
				rememberLatest(*time);
			}

			return !ec;
		}

		bool IsScriptBuildRequired(const std::filesystem::path& gameProject, const std::filesystem::path& managedDir)
		{
			const std::filesystem::path deployedAssembly = managedDir / "AetherGame.dll";
			if (!std::filesystem::exists(managedDir / "AetherGame.deps.json"))
			{
				return true;
			}

			const auto deployedTime = LatestWriteTime(deployedAssembly);
			if (!deployedTime)
			{
				return true;
			}

			std::optional<std::filesystem::file_time_type> latestInput;
			if (!AccumulateLatestScriptInputTime(gameProject.parent_path(), latestInput))
			{
				return true;
			}

			const std::filesystem::path repoRoot = std::filesystem::path(AETHER_MANAGED_SDK_PROJECT).parent_path().parent_path().parent_path();
			const std::filesystem::path directoryBuildProps = repoRoot / "Directory.Build.props";
			(void) AccumulateLatestScriptInputTime(directoryBuildProps, latestInput);

			return latestInput && *latestInput > *deployedTime;
		}
#endif
	} // namespace

	// Dev-only. Rebuilds AetherGame from source and redeploys it before a reload so
	// F5 reflects source edits with no separate build step. In a packaged build the
	// AETHER_GAME_PROJECT / AETHER_DOTNET_EXE definitions are absent and this is a
	// no-op (reload-only). SDK/Interop edits are NOT hot-reloadable and still need a
	// full rebuild + restart; only the collectible game assembly is swapped here.
	bool CSharpScriptingSubsystem::RebuildFromSource(std::string& error)
	{
#if defined(AETHER_GAME_PROJECT) && defined(AETHER_DOTNET_EXE)
		namespace fs = std::filesystem;

		const fs::path gameProject = AETHER_GAME_PROJECT;
		if (!IsScriptBuildRequired(gameProject, m_managedDir))
		{
			AE_VERBOSE(LogCategory::App, "C# scripts are current; skipping dotnet build.");
			return true;
		}

		// Incremental `dotnet build` of the game project (a no-op when it was just
		// built in VS). ArtifactsPath mirrors the CMake managed build.
		const std::string inner = std::string("\"") + AETHER_DOTNET_EXE + "\" build \"" + gameProject.string() + "\" -c " + AETHER_MANAGED_CONFIG + " --nologo -v:m -p:ArtifactsPath=\"" + AETHER_MANAGED_ARTIFACTS + "\" 2>&1";
#	ifdef _WIN32
		// cmd.exe needs the whole command re-wrapped so the quoted, spaced exe path parses.
		const std::string command = "\"" + inner + "\"";
#	else
		const std::string command = inner;
#	endif

		std::string output;
		const int rc = RunCapture(command, output);
		if (rc != 0)
		{
			error = output.empty() ? "dotnet build failed" : output;
			return false;
		}

		// Redeploy the freshly built game assembly into the load dir. The scripts
		// ALC loads from bytes (see ScriptRegistry.Load), so overwriting the on-disk
		// dll mid-run is safe.
		const fs::path buildOut = fs::path(AETHER_MANAGED_ARTIFACTS) / "bin" / "AetherGame" / AETHER_MANAGED_CONFIGDIR;
		for (const char* name: {"AetherGame.dll", "AetherGame.pdb", "AetherGame.deps.json"})
		{
			const fs::path src = buildOut / name;
			if (!fs::exists(src))
			{
				continue;
			}
			std::error_code ec;
			fs::copy_file(src, m_managedDir / name, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				error = std::string("failed to deploy ") + name + ": " + ec.message();
				return false;
			}
		}
		return true;
#else
		(void) error;
		return true; // packaged build / no SDK: reload-only
#endif
	}

	void CSharpScriptingSubsystem::RefreshTypeNames()
	{
		m_typeNames.clear();
		const auto* api = Api();
		if (api == nullptr || api->GetScriptTypeCount == nullptr || api->GetScriptTypeName == nullptr)
		{
			return;
		}

		const int count = api->GetScriptTypeCount();
		std::array<char, 256> buffer{};
		for (int i = 0; i < count; ++i)
		{
			const int written = api->GetScriptTypeName(i, buffer.data(), static_cast<int>(buffer.size()));
			if (written > 0)
			{
				m_typeNames.emplace_back(buffer.data(), static_cast<size_t>(written));
			}
		}
	}

	std::vector<ScriptPropertyInfo> CSharpScriptingSubsystem::GetScriptProperties(const std::string& typeName) const
	{
		std::vector<ScriptPropertyInfo> out;
		const auto* api = Api();
		if (api == nullptr || api->GetPropertyCount == nullptr || api->GetPropertyInfo == nullptr)
		{
			return out;
		}
		const int count = api->GetPropertyCount(typeName.c_str());
		std::array<char, 128> buffer{};
		for (int i = 0; i < count; ++i)
		{
			std::int32_t typeTag = 0;
			const int written = api->GetPropertyInfo(typeName.c_str(), i, buffer.data(), static_cast<int>(buffer.size()), &typeTag);
			if (written > 0)
			{
				out.push_back(ScriptPropertyInfo{.name = std::string(buffer.data(), static_cast<size_t>(written)), .type = static_cast<aether::ScriptPropertyValue::Type>(typeTag)});
			}
		}
		return out;
	}

	int CSharpScriptingSubsystem::FindPropertyIndex(const std::string& typeName, const std::string& name) const
	{
		const auto props = GetScriptProperties(typeName);
		for (size_t i = 0; i < props.size(); ++i)
		{
			if (props[i].name == name)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	bool CSharpScriptingSubsystem::GetPropertyValue(std::uint64_t handle, int index, aether::ScriptPropertyValue& out) const
	{
		const auto* api = Api();
		if (api == nullptr || api->GetProperty == nullptr || handle == 0)
		{
			return false;
		}
		aether::scripting::PropertyValue pv{};
		if (api->GetProperty(handle, index, &pv) == 0)
		{
			return false;
		}
		out.type = static_cast<aether::ScriptPropertyValue::Type>(static_cast<std::int32_t>(pv.type));
		out.f4[0] = pv.f4[0];
		out.f4[1] = pv.f4[1];
		out.f4[2] = pv.f4[2];
		out.f4[3] = pv.f4[3];
		out.i64 = pv.i64;
		out.str = pv.str != nullptr ? pv.str : "";
		return true;
	}

	void CSharpScriptingSubsystem::SetPropertyValue(std::uint64_t handle, int index, const aether::ScriptPropertyValue& value) const
	{
		const auto* api = Api();
		if (api == nullptr || api->SetProperty == nullptr || handle == 0)
		{
			return;
		}
		aether::scripting::PropertyValue pv{};
		pv.type = static_cast<aether::scripting::PropertyType>(static_cast<std::int32_t>(value.type));
		pv.f4[0] = value.f4[0];
		pv.f4[1] = value.f4[1];
		pv.f4[2] = value.f4[2];
		pv.f4[3] = value.f4[3];
		pv.i64 = value.i64;
		pv.str = value.str.c_str();
		api->SetProperty(handle, index, &pv);
	}

	bool CSharpScriptingSubsystem::GetDefaultPropertyValue(const std::string& typeName, int index, aether::ScriptPropertyValue& out) const
	{
		const auto* api = Api();
		if (api == nullptr || api->GetDefaultProperty == nullptr)
		{
			return false;
		}
		aether::scripting::PropertyValue pv{};
		if (api->GetDefaultProperty(typeName.c_str(), index, &pv) == 0)
		{
			return false;
		}
		out.type = static_cast<aether::ScriptPropertyValue::Type>(static_cast<std::int32_t>(pv.type));
		out.f4[0] = pv.f4[0];
		out.f4[1] = pv.f4[1];
		out.f4[2] = pv.f4[2];
		out.f4[3] = pv.f4[3];
		out.i64 = pv.i64;
		out.str = pv.str != nullptr ? pv.str : "";
		return true;
	}

	void CSharpScriptingSubsystem::ApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, aether::ScriptPropertyValue>& props) const
	{
		if (props.empty() || handle == 0)
		{
			return;
		}
		const auto infos = GetScriptProperties(typeName);
		for (const auto& [name, value]: props)
		{
			for (size_t i = 0; i < infos.size(); ++i)
			{
				if (infos[i].name == name)
				{
					SetPropertyValue(handle, static_cast<int>(i), value);
					break;
				}
			}
		}
	}

	void CSharpScriptingSubsystem::ReportScriptError(const std::string& error)
	{
		AE_ERROR(LogCategory::App, "C# script error: {}", error);
		m_pendingErrors.push_back(error);
	}

	std::vector<std::string> CSharpScriptingSubsystem::PollPendingErrors()
	{
		std::vector<std::string> out;
		out.swap(m_pendingErrors);
		return out;
	}

	void CSharpScriptingSubsystem::ClearErrors()
	{
		m_pendingErrors.clear();
		m_errorsCleared = true;
	}

	bool CSharpScriptingSubsystem::ConsumeErrorsCleared()
	{
		const bool cleared = m_errorsCleared;
		m_errorsCleared = false;
		return cleared;
	}
} // namespace aether::app::scripting
