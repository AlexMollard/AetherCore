#pragma once

#include <string_view>

#include "utils/EngineSettings.hpp"

namespace aether
{
	class ServiceContainer;

	// Single source of truth for runtime settings. Owns the merged EngineSettings
	// (the master everyone reads and the editor writes) plus the base layer used to
	// persist only the user delta.
	//
	// Editing flow: a caller writes fields through Values(), then calls
	// ApplyField(key) for each changed key. ApplyField pushes that key to its owning
	// subsystem if it has a live effect (FXAA, VSync, resolution, target FPS) and
	// marks the settings dirty so they persist on Save(). Settings with no live hook
	// simply persist and take effect on next launch.
	class SettingsService
	{
	public:
		// 'values' is the merged result of the settings cascade; 'base' is layers
		// 1+2+3 (defaults -> shipped -> project) for delta saves. 'services' must
		// outlive this object and is used to reach the subsystems live changes are
		// applied to.
		SettingsService(const EngineSettings& values, const EngineSettings& base, ServiceContainer& services);

		[[nodiscard]] const EngineSettings& Get() const
		{
			return m_values;
		}

		// Mutable access for the settings editor. Callers must follow a write with
		// ApplyField(key) so the change reaches its subsystem and is marked for save.
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

		// Applies a single changed key to its owning subsystem (if live) and marks
		// the settings dirty.
		void ApplyField(std::string_view key);

		// Pushes every live-applicable setting to its subsystem. Call once after the
		// subsystems exist (startup) so file values that aren't consumed at init
		// time (FXAA, target FPS) actually take effect. Idempotent.
		void ApplyAll();

		void MarkDirty()
		{
			m_dirty = true;
		}

		[[nodiscard]] bool IsDirty() const
		{
			return m_dirty;
		}

		// Writes the user delta (values vs base) to the per-user file and clears the
		// dirty flag.
		void Save();

	private:
		// Pushes one key to its owning subsystem without touching the dirty flag.
		// Idempotent, so ApplyAll can call it for every key safely.
		void ApplyLive(std::string_view key);

		EngineSettings m_values;
		EngineSettings m_base;
		ServiceContainer& m_services;
		bool m_dirty = false;
	};
} // namespace aether
