#pragma once

// Shared helpers used by all daScript binding modules.
// Reduces per-module boilerplate for the two most common conversions:
//   * glm::vec3 <-> das::float3 field-by-field copy
//   * std::string -> char* deep-copy into the daScript string heap

#include <string>
#include <string_view>

#include "daScript/daScript.h"
#include <glm/glm.hpp>

namespace aether::app::scripting
{
	// -- glm::vec3 <-> das::float3 ------------------------------------------------

	[[nodiscard]] inline glm::vec3 to_glm(das::float3 v) noexcept
	{
		return {v.x, v.y, v.z};
	}

	[[nodiscard]] inline das::float3 to_das(glm::vec3 v) noexcept
	{
		return {v.x, v.y, v.z};
	}

	// -- std::string -> das heap-allocated char* -----------------------------------
	// daScript treats a nullptr return as an empty string - safe to return nullptr
	// when the source is empty so we skip the heap allocation entirely.

	[[nodiscard]] inline char* das_string(das::Context* ctx, const std::string& s)
	{
		return s.empty() ? nullptr : ctx->stringHeap->allocateName(s);
	}

	[[nodiscard]] inline char* das_string(das::Context* ctx, std::string_view s)
	{
		if (s.empty())
		{
			return nullptr;
		}
		// Construct a std::string to guarantee null-termination, then use the
		// std::string overload of allocateName (always available in daScript).
		return ctx->stringHeap->allocateName(std::string(s));
	}

	[[nodiscard]] inline char* das_string(das::Context* ctx, const char* s)
	{
		return (s != nullptr && s[0] != '\0') ? ctx->stringHeap->allocateName(s) : nullptr;
	}

	// -- Borrowed string -> std::string ------------------------------------------
	// daScript passes char* / das::string_view. Deep-copy into a std::string for
	// lifetime safety when storing into a component field.

	[[nodiscard]] inline std::string das_to_std_string(const char* s)
	{
		return s ? std::string(s) : std::string{};
	}
} // namespace aether::app::scripting
