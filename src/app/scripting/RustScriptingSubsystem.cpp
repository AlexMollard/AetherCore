#include "scripting/RustScriptingSubsystem.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "Defines.hpp"
#include "RuntimeProjectSettings.hpp"
#include "io/PlatformPaths.hpp"
#include "project/ProjectCommon.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scripting
{
	struct RustScriptingSubsystem::BuildJob
	{
		std::atomic<bool> done{false};
		RustBuildResult result;
	};

	RustScriptingSubsystem::RustScriptingSubsystem(std::filesystem::path sourceProject, BuildFunction build)
	      : m_projectRoot(std::move(sourceProject)), m_build(std::move(build))
	{
		m_result.report = {.jobId = "rust-initialize", .status = aether::scripting::AgentJobStatus::Queued, .phase = "load", .target = "desktop", .language = "rust", .profile = "desktop-rust-v1"};
	}

	RustScriptingSubsystem::~RustScriptingSubsystem()
	{
		Unload();
	}

	Expected<void> RustScriptingSubsystem::Initialize(SceneContext& context)
	{
		m_context = &context;
		const auto root = m_projectRoot.empty() ? std::filesystem::u8path(io::PlatformPaths::ReadEnvironmentVariable("AETHER_PROJECT_DIR")) : m_projectRoot;
		if (!root.empty())
		{
			AE_TRY(descriptor, project::ReadProjectDescriptor(root));
			m_request.manifest = descriptor->scriptsDir / "Cargo.toml";
			m_request.artifactsDir = descriptor->root / "Builds" / "Intermediate" / "rust";
			m_request.outputDir = m_request.artifactsDir / "output";
			m_request.release = AE_DEV_TOOLING == 0;
			m_result = m_build(m_request);
			if (!m_result.Succeeded())
			{
				AE_UNEXPECTED(AetherError::Engine(m_result.report.diagnostics.empty() ? "Rust build failed" : m_result.report.diagnostics.back().message));
			}
		}
		else
		{
			m_result.module = ResolveRuntimeGameplayModule(io::PlatformPaths::GetExecutableDir(), GameplayLanguage::Rust);
			m_result.report = {.jobId = "rust-module-load", .status = aether::scripting::AgentJobStatus::Passed, .phase = "load", .target = "desktop", .language = "rust", .profile = "desktop-rust-v1"};
		}
		return LoadBuiltModule();
	}

	bool RustScriptingSubsystem::BeginRebuild()
	{
		if (m_request.manifest.empty() || (m_job && !m_job->done.load(std::memory_order_acquire)))
		{
			return false;
		}
		m_job = std::make_shared<BuildJob>();
		m_cancelled = false;
		m_result = {};
		m_result.report = {.jobId = "rust-build", .status = aether::scripting::AgentJobStatus::Running, .phase = "build", .target = "desktop", .language = "rust", .profile = "desktop-rust-v1"};
		std::thread(
		        [job = m_job, request = m_request, build = m_build]
		        {
			        try
			        {
				        job->result = build(request);
			        }
			        catch (const std::exception& error)
			        {
				        job->result.report = {.jobId = "rust-build",
				                .status = aether::scripting::AgentJobStatus::Failed,
				                .phase = "build",
				                .target = "desktop",
				                .language = "rust",
				                .profile = "desktop-rust-v1",
				                .diagnostics = {{.code = "RUST_BUILD_EXCEPTION", .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = error.what()}}};
			        }
			        job->done.store(true, std::memory_order_release);
		        })
		        .detach();
		return true;
	}

	RustScriptingSubsystem::BuildStatus RustScriptingSubsystem::PollRebuild()
	{
		if (m_cancelled)
		{
			return BuildStatus::Failed;
		}
		if (!m_job)
		{
			return BuildStatus::Idle;
		}
		if (!m_job->done.load(std::memory_order_acquire))
		{
			return BuildStatus::Running;
		}
		m_result = m_job->result;
		return m_result.Succeeded() ? BuildStatus::Succeeded : BuildStatus::Failed;
	}

	void RustScriptingSubsystem::ClearRebuild()
	{
		if (m_job && m_result.report.status == aether::scripting::AgentJobStatus::Running)
		{
			// The bounded worker owns only its request and private result. Cancel the
			// requested Play immediately; its eventual artifact must never be loaded.
			m_cancelled = true;
			m_result.module.clear();
			m_result.report.status = aether::scripting::AgentJobStatus::Cancelled;
			m_result.report.diagnostics.push_back({.code = "RUST_BUILD_CANCELLED", .severity = aether::scripting::AgentDiagnosticSeverity::Info, .message = "Rust Play compilation was cancelled; its result will not be loaded."});
		}
		if (m_job && m_job->done.load(std::memory_order_acquire))
		{
			m_job.reset();
		}
	}

	Expected<void> RustScriptingSubsystem::StageFailure(std::string message)
	{
		m_result.report.status = aether::scripting::AgentJobStatus::Failed;
		m_result.report.phase = "load";
		m_result.module.clear();
		m_result.report.diagnostics.push_back({.code = "RUST_MODULE_STAGE", .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = message});
		return std::unexpected(AetherError::Engine(std::move(message)));
	}

	Expected<void> RustScriptingSubsystem::LoadBuiltModule()
	{
		if (!m_context || !m_context->world || !m_result.Succeeded())
		{
			AE_UNEXPECTED(AetherError::Engine("Rust has no successfully built gameplay module to load."));
		}
		m_host.Unload();
		if (!m_loaded.empty())
		{
			std::error_code ec;
			std::filesystem::remove(m_loaded, ec);
			std::filesystem::remove(m_loaded.parent_path(), ec);
			m_loaded.clear();
		}
		auto path = m_result.module;
		if (!m_request.manifest.empty())
		{
			// Cargo can rebuild while the previous code remains loaded on Windows.
			const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			path = m_request.artifactsDir / "loaded" / unique / path.filename();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			if (!ec)
			{
				std::filesystem::copy_file(m_result.module, path, std::filesystem::copy_options::overwrite_existing, ec);
			}
			if (ec)
			{
				return StageFailure("Cannot stage Rust module for loading: " + ec.message());
			}
			m_loaded = path;
		}
		const auto loaded = m_host.Load(path, *m_context->world, m_context);
		if (!loaded)
		{
			m_result.report.status = aether::scripting::AgentJobStatus::Failed;
			m_result.report.phase = "load";
			m_result.report.diagnostics.insert(m_result.report.diagnostics.end(), m_host.Diagnostics().begin(), m_host.Diagnostics().end());
			return std::unexpected(loaded.error());
		}
		AE_INFO(LogCategory::App, "Rust gameplay loaded: {} types", m_host.Gameplay().GetScriptTypeNames().size());
		return {};
	}

	void RustScriptingSubsystem::Unload()
	{
		ClearRebuild();
		m_host.Unload();
		m_context = nullptr;
		if (!m_loaded.empty())
		{
			std::error_code ec;
			std::filesystem::remove(m_loaded, ec);
			std::filesystem::remove(m_loaded.parent_path(), ec);
			m_loaded.clear();
		}
	}
} // namespace aether::app::scripting
