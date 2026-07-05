#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scripting/DotNetHost.hpp"

namespace aether::app::scripting
{
	// Owns the .NET host and the loaded game-scripts assembly, and drives the C#
	// scripting lifecycle for ScriptComponentSystem. Mirrors the daScript
	// ScriptingSubsystem's reload/error surface so DebugLayer / DevToolsPanel can
	// treat either runtime the same way.
	//
	// Constructed once per application. On construction it locates the deployed
	// managed assemblies, boots CoreCLR, and loads AetherScripts.dll. Everything
	// degrades safely: without .NET, IsAvailable() is false and the runner falls
	// back to daScript.
	class CSharpScriptingSubsystem
	{
	public:
		CSharpScriptingSubsystem();

		[[nodiscard]] bool IsAvailable() const
		{
			return m_host.IsAvailable();
		}

		// The managed API table, or nullptr when scripting is unavailable.
		[[nodiscard]] const aether::scripting::ManagedScriptApi* Api() const;

		// (Re)load the game-scripts assembly and refresh the type-name cache.
		// Returns the number of discovered script types, or -1 on failure.
		int LoadScripts();

		// Concrete EntityScript type names discovered in the loaded assembly.
		[[nodiscard]] const std::vector<std::string>& GetScriptTypeNames() const
		{
			return m_typeNames;
		}

		// ── Reload / error surface (mirrors ScriptingSubsystem) ────────────────
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

		void ReportScriptError(const std::string& error);
		[[nodiscard]] std::vector<std::string> PollPendingErrors();
		void ClearErrors();
		[[nodiscard]] bool ConsumeErrorsCleared();

	private:
		void RefreshTypeNames();

		aether::scripting::DotNetHost m_host;
		std::filesystem::path m_managedDir;
		std::string m_scriptsAssemblyPath;
		std::vector<std::string> m_typeNames;

		bool m_reloadRequested = false;
		bool m_reloadInProgress = false;
		std::vector<std::string> m_pendingErrors;
		bool m_errorsCleared = false;
	};
} // namespace aether::app::scripting
