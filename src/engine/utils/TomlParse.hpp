#pragma once

#include <toml++/toml.hpp>
#include "utils/Expected.hpp"

namespace aether
{
	inline Expected<toml::table> ParseTomlDocument(std::string_view text)
	{
#if TOML_EXCEPTIONS
		try
		{
			return toml::parse(text);
		}
		catch (const toml::parse_error& error)
		{
			AE_UNEXPECTED(AetherError::Asset(error.description()));
		}
#else
		auto parsed = toml::parse(text);
		if (!parsed)
		{
			AE_UNEXPECTED(AetherError::Asset(parsed.error().description()));
		}
		return std::move(parsed).table();
#endif
	}
} // namespace aether
