#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "scripting/ManagedInterop.hpp"

namespace aether::scripting
{
	// Hosts the .NET (CoreCLR) runtime in-process via nethost + hostfxr.
	//
	// CoreCLR can only be initialized once per process and never shut down, so
	// exactly one DotNetHost should exist for the lifetime of the app. Initialize()
	// is the single entry point: on success IsAvailable() is true and Api() returns
	// the managed function-pointer table. On any failure (missing runtime, missing
	// assemblies, ABI mismatch) it logs a warning and leaves the host unavailable —
	// it never throws, so a build without .NET, or a broken managed deploy, degrades
	// to "scripting disabled" instead of crashing.
	//
	// When the engine is compiled without AETHER_HAS_DOTNET the whole implementation
	// is stubbed: Initialize() returns false and IsAvailable() is always false.
	class DotNetHost
	{
	public:
		DotNetHost() = default;
		~DotNetHost();

		DotNetHost(const DotNetHost&) = delete;
		DotNetHost& operator=(const DotNetHost&) = delete;
		DotNetHost(DotNetHost&&) = delete;
		DotNetHost& operator=(DotNetHost&&) = delete;

		// Boots CoreCLR from the managed assemblies deployed in `managedDir`
		// (expects AetherCore.Managed.dll + .runtimeconfig.json there). Idempotent:
		// a second call is a no-op that returns the current availability.
		bool Initialize(const std::filesystem::path& managedDir);

		[[nodiscard]] bool IsAvailable() const noexcept { return m_available; }

		// The managed API table. Only meaningful when IsAvailable() is true.
		[[nodiscard]] const ManagedScriptApi& Api() const noexcept { return m_api; }

		// Routes managed ReportScriptError() calls to the app's error UI. When
		// unset, script errors fall back to the engine log.
		void SetErrorHandler(std::function<void(const std::string&)> handler);

	private:
		bool m_available = false;
		bool m_initialized = false;
		ManagedScriptApi m_api{};
		std::function<void(const std::string&)> m_errorHandler;

		// Invoked by the static C callbacks handed to managed code.
		void OnManagedError(const std::string& message);

		friend void NativeReportScriptError(const char* messageUtf8);
	};
} // namespace aether::scripting
