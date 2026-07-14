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
