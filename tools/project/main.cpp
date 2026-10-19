#include "ProjectPipeline.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include "ProjectCli.hpp"
#include "io/FileUtil.hpp"
#include "scripting/AgentDiagnostics.hpp"

int ProjectMain(int argc, char** argv)
{
	using namespace aether;
	const auto requestPath = std::filesystem::u8path(app::ParseOptionArg(argc, argv, "--request"));
	const auto reportPath = std::filesystem::u8path(app::ParseOptionArg(argc, argv, "--report"));
	const auto cancelPath = std::filesystem::u8path(app::ParseOptionArg(argc, argv, "--cancel-file"));
	std::string jobId = "invalid-request";
	const auto write = [&](const nlohmann::json& report)
	{
		if (reportPath.empty()) { throw std::runtime_error("--report is required"); }
		if (!reportPath.parent_path().empty()) { std::filesystem::create_directories(reportPath.parent_path()); }
		const auto result = io::file_util::WriteText(reportPath, report.dump(2));
		if (!result) { throw std::runtime_error(result.error().message); }
	};
	try
	{
		const auto text = io::file_util::ReadText(requestPath);
		if (!text) { throw std::runtime_error("--request must name an existing JSON request file"); }
		const auto request = nlohmann::json::parse(*text);
		if (request.is_object() && request.contains("jobId") && request["jobId"].is_string()) { jobId = request["jobId"].get<std::string>(); }
		std::stop_source cancellation;
		std::jthread monitor([&](std::stop_token stop)
		{
			while (!stop.stop_requested())
			{
				std::error_code error;
				if (!cancelPath.empty() && std::filesystem::exists(cancelPath, error)) { cancellation.request_stop(); return; }
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		});
		nlohmann::json report;
		if (app::HasFlagArg(argc, argv, "--run-child") || app::HasFlagArg(argc, argv, "--publish-child"))
		{
			report = project_tool::ExecuteChild(request, app::HasFlagArg(argc, argv, "--publish-child"));
		}
		else { report = project_tool::ExecutePipeline(request, std::filesystem::absolute(reportPath).parent_path(), cancellation.get_token(), write); }
		write(report);
		std::cout << report.dump() << '\n';
		return report.value("exitCode", 1);
	}
	catch (const std::exception& error)
	{
		scripting::AgentReport failed{.jobId = jobId, .status = scripting::AgentJobStatus::Failed, .phase = "preflight", .target = "desktop", .language = "unknown", .profile = "unknown", .diagnostics = {{.code = "GAMEPLAY_REQUEST", .severity = scripting::AgentDiagnosticSeverity::Error, .message = error.what()}}};
		const auto report = scripting::SerializeAgentReport(failed);
		try { if (report) { write(*report); std::cout << report->dump() << '\n'; } } catch (...) {}
		return 1;
	}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> values;
	std::vector<char*> pointers;
	for (int index = 0; index < argc; ++index)
	{
		const auto text = std::filesystem::path(argv[index]).u8string();
		values.emplace_back(text.begin(), text.end());
	}
	for (auto& value: values) { pointers.push_back(value.data()); }
	return ProjectMain(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return ProjectMain(argc, argv); }
#endif
