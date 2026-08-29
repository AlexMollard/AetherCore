#include "scripting/DotNetHost.hpp"

#include "utils/Logger.hpp"

#if AETHER_HAS_DOTNET

#	include <filesystem>
#	include <string_view>
#	include <system_error>

#	include <nethost.h>
#	include <coreclr_delegates.h>
#	include <hostfxr.h>

#	include "io/PlatformPaths.hpp"

#	ifdef _WIN32
#		include <windows.h>
#	else
#		include <dlfcn.h>
#	endif

namespace aether::scripting
{
	namespace
	{
		DotNetHost* g_host = nullptr;

#	ifdef _WIN32
		using string_t = std::wstring;

		string_t ToCharT(const std::filesystem::path& p)
		{
			return p.wstring();
		}

		std::string NarrowCharT(const char_t* wide)
		{
			if (wide == nullptr || *wide == 0)
			{
				return {};
			}
			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
			if (needed <= 1)
			{
				return {};
			}
			std::string out(static_cast<size_t>(needed - 1), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
			return out;
		}

		void* LoadHostLibrary(const char_t* path)
		{
			return reinterpret_cast<void*>(::LoadLibraryW(path));
		}

		void* GetHostExport(void* lib, const char* name)
		{
			return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
		}
#	else
		using string_t = std::string;

		string_t ToCharT(const std::filesystem::path& p)
		{
			return p.string();
		}

		std::string NarrowCharT(const char_t* s)
		{
			return s != nullptr ? std::string(s) : std::string{};
		}

		void* LoadHostLibrary(const char_t* path)
		{
			return ::dlopen(path, RTLD_LAZY | RTLD_LOCAL);
		}

		void* GetHostExport(void* lib, const char* name)
		{
			return ::dlsym(lib, name);
		}
#	endif

		LogLevel NormalizeManagedLogLevel(const std::int32_t level)
		{
			switch (static_cast<LogLevel>(level))
			{
				case LogLevel::Verbose:
					return LogLevel::Verbose;
				case LogLevel::Warn:
					return LogLevel::Warn;
				case LogLevel::Error:
					return LogLevel::Error;
				case LogLevel::Info:
				default:
					return LogLevel::Info;
			}
		}

		std::string PrefixManagedLogMessage(const std::string_view message)
		{
			std::string prefixed;
			prefixed.reserve(5 + message.size());
			prefixed.append("[C#] ");
			prefixed.append(message);
			return prefixed;
		}

		void NativeLog(std::int32_t level, const char* messageUtf8)
		{
			const std::string_view msg = messageUtf8 != nullptr ? messageUtf8 : "";
			Logger::LogAtSource(NormalizeManagedLogLevel(level), LogCategory::App, PrefixManagedLogMessage(msg), {}, 0);
		}

		void NativeLogAtSource(std::int32_t level, const char* messageUtf8, const char* filePathUtf8, std::int32_t line)
		{
			const std::string_view msg = messageUtf8 != nullptr ? messageUtf8 : "";
			const std::string_view filePath = filePathUtf8 != nullptr ? filePathUtf8 : "";
			Logger::LogAtSource(NormalizeManagedLogLevel(level), LogCategory::App, PrefixManagedLogMessage(msg), filePath, line);
		}

		void ForwardHostfxrError(const char_t* message)
		{
			AE_WARN(LogCategory::App, "hostfxr: {}", NarrowCharT(message));
		}
	} // namespace

	void NativeReportScriptError(const char* messageUtf8)
	{
		const std::string msg = messageUtf8 != nullptr ? messageUtf8 : "";
		if (g_host != nullptr)
		{
			g_host->OnManagedError(msg);
		}
		else
		{
			AE_ERROR(LogCategory::App, "[C# script error] {}", msg);
		}
	}

	DotNetHost::~DotNetHost()
	{
		// CoreCLR is never torn down (it cannot be re-initialized in-process); we
		if (g_host == this)
		{
			g_host = nullptr;
		}
	}

	void DotNetHost::SetErrorHandler(std::function<void(const std::string&)> handler)
	{
		m_errorHandler = std::move(handler);
	}

	void DotNetHost::OnManagedError(const std::string& message)
	{
		if (m_errorHandler)
		{
			m_errorHandler(message);
		}
		else
		{
			AE_ERROR(LogCategory::App, "[C# script error] {}", message);
		}
	}

	DotNetHost* DotNetHost::Active() noexcept
	{
		return g_host;
	}

