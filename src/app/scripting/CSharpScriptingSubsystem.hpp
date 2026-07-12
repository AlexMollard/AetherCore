#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "scene/Components.hpp"
#include "scripting/DotNetHost.hpp"

namespace aether::app::scripting
{
	// Metadata for one inspector-exposed script property.
	struct ScriptPropertyInfo
	{
		std::string name;
		aether::ScriptPropertyValue::Type type = aether::ScriptPropertyValue::Type::None;
	};

	// Owns the .NET host and the loaded game-scripts assembly, and drives the C#
	// scripting lifecycle for ScriptComponentSystem. Exposes a reload/error
	// surface consumed by DebugLayer / DevToolsPanel (F5 hot-reload, error toasts).
	//
	// Constructed once per application. On construction it locates the deployed
	// managed assemblies, boots CoreCLR, and loads AetherGame.dll. Everything
	// degrades safely: without .NET, IsAvailable() is false and entity scripts
	// simply do not run.
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

		// Dev-only: rebuild the game scripts (AetherGame) from source with
		// `dotnet build` and redeploy the assembly into the managed load dir, so
		// F5 picks up source edits without a separate build step. Returns true - a
		// no-op - in a packaged build where the source/SDK paths were not baked in.
		// On a build failure returns false and fills `error` with the build output.
		bool RebuildFromSource(std::string& error);

		// ── Async source rebuild (non-blocking Play) ──────────────────────────
		// Status of the background build kicked off by BeginRebuildFromSource.
		enum class BuildStatus
		{
			Idle,      // no build in flight
			Running,   // build executing on a worker thread
			Succeeded, // finished OK; assembly redeployed
			Failed,    // build failed; PollRebuildStatus fills the error
		};

		// Kick off a source rebuild on a background thread so the caller (the main
		// thread) never blocks on `dotnet build`; the editor stays fully responsive.
		// The worker does only file IO (build + redeploy the dll) - no ECS or
		// managed-runtime access. A no-op build (scripts already current) completes
		// immediately; if a build is already in flight, this adopts it.
		void BeginRebuildFromSource();

		// Main-thread poll of the async build. Returns Idle/Running/Succeeded/Failed;
		// on Failed, `error` holds the captured build output.
		[[nodiscard]] BuildStatus PollRebuildStatus(std::string& error) const;

		// Drop a consumed terminal build result (back to Idle). Never blocks.
		void ClearRebuild();

		// Concrete EntityScript type names discovered in the loaded assembly.
		[[nodiscard]] const std::vector<std::string>& GetScriptTypeNames() const
		{
			return m_typeNames;
		}

		// ── Script properties (inspector + scene overrides) ────────────────────
		// The inspector-exposed fields of a script type.
		[[nodiscard]] std::vector<ScriptPropertyInfo> GetScriptProperties(const std::string& typeName) const;
		// Index of a named property within a type, or -1.
		[[nodiscard]] int FindPropertyIndex(const std::string& typeName, const std::string& name) const;
		// Read/write a property value on a live script instance (GCHandle).
		[[nodiscard]] bool GetPropertyValue(std::uint64_t handle, int index, aether::ScriptPropertyValue& out) const;
		void SetPropertyValue(std::uint64_t handle, int index, const aether::ScriptPropertyValue& value) const;
		// Read a type's default field value (edit mode, no live instance).
		[[nodiscard]] bool GetDefaultPropertyValue(const std::string& typeName, int index, aether::ScriptPropertyValue& out) const;
		// Apply stored per-entity overrides to a freshly created instance.
		void ApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, aether::ScriptPropertyValue>& props) const;

		// ── Reload / error surface (F5 hot-reload + error toasts) ──────────────
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

		// Background build job (detached worker + atomic completion flags), shared
		// with the worker so it stays alive even if we stop tracking it. Never joined,
		// so polling it never blocks the main thread.
		std::shared_ptr<struct ScriptBuildJob> m_buildJob;

		bool m_reloadRequested = false;
		bool m_reloadInProgress = false;
		std::vector<std::string> m_pendingErrors;
		bool m_errorsCleared = false;
	};
} // namespace aether::app::scripting
