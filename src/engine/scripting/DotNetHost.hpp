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
