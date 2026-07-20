#include "editor/VisualStudioScriptDebug.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/Process.hpp"
#include "project/ProjectCommon.hpp"

#ifdef _WIN32
#	include <Windows.h>
#	include <objbase.h>
#	include <shellapi.h>
#	include <winver.h>

#	pragma comment(lib, "version.lib")
#endif

namespace aether::editor
{
	namespace
	{
#ifdef _WIN32
		std::string Trim(const std::string& text)
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				return {};
			}
			const auto last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		std::optional<std::filesystem::path> FindVSWhere()
		{
			char programFiles[32768]{};
			constexpr DWORD kBufferSize = sizeof(programFiles) / sizeof(programFiles[0]);
			const DWORD written = GetEnvironmentVariableA("ProgramFiles(x86)", programFiles, kBufferSize);
			if (written == 0 || written >= kBufferSize)
			{
				return std::nullopt;
			}

			std::filesystem::path vswhere = std::filesystem::path(programFiles) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
			if (!io::file_util::Exists(vswhere))
			{
				return std::nullopt;
			}

			return vswhere;
		}

		std::vector<std::filesystem::path> ParseLines(std::string_view output)
		{
			std::vector<std::filesystem::path> paths;
			std::size_t start = 0;
			while (start < output.size())
			{
				const std::size_t end = output.find('\n', start);
				const std::string line = Trim(std::string(output.substr(start, end - start)));
				if (!line.empty())
				{
					paths.emplace_back(line);
				}
				if (end == std::string_view::npos)
				{
					break;
				}
				start = end + 1;
			}
			return paths;
		}

		std::string DisplayNameFor(const std::filesystem::path& installPath)
		{
			const std::string leaf = installPath.filename().string();
			return leaf.empty() ? "Visual Studio (" + installPath.string() + ")" : "Visual Studio " + leaf + " (" + installPath.string() + ")";
		}

