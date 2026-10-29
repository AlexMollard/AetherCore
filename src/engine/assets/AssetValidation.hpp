#pragma once

#include <cmath>
#include <limits>
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

	inline Expected<std::string> ReadAssetReference(const TomlConfig& config, std::string_view key)
	{
		const auto raw = config.GetRawString(key);
		auto resolved = config.GetString(key);
		if (raw.starts_with("uid://") && resolved.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Unresolved asset reference: " + raw));
		}
		return raw.starts_with("uid://") ? raw : resolved;
	}

	inline Expected<std::uint32_t> ReadAssetVersion(const TomlConfig& config, std::string_view key, std::uint32_t current)
	{
		const double version = config.Has(key) ? config.GetNumber(key).value_or(std::numeric_limits<double>::quiet_NaN()) : 1.0;
		if (!std::isfinite(version) || version < 1 || version > current || std::floor(version) != version)
		{
			AE_UNEXPECTED(AetherError::Asset("Unsupported asset schema: " + std::string(key)));
		}
		return static_cast<std::uint32_t>(version);
	}
} // namespace aether
