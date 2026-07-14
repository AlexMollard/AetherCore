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

namespace aether::editor
{
	// render graph, so they only ever run on the main thread (see
	struct MethodContext
	{
		ServiceContainer& services;
		std::uint64_t frameIndex = 0;
		double fps = 0.0;
	};

	using MethodHandler = std::function<nlohmann::json(const nlohmann::json& params, MethodContext& ctx)>;

	struct ControlMethod
	{
		std::string name;
		std::string tool;
		std::string description;
		bool mutates = false;
		nlohmann::json paramsSchema;
		MethodHandler handler;
	};

	[[nodiscard]] std::vector<ControlMethod> BuildControlMethods();
} // namespace aether::editor