		int VisualStudioMajorVersion(const std::filesystem::path& devenv)
		{
			DWORD unused = 0;
			const DWORD size = GetFileVersionInfoSizeW(devenv.c_str(), &unused);
			if (size == 0)
			{
				return 0;
			}

			std::vector<std::byte> data(size);
			if (GetFileVersionInfoW(devenv.c_str(), 0, size, data.data()) == 0)
			{
				return 0;
			}

			VS_FIXEDFILEINFO* versionInfo = nullptr;
			UINT versionInfoSize = 0;
			if ((VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&versionInfo), &versionInfoSize) == 0) || versionInfo == nullptr || versionInfoSize < sizeof(VS_FIXEDFILEINFO))
			{
				return 0;
			}
			return static_cast<int>(HIWORD(versionInfo->dwFileVersionMS));
		}

		std::wstring DteProgId(int majorVersion)
		{
			return L"VisualStudio.DTE." + std::to_wstring(majorVersion) + L".0";
		}

		bool HasDteAutomation(int majorVersion)
		{
			CLSID classId{};
			const std::wstring progId = DteProgId(majorVersion);
			return CLSIDFromProgID(progId.c_str(), &classId) == S_OK;
		}

		std::optional<std::filesystem::path> FindPowerShell()
		{
			wchar_t systemDirectory[MAX_PATH]{};
			const UINT written = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
			if (written == 0 || written >= std::size(systemDirectory))
			{
				return std::nullopt;
			}

			const std::filesystem::path powershell = std::filesystem::path(systemDirectory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
			return io::file_util::Exists(powershell) ? std::optional{powershell} : std::nullopt;
		}

		std::wstring PowerShellLiteral(const std::wstring& value)
		{
			std::wstring escaped;
			escaped.reserve(value.size() + 2);
			escaped.push_back(L'\'');
			for (const wchar_t character: value)
			{
				escaped.push_back(character);
				if (character == L'\'')
				{
					escaped.push_back(character);
				}
			}
			escaped.push_back(L'\'');
			return escaped;
		}

		std::wstring EncodePowerShellCommand(const std::wstring& command)
		{
			static constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			static_assert(sizeof(wchar_t) == 2, "PowerShell -EncodedCommand requires UTF-16LE input.");

			const auto* bytes = reinterpret_cast<const unsigned char*>(command.data());
			const std::size_t byteCount = command.size() * sizeof(wchar_t);
			std::string encoded;
			encoded.reserve((byteCount + 2) / 3 * 4);
			for (std::size_t index = 0; index < byteCount; index += 3)
			{
				const unsigned int block = static_cast<unsigned int>(bytes[index]) << 16 | (index + 1 < byteCount ? static_cast<unsigned int>(bytes[index + 1]) << 8 : 0) | (index + 2 < byteCount ? bytes[index + 2] : 0);
				encoded.push_back(kBase64[(block >> 18) & 0x3f]);
				encoded.push_back(kBase64[(block >> 12) & 0x3f]);
				encoded.push_back(index + 1 < byteCount ? kBase64[(block >> 6) & 0x3f] : '=');
				encoded.push_back(index + 2 < byteCount ? kBase64[block & 0x3f] : '=');
			}
			return {encoded.begin(), encoded.end()};
		}

		const VisualStudioInstallation* FindInstallation(const std::vector<VisualStudioInstallation>& installations, const std::filesystem::path& selectedInstall)
		{
			if (selectedInstall.empty())
			{
				const auto compatible = std::ranges::find_if(installations, [](const VisualStudioInstallation& installation) { return installation.supportsDotNet10 && installation.hasDebuggerAutomation; });
				return compatible != installations.end() ? &*compatible : nullptr;
			}

			const auto selected = std::ranges::find(installations, selectedInstall, &VisualStudioInstallation::installPath);
			return selected != installations.end() ? &*selected : nullptr;
		}

		bool ShellOpen(const std::filesystem::path& executable, const std::wstring& arguments)
		{
			const HINSTANCE result = ShellExecuteW(nullptr, L"open", executable.c_str(), arguments.empty() ? nullptr : arguments.c_str(), nullptr, SW_HIDE);
			return reinterpret_cast<std::intptr_t>(result) > 32;
		}

		bool LaunchVisualStudioAutomation(const VisualStudioInstallation& installation, const std::filesystem::path& solution)
		{
			const auto powershell = FindPowerShell();
			if (!powershell)
			{
				return false;
			}

			// Open the saved per-project solution in a fresh DTE instance and attach
			// to this Editor. `New-Object -ComObject VisualStudio.DTE` starts VS in
			// COM automation/embedding mode, which shuts down when its automation
			// client (this hidden PowerShell) releases it on exit. Setting
			// `UserControl = $true` hands the instance to the user so it stays open
			// afterwards - otherwise VS tears down right after Attach() and, being
			// mid-debug, prompts "stop debugging?" then closes.
			std::wstring command = L"$ErrorActionPreference='Stop';";
			command += L"$dte=New-Object -ComObject " + PowerShellLiteral(DteProgId(installation.majorVersion)) + L";";
			command += L"$dte.UserControl=$true;";
			command += L"$dte.MainWindow.Visible=$true;";
			command += L"$dte.Solution.Open(" + PowerShellLiteral(solution.wstring()) + L");";
			command += L"$target=$null;for($attempt=0;$attempt -lt 40 -and $null -eq $target;$attempt++){foreach($candidate in $dte.Debugger.LocalProcesses){if($candidate.ProcessID -eq " + std::to_wstring(GetCurrentProcessId());
			command += L"){$target=$candidate;break}};if($null -eq $target){Start-Sleep -Milliseconds 250}};";
			command += L"if($null -eq $target){throw 'Could not locate the Editor process for Visual Studio attachment.'};$target.Attach();";
			return ShellOpen(*powershell, L"-NoProfile -WindowStyle Hidden -EncodedCommand " + EncodePowerShellCommand(command));
		}
#endif
	} // namespace

	std::vector<VisualStudioInstallation> FindVisualStudioInstallations()
	{
		std::vector<VisualStudioInstallation> installations;
#ifdef _WIN32
		const auto vswhere = FindVSWhere();
		if (!vswhere)
		{
			return installations;
		}

		std::string output;
		const std::string command = "\"" + vswhere->string() + "\" -all -products * -format value -property installationPath";
		if (io::RunProcessCapture(command, output) != 0)
		{
			return installations;
		}

		for (const std::filesystem::path& installPath: ParseLines(output))
		{
			const std::filesystem::path devenv = installPath / "Common7" / "IDE" / "devenv.exe";
			if (!io::file_util::Exists(devenv))
			{
				continue;
			}
			const int majorVersion = VisualStudioMajorVersion(devenv);
			installations.push_back({.installPath = installPath, .displayName = DisplayNameFor(installPath), .majorVersion = majorVersion, .supportsDotNet10 = majorVersion >= 18, .hasDebuggerAutomation = HasDteAutomation(majorVersion)});
		}
#endif
		return installations;
	}

	EditorProjectActionResult OpenVisualStudioAndAttachScriptDebugger(const std::filesystem::path& scriptsProject, const std::filesystem::path& visualStudioInstall)
	{
		if (!io::file_util::Exists(scriptsProject))
		{
			return {.succeeded = false, .message = "Project scripts are missing: " + scriptsProject.string()};
		}

#ifdef _WIN32
		const std::vector<VisualStudioInstallation> installations = FindVisualStudioInstallations();
		const VisualStudioInstallation* installation = FindInstallation(installations, visualStudioInstall);
		if (installation == nullptr)
		{
			return {.succeeded = false, .message = "Visual Studio 2026 or newer is required for .NET 10 C# scripts."};
		}

		if (!installation->supportsDotNet10)
		{
			return {.succeeded = false, .message = installation->displayName + " is not compatible with .NET 10. Choose Visual Studio 2026 or newer."};
		}

		if (!installation->hasDebuggerAutomation)
		{
			return {.succeeded = false, .message = installation->displayName + " does not expose Visual Studio automation for debugger attachment."};
		}

		// The scripts project lives at <root>/scripts/AetherGame.csproj; the generated
		// solution sits at the project root.
		const std::filesystem::path projectRoot = scriptsProject.parent_path().parent_path();
		std::string solutionError;
		if (!app::project::EnsureGameSolution(projectRoot, solutionError))
		{
			return {.succeeded = false, .message = solutionError};
		}
		const std::filesystem::path solution = projectRoot / "AetherGame.slnx";
		if (!io::file_util::Exists(solution))
		{
			return {.succeeded = false, .message = "The AetherGame solution could not be generated for Visual Studio."};
		}

		if (!LaunchVisualStudioAutomation(*installation, solution))
		{
			return {.succeeded = false, .message = "Could not start Visual Studio's debugger automation."};
		}

		return {.succeeded = true, .message = "Opening AetherGame.slnx in Visual Studio and attaching that IDE to this Editor. Set breakpoints, then press Play."};
#else
		return {.succeeded = false, .message = "Visual Studio script debugging is currently available on Windows only."};
#endif
	}
} // namespace aether::editor