	bool DotNetHost::Initialize(const std::filesystem::path& managedDir)
	{
		if (m_initialized)
		{
			return m_available;
		}
		m_initialized = true;
		g_host = this;

		// A packaged install ships its own .NET runtime beside the executable, so the
		// editor runs on a machine with no .NET at all. Point hostfxr at it EXPLICITLY
		// rather than through DOTNET_ROOT: that variable is inherited by the `dotnet build`
		// the editor shells out to for a project's scripts, and aiming MSBuild at a
		// runtime-only layout with no SDK in it would break compilation to fix hosting.
		//
		// Absent - a development build, or an install that relies on a system-wide .NET -
		// nullptr keeps the default global resolution.
		const std::filesystem::path bundledRoot = io::PlatformPaths::GetExecutableDir() / "dotnet";
		std::error_code bundledEc;
		const bool hasBundledRuntime = std::filesystem::is_directory(bundledRoot, bundledEc);
		const string_t bundledRootStr = hasBundledRuntime ? ToCharT(bundledRoot) : string_t{};

		get_hostfxr_parameters hostfxrParams{};
		hostfxrParams.size = sizeof(hostfxrParams);
		hostfxrParams.assembly_path = nullptr;
		hostfxrParams.dotnet_root = hasBundledRuntime ? bundledRootStr.c_str() : nullptr;

		char_t hostfxrPath[1024];
		size_t hostfxrPathLen = std::size(hostfxrPath);
		if (const int rc = get_hostfxr_path(hostfxrPath, &hostfxrPathLen, hasBundledRuntime ? &hostfxrParams : nullptr); rc != 0)
		{
			AE_WARN(LogCategory::App, ".NET runtime not found (get_hostfxr_path=0x{:08X}) - C# scripting disabled", static_cast<unsigned>(rc));
			return false;
		}
		if (hasBundledRuntime)
		{
			AE_INFO(LogCategory::App, ".NET runtime: bundled ({})", bundledRoot.string());
		}

		void* hostfxrLib = LoadHostLibrary(hostfxrPath);
		if (hostfxrLib == nullptr)
		{
			AE_WARN(LogCategory::App, "Failed to load hostfxr - C# scripting disabled");
			return false;
		}

		auto initForConfig = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetHostExport(hostfxrLib, "hostfxr_initialize_for_runtime_config"));
		auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetHostExport(hostfxrLib, "hostfxr_get_runtime_delegate"));
		auto closeCtx = reinterpret_cast<hostfxr_close_fn>(GetHostExport(hostfxrLib, "hostfxr_close"));
		auto setErrorWriter = reinterpret_cast<hostfxr_set_error_writer_fn>(GetHostExport(hostfxrLib, "hostfxr_set_error_writer"));

		if (initForConfig == nullptr || getDelegate == nullptr || closeCtx == nullptr)
		{
			AE_WARN(LogCategory::App, "hostfxr is missing required exports - C# scripting disabled");
			return false;
		}

		if (setErrorWriter != nullptr)
		{
			setErrorWriter(&ForwardHostfxrError);
		}

		const string_t configPath = ToCharT(managedDir / "AetherCore.Interop.runtimeconfig.json");
		hostfxr_handle ctx = nullptr;
		const int initRc = initForConfig(configPath.c_str(), nullptr, &ctx);
		if (initRc < 0 || ctx == nullptr)
		{
			AE_WARN(LogCategory::App, "hostfxr_initialize_for_runtime_config failed (0x{:08X}) for '{}' - C# scripting disabled", static_cast<unsigned>(initRc), (managedDir / "AetherCore.Interop.runtimeconfig.json").string());
			if (ctx != nullptr)
			{
				closeCtx(ctx);
			}
			return false;
		}

		void* loadAssemblyPtr = nullptr;
		const int delRc = getDelegate(ctx, hdt_load_assembly_and_get_function_pointer, &loadAssemblyPtr);
		closeCtx(ctx);
		if (delRc != 0 || loadAssemblyPtr == nullptr)
		{
			AE_WARN(LogCategory::App, "hostfxr_get_runtime_delegate failed (0x{:08X}) - C# scripting disabled", static_cast<unsigned>(delRc));
			return false;
		}
		auto loadAssembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadAssemblyPtr);

		const string_t assemblyPath = ToCharT(managedDir / "AetherCore.Interop.dll");
		ManagedBootstrapFn bootstrapInit = nullptr;
		const int fnRc = loadAssembly(assemblyPath.c_str(),
#	ifdef _WIN32
		        L"AetherCore.Interop.Bootstrap, AetherCore.Interop",
		        L"Init",
#	else
		        "AetherCore.Interop.Bootstrap, AetherCore.Interop",
		        "Init",
#	endif
		        UNMANAGEDCALLERSONLY_METHOD,
		        nullptr,
		        reinterpret_cast<void**>(&bootstrapInit));
		if (fnRc != 0 || bootstrapInit == nullptr)
		{
			AE_WARN(LogCategory::App, "Failed to bind AetherCore.Interop Bootstrap.Init (0x{:08X}) - C# scripting disabled", static_cast<unsigned>(fnRc));
			return false;
		}

		// ── 5. Exchange the ABI tables ───────────────────────────────────────
		NativeHostCallbacks callbacks{};
		callbacks.Log = &NativeLog;
		callbacks.LogAtSource = &NativeLogAtSource;
		callbacks.ReportScriptError = &NativeReportScriptError;

		const int bootRc = bootstrapInit(&callbacks, static_cast<std::int32_t>(sizeof(callbacks)), &m_api, static_cast<std::int32_t>(sizeof(m_api)));
		if (bootRc != 0)
		{
			AE_WARN(LogCategory::App, "AetherCore.Interop rejected the interop ABI (code {}) - C# scripting disabled", bootRc);
			m_api = {};
			return false;
		}

		m_available = true;
		AE_INFO(LogCategory::App, ".NET runtime initialized - C# scripting online");
		return true;
	}
} // namespace aether::scripting

#else

namespace aether::scripting
{
	DotNetHost::~DotNetHost() = default;

	void DotNetHost::SetErrorHandler(std::function<void(const std::string&)> handler)
	{
		m_errorHandler = std::move(handler);
	}

	void DotNetHost::OnManagedError(const std::string&)
	{
	}

	bool DotNetHost::Initialize(const std::filesystem::path&)
	{
		AE_WARN(LogCategory::App, "Engine built without .NET support - C# scripting disabled");
		m_initialized = true;
		m_available = false;
		return false;
	}
} // namespace aether::scripting

#endif
