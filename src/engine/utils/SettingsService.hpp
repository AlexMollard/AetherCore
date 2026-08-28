#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "utils/EngineSettings.hpp"

namespace aether
{
	class ServiceContainer;

	class SettingsService
	{
	public:
		// 1+2+3 (defaults -> shipped -> project) for delta saves. 'services' must
		SettingsService(const EngineSettings& values, const EngineSettings& base, const EngineSettings& shipped, ServiceContainer& services);

		[[nodiscard]] const EngineSettings& Get() const
		{
			return m_values;
		}

		// Mutable access for the settings editor. Callers must follow a write with
		[[nodiscard]] EngineSettings& Values()
		{
			return m_values;
		}

		[[nodiscard]] EngineSettings& Shipped() noexcept
		{
			return m_shipped;
		}

		// Where authored settings are written. Empty until a project is open, in which case
		// Save() writes only the per-user file - there is nowhere else to put them.
		void SetProjectFile(std::filesystem::path projectFile)
		{
			m_projectFile = std::move(projectFile);
		}

		[[nodiscard]] const std::filesystem::path& ProjectFile() const noexcept
		{
			return m_projectFile;
		}

		// Populated by Save() when the project file could not be written, so the editor can
		// say so instead of silently losing the edit.
		[[nodiscard]] const std::string& LastSaveError() const noexcept
		{
			return m_lastSaveError;
		}

		[[nodiscard]] EngineSettings& Base() noexcept
		{
			return m_base;
		}

		[[nodiscard]] const EngineSettings& Base() const
		{
			return m_base;
		}

		void ApplyField(std::string_view key);

		void ApplyAll();

		void MarkDirty()
		{
			m_dirty = true;
		}

		[[nodiscard]] bool IsDirty() const
		{
			return m_dirty;
		}

		void Save();

	private:
		void ApplyLive(std::string_view key);

		EngineSettings m_values;
		EngineSettings m_base;
		EngineSettings m_shipped;
		std::filesystem::path m_projectFile;
		std::string m_lastSaveError;
		ServiceContainer& m_services;
		bool m_dirty = false;
	};
} // namespace aether
