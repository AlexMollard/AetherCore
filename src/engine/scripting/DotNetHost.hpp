#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "scripting/ManagedInterop.hpp"

namespace aether::scripting
{
	// CoreCLR can only be initialized once per process and never shut down, so
	class DotNetHost
	{
	public:
		DotNetHost() = default;
		~DotNetHost();

		DotNetHost(const DotNetHost&) = delete;
		DotNetHost& operator=(const DotNetHost&) = delete;
		DotNetHost(DotNetHost&&) = delete;
		DotNetHost& operator=(DotNetHost&&) = delete;

		bool Initialize(const std::filesystem::path& managedDir);

		[[nodiscard]] bool IsAvailable() const noexcept
		{
			return m_available;
		}

		// The host that is currently initialised, or null.
		//
		// CoreCLR is a process singleton - there is exactly one runtime, and exactly one
		// host bound to it - so this is a fact rather than a convenience. It exists for
		// code that legitimately needs the managed API but sits outside the service
		// container that owns the subsystem, such as the publish steps.
		[[nodiscard]] static DotNetHost* Active() noexcept;

		[[nodiscard]] const ManagedScriptApi& Api() const noexcept
		{
			return m_api;
		}

		void SetErrorHandler(std::function<void(const std::string&)> handler);

	private:
		bool m_available = false;
		bool m_initialized = false;
		ManagedScriptApi m_api{};
		std::function<void(const std::string&)> m_errorHandler;

		void OnManagedError(const std::string& message);

		friend void NativeReportScriptError(const char* messageUtf8);
	};
} // namespace aether::scripting
