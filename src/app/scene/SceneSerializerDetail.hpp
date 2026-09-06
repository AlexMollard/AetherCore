#pragma once

// Helpers shared by the SceneSerializer translation units (SceneSerializer,
// SceneSerializerToml, SceneSerializerCapture, SceneSerializerApply). The
// serializer is split into several TUs so the TOML tables, capture, and apply
// code compile independently - keep this header light and engine-only.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"

namespace aether::app::scene::detail
{
	inline std::optional<PrimitiveMesh> PrimitiveFromName(std::string_view s)
	{
		if (s == "cube")
		{
			return PrimitiveMesh::Cube;
		}
		if (s == "sphere")
		{
			return PrimitiveMesh::Sphere;
		}
		if (s == "plane")
		{
			return PrimitiveMesh::Plane;
		}
		if (s == "quad")
		{
			return PrimitiveMesh::Quad;
		}
		if (s == "triangle")
		{
			return PrimitiveMesh::Triangle;
		}
		return std::nullopt;
	}

	// Entity/Component script properties hold live entity ids in memory but scene-local
	// indices on disk; these convert between the two. -1 is the "no reference" sentinel
	// (not 0): a live Entity's own id 0 already means "unassigned" (Entity::IsValid() is
	// id != 0), but 0 is ALSO a valid array index - the first captured entity - so reusing
	// it as "unset" made a real reference to that entity indistinguishable from having no
	// reference at all. ScriptPropsFromSceneRefs below already treats any negative i64 as
	// unresolved, so -1 round-trips through TOML and apply with no further changes there.
	inline std::map<std::string, ScriptPropertyValue> ScriptPropsToSceneRefs(const std::map<std::string, ScriptPropertyValue>& props, const std::unordered_map<std::uint32_t, int>& indexOf)
	{
		std::map<std::string, ScriptPropertyValue> out = props;
		for (auto& [_, value]: out)
		{
			const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
			if (!isRef)
			{
				continue;
			}
			if (value.i64 == 0)
			{
				value.i64 = -1;
				continue;
			}
			const auto it = indexOf.find(static_cast<std::uint32_t>(value.i64));
			// Not found (the live entity isn't part of this capture) is exactly as
			// unresolvable as no reference at all - same sentinel, same reasoning.
			value.i64 = it != indexOf.end() ? it->second : -1;
		}
		return out;
	}

	inline std::map<std::string, ScriptPropertyValue> ScriptPropsFromSceneRefs(const std::map<std::string, ScriptPropertyValue>& props, const std::vector<Entity>& created)
	{
		std::map<std::string, ScriptPropertyValue> out = props;
		for (auto& [_, value]: out)
		{
			const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
			if (!isRef || value.i64 < 0 || static_cast<std::size_t>(value.i64) >= created.size())
			{
				if (isRef)
				{
					value.i64 = 0;
				}
				continue;
			}
			value.i64 = created[static_cast<std::size_t>(value.i64)].id;
		}
		return out;
	}
} // namespace aether::app::scene::detail
