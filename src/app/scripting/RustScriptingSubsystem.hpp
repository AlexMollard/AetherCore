#pragma once

#include <memory>
#include <functional>
#include "scripting/RustGameplayHost.hpp"
#include "scripting/RustGameplayBuild.hpp"

namespace aether::app::scripting
{
	class RustScriptingSubsystem
	{
	public:
		enum class BuildStatus
		{
			Idle,
			Running,
			Succeeded,
			Failed
		};
		using BuildFunction = std::function<RustBuildResult(const RustBuildRequest&)>;
		explicit RustScriptingSubsystem(std::filesystem::path sourceProject = {}, BuildFunction build = BuildRustGameplay);
		~RustScriptingSubsystem();
		Expected<void> Initialize(SceneContext& context);

		[[nodiscard]] const std::filesystem::path& SourceProject() const
		{
			return m_projectRoot;
		}

		[[nodiscard]] GameplayHost& Gameplay()
		{
			return m_host.Gameplay();
		}

		[[nodiscard]] const RustGameplayHost& Host() const
		{
			return m_host;
		}

		bool BeginRebuild();
		BuildStatus PollRebuild();
		void ClearRebuild();
		Expected<void> LoadBuiltModule();
		void Unload();

		[[nodiscard]] const aether::scripting::AgentReport& Report() const
		{
			return m_result.report;
		}

	private:
		struct BuildJob;
		Expected<void> StageFailure(std::string message);
		RustGameplayHost m_host;
		RustBuildRequest m_request;
		RustBuildResult m_result;
		std::shared_ptr<BuildJob> m_job;
		std::filesystem::path m_projectRoot;
		SceneContext* m_context = nullptr;
		std::filesystem::path m_loaded;
		BuildFunction m_build;
		bool m_cancelled = false;
	};
} // namespace aether::app::scripting
