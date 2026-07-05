#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace das
{
	class Context;
	class SimFunction;
} // namespace das

namespace aether::app::scripting
{
	struct SceneContext;
	struct ScriptHandle;

	// Owns the daScript runtime context and compiled scripts.
	// Constructed once per application; ScriptedSceneLayers use it to compile
	// and invoke .das files.
	class ScriptingSubsystem
	{
	public:
		ScriptingSubsystem();
		~ScriptingSubsystem();

		// Compile a .das script file.  Returns a handle with IsValid()=false on
		// failure; the error is stored in GetLastError() and logged.
		[[nodiscard]] ScriptHandle Compile(const std::string& path);

		// Invoke on_attach(). Sets the TLS active context before the call.
		void CallOnAttach(ScriptHandle& handle, SceneContext& ctx);

		// Invoke on_update(). Sets the TLS active context before the call.
		void CallOnUpdate(ScriptHandle& handle, SceneContext& ctx);

		// Invoke on_detach(). Sets the TLS active context before the call.
		void CallOnDetach(ScriptHandle& handle, SceneContext& ctx);

		// Entity-script entry points (ScriptComponent runner):
		//   def on_entity_attach(world : World?; self : uint)
		//   def on_entity_update(world : World?; self : uint; dt : float)
		// No-ops (returning false) when the script does not export them.
		bool CallEntityAttach(ScriptHandle& handle, SceneContext& ctx, std::uint32_t self);
		bool CallEntityUpdate(ScriptHandle& handle, SceneContext& ctx, std::uint32_t self, float dt);

		// Signal that the active script should be reloaded on the next
		// CallOnUpdate.  Called by DebugLayer when F5 is pressed.
		void RequestReload()
		{
			m_reloadRequested = true;
		}

		[[nodiscard]] bool HasReloadRequest() const
		{
			return m_reloadRequested;
		}

		void ClearReloadRequest()
		{
			m_reloadRequested = false;
		}

		[[nodiscard]] bool IsReloadInProgress() const
		{
			return m_reloadInProgress;
		}

		void SetReloadInProgress(bool inProgress)
		{
			m_reloadInProgress = inProgress;
		}

		// Delete the das::Context inside a handle and reset it to invalid.
		// Must be called instead of `delete handle.ctx` because das::Context is
		// only forward-declared in ScriptHandle.hpp.
		static void FreeHandle(ScriptHandle& handle);

		[[nodiscard]] const std::string& GetLastError() const
		{
			return m_lastError;
		}

		[[nodiscard]] bool HasError() const
		{
			return !m_lastError.empty();
		}

		void ClearError()
		{
			m_lastError.clear();
		}

		// Record a script error and queue it for UI toast display.
		void ReportScriptError(const std::string& error);

		// Drain all pending errors for toast display.  Returns and clears the
		// internal queue so each error is consumed exactly once.
		[[nodiscard]] std::vector<std::string> PollPendingErrors();

		// Clear all errors and signal that any displayed error toasts should
		// be dismissed (called after a successful script reload).
		void ClearErrors();

		// Check and consume the "errors cleared" flag.  Returns true once
		// after ClearErrors() was called, then resets the flag.
		[[nodiscard]] bool ConsumeErrorsCleared();

	private:
		bool m_modulesRegistered = false;
		bool m_reloadRequested = false;
		bool m_reloadInProgress = false;
		std::string m_lastError;
		std::vector<std::string> m_pendingErrors;
		bool m_errorsCleared = false;
	};
} // namespace aether::app::scripting
