#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace aether::editor
{
	// The pieces of a refusal that are pure text over pure data, kept out of the server
	// so they can be tested without standing an editor up. Both exist because the
	// original refusals named a symptom the caller could not act on: "unknown method"
	// with no hint what the real one was, and a raw nlohmann type error naming neither
	// the method nor the parameter.

	// One entry of the method table, reduced to what a refusal needs.
	struct MethodIdentity
	{
		std::string name; // the dotted name dispatch matches on
		std::string tool; // the MCP alias describe advertises but dispatch ignores
	};

	// Message for a method name that does not exist. Names the MCP alias when that is
	// what was used, otherwise suggests the closest real names and says how many methods
	// the requested namespace holds. A shared namespace ALONE is never a suggestion - it
	// ranks every method in the namespace equally and prints the first few alphabetically.
	[[nodiscard]] std::string DescribeUnknownMethod(std::string_view requested, const std::vector<MethodIdentity>& known);

	// The JSON type name as a refusal should spell it.
	[[nodiscard]] std::string_view JsonTypeName(const nlohmann::json& value);

	// Whether a value satisfies the type its schema declares. An unmodelled or absent
	// declared type passes: the handler is left to judge what the schema did not say.
	[[nodiscard]] bool JsonMatchesDeclaredType(const nlohmann::json& value, std::string_view declared);
} // namespace aether::editor
