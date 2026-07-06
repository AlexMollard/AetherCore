#include "scripting/DotNetHost.hpp"

#include "utils/Logger.hpp"

#if AETHER_HAS_DOTNET

#	include <string_view>

#	include <nethost.h>
#	include <coreclr_delegates.h>
#	include <hostfxr.h>

#	ifdef _WIN32
#		include <windows.h>
#	else
#		include <dlfcn.h>
#	endif

namespace aether::scripting
{
	namespace
	{
		// The one live host, so the static C callbacks handed to managed code can
		// route back into it. CoreCLR is a process singleton, so is this.
		DotNetHost* g_host = nullptr;

#	ifdef _WIN32
		using string_t = std::wstring;

		string_t ToCharT(const std::filesystem::path& p)
		{
			return p.wstring();
		}

		// hostfxr speaks UTF-16 on Windows; narrow to UTF-8 for the engine log.
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

		// ── Native callbacks exposed to managed code ─────────────────────────────
		void NativeLog(std::int32_t level, const char* messageUtf8)
		{
			const std::string_view msg = messageUtf8 != nullptr ? messageUtf8 : "";
			const auto lvl = static_cast<LogLevel>(level);
			switch (lvl)
			{
				case LogLevel::Verbose:
					AE_INFO(LogCategory::App, "[C#] {}", msg);
					break;
				case LogLevel::Warn:
					AE_WARN(LogCategory::App, "[C#] {}", msg);
					break;
				case LogLevel::Error:
					AE_ERROR(LogCategory::App, "[C#] {}", msg);
					break;
				case LogLevel::Info:
				default:
					AE_INFO(LogCategory::App, "[C#] {}", msg);
					break;
			}
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
		// just detach the global so late callbacks are inert.
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

	bool DotNetHost::Initialize(const std::filesystem::path& managedDir)
	{
		if (m_initialized)
		{
			return m_available;
		}
		m_initialized = true;
		g_host = this;

		// ── 1. Locate hostfxr via nethost ────────────────────────────────────
		char_t hostfxrPath[1024];
		size_t hostfxrPathLen = std::size(hostfxrPath);
		if (const int rc = get_hostfxr_path(hostfxrPath, &hostfxrPathLen, nullptr); rc != 0)
		{
			AE_WARN(LogCategory::App, ".NET runtime not found (get_hostfxr_path=0x{:08X}) - C# scripting disabled", static_cast<unsigned>(rc));
			return false;
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

		// ── 2. Initialize the runtime from AetherCore.Interop.runtimeconfig.json ─
		const string_t configPath = ToCharT(managedDir / "AetherCore.Interop.runtimeconfig.json");
		hostfxr_handle ctx = nullptr;
		const int initRc = initForConfig(configPath.c_str(), nullptr, &ctx);
		// Negative == failure (HRESULT-style); non-negative success codes include
		// Success(0), Success_HostAlreadyInitialized(1), Success_DifferentRuntimeProperties(2).
		if (initRc < 0 || ctx == nullptr)
		{
			AE_WARN(LogCategory::App, "hostfxr_initialize_for_runtime_config failed (0x{:08X}) for '{}' - C# scripting disabled", static_cast<unsigned>(initRc), (managedDir / "AetherCore.Interop.runtimeconfig.json").string());
			if (ctx != nullptr)
			{
				closeCtx(ctx);
			}
			return false;
		}

		// ── 3. Get the load-assembly delegate, then close the init context ───
		void* loadAssemblyPtr = nullptr;
		const int delRc = getDelegate(ctx, hdt_load_assembly_and_get_function_pointer, &loadAssemblyPtr);
		closeCtx(ctx);
		if (delRc != 0 || loadAssemblyPtr == nullptr)
		{
			AE_WARN(LogCategory::App, "hostfxr_get_runtime_delegate failed (0x{:08X}) - C# scripting disabled", static_cast<unsigned>(delRc));
			return false;
		}
		auto loadAssembly = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadAssemblyPtr);

		// ── 4. Resolve the managed Bootstrap.Init entry point ────────────────
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

#else // !AETHER_HAS_DOTNET

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

#endif // AETHER_HAS_DOTNET
