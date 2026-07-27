#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "scene/Components.hpp"
#include "scripting/DotNetHost.hpp"

namespace aether::app::scripting
{
	struct ScriptPropertyInfo
	{
		std::string name;
		aether::ScriptPropertyValue::Type type = aether::ScriptPropertyValue::Type::None;
	};

	class CSharpScriptingSubsystem
	{
	public:
		CSharpScriptingSubsystem();

		[[nodiscard]] bool IsAvailable() const
		{
			return m_host.IsAvailable();
		}

		[[nodiscard]] const aether::scripting::ManagedScriptApi* Api() const;

		int LoadScripts();

		// Bumped by every successful or failed LoadScripts(). A consumer that caches
		// anything derived from the loaded script types (property tables, RPC method
		// indices) stores the generation alongside its cache and drops it when this
		// changes; there is no callback to subscribe to, and polling a counter cannot
		// be forgotten at a new reload call site the way an explicit invalidation can.
		[[nodiscard]] std::uint32_t ScriptReloadGeneration() const
		{
			return m_reloadGeneration;
		}

		bool RebuildFromSource(std::string& error);

		// on project open; GameRuntime never does, so it only loads the deployed
		void SetScriptProject(std::filesystem::path scriptsProject, std::filesystem::path artifactsDir);

		enum class BuildStatus
		{
			Idle,
			Running, // build executing on a worker thread
			Succeeded,
			Failed,
		};

		// Kick off a source rebuild on a background thread so the caller (the main
		void BeginRebuildFromSource();

		// Main-thread poll of the async build. Returns Idle/Running/Succeeded/Failed;
		[[nodiscard]] BuildStatus PollRebuildStatus(std::string& error) const;

		// Drop a consumed terminal build result (back to Idle). Never blocks.
		void ClearRebuild();

		// its worker thread; drives the editor's "compiling scripts" UI.
		[[nodiscard]] bool IsBuilding() const;

		[[nodiscard]] const std::vector<std::string>& GetScriptTypeNames() const
		{
			return m_typeNames;
		}

		[[nodiscard]] std::vector<ScriptPropertyInfo> GetScriptProperties(const std::string& typeName) const;
		[[nodiscard]] int FindPropertyIndex(const std::string& typeName, const std::string& name) const;
		// Property indices marked [Replicated] on `typeName`, in property-table order.
		// Indices address the same table GetScriptProperties/GetPropertyValue use, so
		// replication reuses the existing bridge instead of a second value path.
		[[nodiscard]] std::vector<int> GetReplicatedPropertyIndices(const std::string& typeName) const;
		// Index of `methodName` in `typeName`'s [NetRpc] method table, or -1 if the
		// type is unknown or declares no such RPC. `outTarget` receives the
		// NetRpcTarget the method's attribute declared (untouched when the lookup
		// fails) - the declaration is the single source of truth for direction, so
		// the caller reads it here rather than being told at the call site.
		[[nodiscard]] int FindNetRpcMethod(const std::string& typeName, const std::string& methodName,
		        int& outTarget) const;
		// Invokes RPC method `methodIndex` on the live instance `handle` names. A
		// no-op if the api is unavailable or the handle is 0.
		void InvokeNetRpc(std::uint64_t handle, int methodIndex, std::span<const std::byte> args) const;
		[[nodiscard]] bool GetPropertyValue(std::uint64_t handle, int index, aether::ScriptPropertyValue& out) const;
		void SetPropertyValue(std::uint64_t handle, int index, const aether::ScriptPropertyValue& value) const;
		[[nodiscard]] bool GetDefaultPropertyValue(const std::string& typeName, int index, aether::ScriptPropertyValue& out) const;
		void ApplyProperties(std::uint64_t handle, const std::string& typeName, const std::map<std::string, aether::ScriptPropertyValue>& props) const;

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
		std::uint32_t m_reloadGeneration = 0;

		std::filesystem::path m_scriptProject;
		std::filesystem::path m_scriptArtifactsDir;

		// Background build job (detached worker + atomic completion flags), shared
		std::shared_ptr<struct ScriptBuildJob> m_buildJob;

		bool m_reloadRequested = false;
		bool m_reloadInProgress = false;
		std::vector<std::string> m_pendingErrors;
		bool m_errorsCleared = false;
	};
} // namespace aether::app::scripting
