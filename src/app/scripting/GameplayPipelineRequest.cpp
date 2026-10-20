#include "scripting/GameplayPipelineRequest.hpp"

#include <cmath>
#include <set>

namespace aether::app::scripting
{
	Expected<nlohmann::json> ValidateGameplayPipelineRequest(const nlohmann::json& request)
	{
		const auto invalid = [](std::string message) { return std::unexpected(AetherError::Engine(std::move(message))); };
		if (!request.is_object()) { return invalid("Gameplay request must be an object"); }
		const std::set<std::string> fields{"jobId", "project", "operation", "target", "scene", "ticks", "step", "timeoutMs", "launchConfig", "name", "language", "template"};
		for (const auto& [name, value]: request.items())
		{
			if (!fields.contains(name)) { return invalid("Unknown gameplay request field: " + name); }
		}
		for (const char* key: {"project", "operation"})
		{
			if (!request.contains(key) || !request[key].is_string() || request[key].get<std::string>().find_first_not_of(" \t\r\n") == std::string::npos || request[key].get<std::string>().find('\0') != std::string::npos) { return invalid(std::string(key) + " must be a nonempty string without NUL"); }
		}
		for (const char* key: {"jobId", "target", "scene", "name", "language", "template"})
		{
			if (request.contains(key) && (!request[key].is_string() || request[key].get<std::string>().find('\0') != std::string::npos)) { return invalid(std::string(key) + " must be a string without NUL"); }
		}
		const std::set<std::string> operations{"create", "inspect", "build", "run", "relaunch", "test", "publish"};
		if (!operations.contains(request["operation"].get<std::string>())) { return invalid("Unsupported gameplay operation"); }
		const auto target = request.value("target", "desktop");
		if (target != "desktop" && target != "web")
		{
			return invalid("Unsupported gameplay target");
		}
		if (target == "web" && request["operation"] != "publish")
		{
			return invalid("Web target currently supports publish; use browser automation for tests");
		}
		for (const auto& [key, bounds]: std::map<std::string, std::pair<std::int64_t, std::int64_t>>{{"ticks", {1, 1000000}}, {"timeoutMs", {100, 600000}}})
		{
			if (request.contains(key) && (!request[key].is_number_integer() || request[key].get<double>() < static_cast<double>(bounds.first) || request[key].get<double>() > static_cast<double>(bounds.second))) { return invalid(key + " is outside its supported integer range"); }
		}
		if (request.contains("step") && (!request["step"].is_number() || !std::isfinite(request["step"].get<double>()) || request["step"].get<double>() <= 0 || request["step"].get<double>() > 1)) { return invalid("step must be finite and in (0, 1]"); }
		if (request.contains("launchConfig") && !request["launchConfig"].is_object()) { return invalid("launchConfig must be an object"); }
		auto normalized = request;
		if (!normalized.contains("target")) { normalized["target"] = "desktop"; }
		if (!normalized.contains("ticks")) { normalized["ticks"] = 60; }
		if (!normalized.contains("timeoutMs")) { normalized["timeoutMs"] = target == "web" ? 600000 : 180000; }
		if (!normalized.contains("step")) { normalized["step"] = 1.0 / 60.0; }
		if (!normalized.contains("launchConfig")) { normalized["launchConfig"] = nlohmann::json::object(); }
		return normalized;
	}
}
