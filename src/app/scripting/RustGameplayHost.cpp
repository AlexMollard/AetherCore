#include "scripting/RustGameplayHost.hpp"

#include <system_error>

#if defined(_WIN32)
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#endif

#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/RustGameplayServices.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scripting
{
	struct RustGameplayHost::Session
	{
#if defined(_WIN32)
		HMODULE library = nullptr;
#else
		void* library = nullptr;
#endif
		AetherGameplayModuleApi module{};
		std::unique_ptr<aether::scripting::GameplayAccess> access;
		std::unique_ptr<RustGameplayServices> services;
		bool initialized = false;

		~Session()
		{
			if (library != nullptr)
			{
#if defined(_WIN32)
				FreeLibrary(library);
#else
				dlclose(library);
#endif
			}
		}

		void* Symbol(const char* name) const
		{
#if defined(_WIN32)
			return reinterpret_cast<void*>(GetProcAddress(library, name));
#else
			return dlsym(library, name);
#endif
		}
	};

	RustGameplayHost::RustGameplayHost()
	{
		m_gameplay.SetErrorHandler(
		        [this](const std::string& message)
		        {
			        m_diagnostics.push_back({.code = "RUST_CALLBACK", .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = message});
			        AE_ERROR(LogCategory::App, "Rust gameplay: {}", message);
		        });
	}

	RustGameplayHost::~RustGameplayHost()
	{
		Unload();
	}

	AetherGameplayStatus RustGameplayHost::Diagnostic(void* user, const AetherGameplayDiagnostic& diagnostic)
	{
		auto converted = aether::scripting::FromGameplayDiagnostic(diagnostic);
		if (!converted)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		auto& host = *static_cast<RustGameplayHost*>(user);
		AE_INFO(LogCategory::App, "Rust gameplay [{}]: {}", converted->code, converted->message);
		host.m_diagnostics.push_back(std::move(*converted));
		return AE_GAMEPLAY_OK;
	}

	Expected<void> RustGameplayHost::Failure(std::string code, std::string message)
	{
		m_diagnostics.push_back({.code = std::move(code), .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = message});
		AE_ERROR(LogCategory::App, "Rust gameplay: {}", message);
		Unload();
		return std::unexpected(AetherError::Engine(std::move(message)));
	}

	Expected<void> RustGameplayHost::Load(const std::filesystem::path& path, World& world, SceneContext* context)
	{
		Unload();
		m_diagnostics.clear();
		if (context != nullptr && context->world != &world)
		{
			return Failure("RUST_CONTEXT_WORLD", "Rust gameplay scene context belongs to a different world");
		}
		std::error_code error;
		const auto absolute = std::filesystem::canonical(path, error);
		if (error || !std::filesystem::is_regular_file(absolute, error))
		{
			return Failure("RUST_LIBRARY_MISSING", "Rust gameplay library does not exist: " + path.string());
		}
		m_session = std::make_unique<Session>();
#if defined(_WIN32)
		m_session->library = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
		m_session->library = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		if (m_session->library == nullptr)
		{
			return Failure("RUST_LIBRARY_LOAD", "Could not load Rust gameplay library: " + absolute.string());
		}
		const auto query = reinterpret_cast<AetherGameplayQuery>(m_session->Symbol("aether_gameplay_query"));
		if (query == nullptr)
		{
			return Failure("RUST_QUERY_MISSING", "Rust gameplay library has no aether_gameplay_query export");
		}
		auto& session = *m_session;
		const auto queried = aether::scripting::QueryGameplayModule(query, AE_GAMEPLAY_ABI_MAJOR, sizeof(session.module), &session.module);
		if (queried != AE_GAMEPLAY_OK)
		{
			return Failure("RUST_ABI_MISMATCH", "Rust gameplay ABI query failed (status " + std::to_string(queried) + ")");
		}
		session.access = std::make_unique<aether::scripting::GameplayAccess>(world, Diagnostic, this);
		m_gameplay.SetAccess(session.access.get());
		const auto bind = reinterpret_cast<AetherGameplayBindServices>(session.Symbol("aether_gameplay_bind_services"));
		if (bind != nullptr && context != nullptr)
		{
			session.services = std::make_unique<RustGameplayServices>(*context, *session.access);
			const auto api = session.services->EngineApi();
			AetherGameplayStatus status = AE_GAMEPLAY_SCRIPT_ERROR;
			try
			{
				aether::scripting::GameplayAccess::Scope scope(*session.access, aether::scripting::GameplayAccess::ScopeKind::Callback);
				status = scope.Status() == AE_GAMEPLAY_OK ? bind(AE_GAMEPLAY_SERVICES_ABI_MAJOR, sizeof(api), &api) : scope.Status();
			}
			catch (...)
			{
			}
			if (status != AE_GAMEPLAY_OK)
			{
				return Failure("RUST_SERVICES_BIND", "Rust gameplay services binding failed (status " + std::to_string(status) + ")");
			}
		}
		AetherGameplayStatus initialized = AE_GAMEPLAY_SCRIPT_ERROR;
		{
			aether::scripting::GameplayAccess::Scope scope(*session.access, aether::scripting::GameplayAccess::ScopeKind::Callback);
			const auto api = session.access->EngineApi();
			try
			{
				initialized = scope.Status() == AE_GAMEPLAY_OK ? session.module.initialize(session.module.context, &api) : scope.Status();
			}
			catch (...)
			{
			}
		}
		// Even partial initialization is shut down before the library is released.
		session.initialized = true;
		if (initialized != AE_GAMEPLAY_OK)
		{
			return Failure("RUST_INITIALIZE", "Rust gameplay initialization failed (status " + std::to_string(initialized) + ")");
		}
		auto reset = m_gameplay.Reset(session.module, "rust", ++m_generation);
		if (!reset)
		{
			return Failure("RUST_METADATA", reset.error().message);
		}
		return {};
	}

	void RustGameplayHost::Unload()
	{
		m_gameplay.Clear(++m_generation);
		if (m_session != nullptr && m_session->initialized)
		{
			aether::scripting::GameplayAccess::Scope scope(*m_session->access, aether::scripting::GameplayAccess::ScopeKind::Callback);
			try
			{
				(void) m_session->module.shutdown(m_session->module.context);
			}
			catch (...)
			{
			}
		}
		m_gameplay.SetAccess(nullptr);
		if (m_session != nullptr)
		{
			m_session->services.reset();
			m_session->access.reset();
		}
		m_session.reset();
	}
} // namespace aether::app::scripting
