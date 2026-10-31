#include "editor/publish/PublishTransaction.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "editor/publish/PublishPlan.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		StepResult Failure(std::string message)
		{
			return {.ok = false, .message = std::move(message), .remediation = "Check write permissions and close anything using the publish output, then publish again."};
		}

		class OutputTransaction
		{
		public:
			~OutputTransaction()
			{
				if (!m_workspace.empty() && !m_preserveBackup)
				{
					std::error_code ec;
					std::filesystem::remove_all(m_workspace, ec);
					if (ec)
					{
						AE_WARN(LogCategory::App, "Could not remove publish workspace '{}': {}", m_workspace.generic_string(), ec.message());
					}
				}
			}

			StepResult Begin(const std::filesystem::path& output)
			{
				if (output.empty() || output.filename().empty())
				{
					return Failure("The publish output directory is invalid.");
				}
				m_output = output;
				std::error_code ec;
				std::filesystem::create_directories(output.parent_path(), ec);
				if (ec)
				{
					return Failure("Could not create the publish parent folder: " + ec.message());
				}

				static std::atomic_uint64_t sequence{0};
				for (int attempt = 0; attempt < 32; ++attempt)
				{
					const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence.fetch_add(1));
					auto workspaceName = output.filename();
					workspaceName += ".publish-" + suffix;
					const auto candidate = output.parent_path() / workspaceName;
					if (std::filesystem::create_directory(candidate, ec))
					{
						m_workspace = candidate;
						break;
					}
					if (ec)
					{
						return Failure("Could not reserve a publish workspace: " + ec.message());
					}
				}
				if (m_workspace.empty())
				{
					return Failure("Could not reserve a unique publish workspace.");
				}
				std::filesystem::create_directory(StagingDirectory(), ec);
				return ec ? Failure("Could not create the staged package folder: " + ec.message()) : StepResult{};
			}

			std::filesystem::path StagingDirectory() const { return m_workspace / "package"; }

			StepResult Commit()
			{
				std::error_code ec;
				const bool hadOutput = std::filesystem::exists(m_output, ec);
				if (ec)
				{
					return Failure("Could not inspect the previous publish output: " + ec.message());
				}
				const auto backup = m_workspace / "previous";
				if (hadOutput)
				{
					std::filesystem::rename(m_output, backup, ec);
					if (ec)
					{
						return Failure("Could not preserve the previous publish output: " + ec.message());
					}
					// Until promotion or rollback completes, cleanup must never erase the backup.
					m_preserveBackup = true;
				}
				std::filesystem::rename(StagingDirectory(), m_output, ec);
				if (ec)
				{
					const std::string error = ec.message();
					if (hadOutput)
					{
						std::filesystem::rename(backup, m_output, ec);
						if (ec)
						{
							return Failure("Could not promote the package (" + error + ") or restore the previous output (" + ec.message()
							        + "). The previous output is preserved at " + backup.generic_string() + ".");
						}
					}
					m_preserveBackup = false;
					return Failure("Could not promote the package: " + error);
				}
				m_preserveBackup = false;
				return {};
			}

		private:
			std::filesystem::path m_output;
			std::filesystem::path m_workspace;
			bool m_preserveBackup = false;
		};
	}

	StepResult RunNativePublishSteps(const PublishPlan& plan, const std::span<const PublishStep> steps, PublishContext& context,
	        const PublishToolchain& toolchain, const EditorProjectPublishProgress& progress)
	{
		OutputTransaction transaction;
		try
		{
			if (steps.empty())
			{
				return Failure("The native publish pipeline is empty.");
			}
			if (const auto begin = transaction.Begin(plan.outputDir); !begin.ok)
			{
				return begin;
			}
			PublishPlan stagedPlan = plan;
			stagedPlan.outputDir = transaction.StagingDirectory();
			for (std::size_t i = 0; i < steps.size(); ++i)
			{
				if (progress)
				{
					progress(static_cast<float>(i) / static_cast<float>(steps.size()), steps[i].name);
				}
				const auto result = steps[i].run(stagedPlan, context, toolchain);
				if (!result.ok)
				{
					AE_ERROR(LogCategory::App, "Publish failed at '{}': {}", steps[i].name, result.message);
					return result;
				}
			}
			return transaction.Commit();
		}
		catch (const std::exception& error)
		{
			return Failure("Publishing failed: " + std::string(error.what()));
		}
	}
}
