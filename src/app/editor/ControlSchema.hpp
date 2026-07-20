#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace aether::editor
{
	// Shared JSON-schema builders for the MCP control-method definitions. Previously
	// copy-pasted (and drifting) across the ControlMethods / ControlMethods2D /
	// ControlMethodsPixel translation units.
	inline nlohmann::json Obj(nlohmann::json properties = nlohmann::json::object(), const std::vector<std::string>& required = {})
	{
		nlohmann::json schema{{"type", "object"}, {"properties", std::move(properties)}};
		if (!required.empty())
		{
			schema["required"] = required;
		}
		return schema;
	}

	inline nlohmann::json IntProp()
	{
		return nlohmann::json{{"type", "integer"}};
	}

	inline nlohmann::json StrProp()
	{
		return nlohmann::json{{"type", "string"}};
	}

	inline nlohmann::json NumProp()
	{
		return nlohmann::json{{"type", "number"}};
	}

	inline nlohmann::json BoolProp()
	{
		return nlohmann::json{{"type", "boolean"}};
	}

	inline nlohmann::json RectProp()
	{
		return nlohmann::json{{"type", "array"}, {"items", IntProp()}, {"minItems", 4}, {"maxItems", 4}, {"description", "cell rect [x0, y0, x1, y1], inclusive"}};
	}

	inline nlohmann::json ColorProp()
	{
		return nlohmann::json{{"type", "array"}, {"items", nlohmann::json{{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}}, {"minItems", 3}, {"maxItems", 4}, {"description", "[r, g, b] or [r, g, b, a], 0-255; omit to use the current colour"}};
	}
} // namespace aether::editor
