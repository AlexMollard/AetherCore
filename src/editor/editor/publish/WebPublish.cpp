#include "editor/publish/WebPublish.hpp"

#include "editor/publish/PublishPlan.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "io/Process.hpp"
#include "scripting/AgentDiagnostics.hpp"

namespace aether::editor
{
	nlohmann::json RunWebPublish(const PublishPlan& plan, nlohmann::json request, const std::filesystem::path& directory, std::stop_token stop)
	{
		const auto utf8 = [](const std::filesystem::path& path)
		{
			const auto text = path.generic_u8string();
			return std::string(text.begin(), text.end());
		};
		scripting::AgentReport failed{
		        .jobId = request.value("jobId", "web-publish"), .status = scripting::AgentJobStatus::Failed, .phase = "web-publish", .target = "web", .language = std::string(app::GameplayLanguageName(plan.language)), .profile = plan.profile};
		const auto fail = [&](const std::string& code, const std::string& message)
		{
			failed.diagnostics.push_back({code, scripting::AgentDiagnosticSeverity::Error, message});
			return scripting::SerializeAgentReport(failed).value();
		};
		auto source = std::filesystem::path(__FILE__);
		for (int level = 0; level < 5; ++level)
		{
			source = source.parent_path();
		}
		const auto script = source / "tools/project/web_publish.py";
		if (!std::filesystem::is_regular_file(script))
		{
			return fail("WEB_ENGINE_SOURCE_REQUIRED", "Web publishing requires the matching engine source/toolchain checkout");
		}
		std::filesystem::create_directories(directory);
		request["project"] = utf8(plan.projectRoot);
		request["target"] = "web";
		request["language"] = std::string(app::GameplayLanguageName(plan.language));
		const auto input = directory / "web-request.json";
		const auto output = directory / "web-report.json";
		const auto written = io::file_util::WriteText(input, request.dump(2));
		if (!written)
		{
			return fail("WEB_IO", written.error().message);
		}
		auto python = io::PlatformPaths::ReadEnvironmentVariable("AETHER_PYTHON");
		if (python.empty())
		{
			python = utf8(io::PlatformPaths::FindExecutable("python"));
		}
		if (python.empty())
		{
			return fail("WEB_PYTHON_MISSING", "Install Python or set AETHER_PYTHON to its executable path");
		}
		const std::vector<std::string> arguments{utf8(script), "--request", utf8(input), "--report", utf8(output), "--directory", utf8(directory)};
		const int result = io::RunProcessArguments(std::filesystem::u8path(python), arguments, source, directory / "web-publish.log", request.value("timeoutMs", 600000), stop);
		const auto text = io::file_util::ReadText(output);
		if (!text)
		{
			return fail(result == io::kProcessTimedOut ? "WEB_TIMEOUT" : result == io::kProcessCancelled ? "WEB_CANCELLED" : "WEB_PROCESS_FAILED", "Web publishing produced no terminal report; inspect web-publish.log");
		}
		auto report = nlohmann::json::parse(*text, nullptr, false);
		if (report.is_discarded() || !report.is_object())
		{
			return fail("WEB_REPORT", "Web publish returned an invalid report");
		}
		if (result != 0 && report.value("status", "") == "passed")
		{
			return fail("WEB_PROCESS_FAILED", "Web publisher exited unsuccessfully after its report");
		}
		return report;
	}
} // namespace aether::editor
