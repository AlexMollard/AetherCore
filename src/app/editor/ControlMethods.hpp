#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aether
{
	class ServiceContainer;
}

namespace aether::app::editor
{
	// Everything a control-endpoint method handler needs. Handlers touch the ECS /
	// render graph, so they only ever run on the main thread (see
	// ControlServer::DrainCommands). Kept free of ControlServer itself so the
	// method table stays a plain, testable list of pure functions.
	struct MethodContext
	{
		ServiceContainer& services;
		std::uint64_t frameIndex = 0;
		double fps = 0.0;
	};

	// A handler returns its JSON result object; put an "error" key in it to signal
	// failure (the transport wraps it into {id, error}).
	using MethodHandler = std::function<nlohmann::json(const nlohmann::json& params, MethodContext& ctx)>;

	// One registered control method. This struct is the SINGLE SOURCE OF TRUTH for
	// a capability: its wire name, the friendly MCP tool name, a description, a
	// JSON-Schema for its params, and the handler. The endpoint's `describe` method
	// emits this list as a manifest, and the MCP generates its tools from it - so
	// adding a capability is exactly one entry here and zero lines of Python.
	struct ControlMethod
	{
		std::string name;             // wire method, e.g. "scene.create"
		std::string tool;             // friendly MCP tool name, e.g. "create_entity"
		std::string description;      // one line, shown to the agent
		bool mutates = false;         // hint: does it change engine state?
		nlohmann::json paramsSchema;  // JSON Schema (object) for the params
		MethodHandler handler;
	};

	// Builds the full method table. Add new capabilities here.
	[[nodiscard]] std::vector<ControlMethod> BuildControlMethods();
} // namespace aether::app::editor
