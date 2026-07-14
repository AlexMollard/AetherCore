#pragma once

#include <string_view>

#include "utils/EngineSettings.hpp"

namespace aether
{
	class ServiceContainer;

	class SettingsService
	{
	public:
		// 1+2+3 (defaults -> shipped -> project) for delta saves. 'services' must
		SettingsService(const EngineSettings& values, const EngineSettings& base, ServiceContainer& services);

		[[nodiscard]] const EngineSettings& Get() const
		{
			return m_values;
		}

		// Mutable access for the settings editor. Callers must follow a write with
		[[nodiscard]] EngineSettings& Values()
		{
			return m_values;
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
		ServiceContainer& m_services;
		bool m_dirty = false;
	};
} // namespace aether
