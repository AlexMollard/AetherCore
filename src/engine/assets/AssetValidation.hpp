#pragma once

#include <cmath>
#include "utils/TomlConfig.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	inline Expected<std::size_t> ReadAssetCount(const TomlConfig& config, std::string_view key, std::size_t maximum, float fallback = 0)
	{
		const float value = config.GetFloat(key, fallback);
		if (!std::isfinite(value) || value < 0 || value > maximum || std::floor(value) != value)
		{
			AE_UNEXPECTED(AetherError::Asset("Asset count out of range: " + std::string(key)));
		}
		return static_cast<std::size_t>(value);
	}

	inline Expected<std::uint32_t> ReadAssetVersion(const TomlConfig& config, std::string_view key, std::uint32_t current)
	{
		const float version = config.GetFloat(key, 1);
		if (!std::isfinite(version) || version < 1 || version > current || std::floor(version) != version)
		{
			AE_UNEXPECTED(AetherError::Asset("Unsupported asset schema: " + std::string(key)));
		}
		return static_cast<std::uint32_t>(version);
	}
} // namespace aether
