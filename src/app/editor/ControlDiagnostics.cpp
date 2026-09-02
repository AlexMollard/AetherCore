#include "editor/ControlDiagnostics.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "utils/FuzzyMatch.hpp"

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		std::string_view NamespaceOf(std::string_view name)
		{
			const auto dot = name.rfind('.');
			return dot == std::string_view::npos ? std::string_view{} : name.substr(0, dot);
		}

		std::string_view LeafOf(std::string_view name)
		{
			const auto dot = name.rfind('.');
			return dot == std::string_view::npos ? name : name.substr(dot + 1);
		}
	} // namespace

	std::string_view JsonTypeName(const json& value)
	{
		if (value.is_string()) { return "string"; }
		if (value.is_boolean()) { return "boolean"; }
		if (value.is_number_integer()) { return "integer"; }
		if (value.is_number()) { return "number"; }
		if (value.is_array()) { return "array"; }
		if (value.is_object()) { return "object"; }
		if (value.is_null()) { return "null"; }
		return "unknown";
	}

	// Handlers read declared fields with p["key"].get<T>(), which throws when the
	// caller passed the wrong type - and the raw nlohmann message names neither the
	// method nor the parameter, so a caller saw "type must be number, but is string"
	// with no way to tell which argument it meant.
	bool JsonMatchesDeclaredType(const json& value, std::string_view declared)
	{
		if (declared == "number") { return value.is_number(); }
		if (declared == "integer") { return value.is_number_integer(); }
		if (declared == "string") { return value.is_string(); }
		if (declared == "boolean") { return value.is_boolean(); }
		if (declared == "array") { return value.is_array(); }
		if (declared == "object") { return value.is_object(); }
		return true; // no declared type, or one we do not model - leave it to the handler
	}

	// A bare "unknown method" hands back nothing the caller can act on, even though
	// the server knows every name it would have accepted. Callers reach for a
	// plausible-but-wrong name far more often than they misspell one, so the useful
	// signals are the namespace and the leaf, not edit distance alone.
	std::string DescribeUnknownMethod(std::string_view requested, const std::vector<MethodIdentity>& methods)
	{
		const std::string_view wantNamespace = NamespaceOf(requested);
		const std::string_view wantLeaf = LeafOf(requested);

		// The MCP tool alias is advertised by describe but is not what dispatch
		// matches on, so calling by it looks like a name that does not exist.
		for (const MethodIdentity& m: methods)
		{
			if (m.tool == requested)
			{
				return "unknown method: " + std::string(requested) + " ('" + std::string(requested) + "' is the MCP tool alias; call it as '" + m.name + "')";
			}
		}

		std::vector<std::pair<int, std::string>> scored;
		std::size_t inNamespace = 0;
		for (const MethodIdentity& m: methods)
		{
			const std::string_view leaf = LeafOf(m.name);
			if (!wantNamespace.empty() && NamespaceOf(m.name) == wantNamespace)
			{
				++inNamespace;
			}

			// Shared leading characters, so a near-miss like entity/entities or
			// scrol/scroll ranks first. Plain subsequence matching misses both of
			// those: neither is a subsequence of the name it was reaching for.
			std::size_t prefix = 0;
			while (prefix < leaf.size() && prefix < wantLeaf.size() && leaf[prefix] == wantLeaf[prefix])
			{
				++prefix;
			}

			const auto fuzzy = FuzzyMatch(requested, m.name);
			// Three characters, not two: "li" alone pulls in line/lights/list and turns
			// the suggestion into noise, while every real near-miss shares more.
			const bool similar = leaf == wantLeaf || prefix >= 3 || fuzzy.has_value();
			if (!similar)
			{
				continue;
			}

			// A shared namespace alone is not a suggestion - it would rank every
			// method in the namespace equally and print the first few alphabetically.
			int score = static_cast<int>(prefix) * 5 + (leaf == wantLeaf ? 100 : 0) + (fuzzy ? *fuzzy / 10 : 0);
			if (!wantNamespace.empty() && NamespaceOf(m.name) == wantNamespace)
			{
				score += 10;
			}
			scored.emplace_back(score, m.name);
		}

		std::string message = "unknown method: " + std::string(requested);
		if (!scored.empty())
		{
			std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
			scored.resize(std::min<std::size_t>(scored.size(), 5));

			std::string suggestion;
			for (const auto& [ignored, name]: scored)
			{
				suggestion += suggestion.empty() ? "" : ", ";
				suggestion += name;
			}
			message += ". Closest: " + suggestion;
		}
		if (inNamespace > 0)
		{
			message += ". The '" + std::string(wantNamespace) + ".' namespace has " + std::to_string(inNamespace) + " methods";
		}
		return message + ". Call 'describe' for the full list.";
	}
} // namespace aether::editor
