#include "OverlayBackend.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>

#include "utils/Expected.hpp"

namespace aether::io
{
	namespace
	{
		std::string ApplyPrefix(const std::string& prefix, std::string_view relativePath)
		{
			return prefix.empty() ? std::string(relativePath) : prefix + std::string(relativePath);
		}
	} // namespace

	OverlayBackend::OverlayBackend(std::vector<Layer> layers)
	      : m_layers(std::move(layers))
	{
	}

	bool OverlayBackend::Exists(std::string_view relativePath) const
	{
		for (const auto& layer: m_layers)
		{
			if (layer.backend->Exists(ApplyPrefix(layer.prefix, relativePath)))
			{
				return true;
			}
		}
		return false;
	}

	Expected<std::vector<std::byte>> OverlayBackend::Read(std::string_view relativePath) const
	{
		for (const auto& layer: m_layers)
		{
			auto result = layer.backend->Read(ApplyPrefix(layer.prefix, relativePath));
			if (result.has_value())
			{
				return result;
			}
		}
		AE_UNEXPECTED(AetherError::FileSystem("overlay: not found: " + std::string(relativePath)));
	}

	Expected<std::unique_ptr<std::istream>> OverlayBackend::OpenStream(std::string_view relativePath) const
	{
		for (const auto& layer: m_layers)
		{
			auto result = layer.backend->OpenStream(ApplyPrefix(layer.prefix, relativePath));
			if (result.has_value())
			{
				return result;
			}
		}
		AE_UNEXPECTED(AetherError::FileSystem("overlay: not found: " + std::string(relativePath)));
	}

	Expected<std::vector<std::string>> OverlayBackend::Glob(std::string_view pattern, const FileGlobOptions& options) const
	{
		std::vector<std::string> merged;
		std::unordered_set<std::string> seen;
		std::optional<AetherError> firstError;
		bool anySucceeded = false;

		for (const auto& layer: m_layers)
		{
			auto result = layer.backend->Glob(ApplyPrefix(layer.prefix, pattern), options);
			if (!result.has_value())
			{
				if (!firstError.has_value())
				{
					firstError = result.error();
				}
				continue;
			}

			anySucceeded = true;
			for (auto& path: *result)
			{
				std::string stripped = (!layer.prefix.empty() && path.starts_with(layer.prefix)) ? path.substr(layer.prefix.size()) : path;

				if (seen.insert(stripped).second)
				{
					merged.push_back(std::move(stripped));
				}
			}
		}

		if (!anySucceeded && firstError.has_value())
		{
			AE_UNEXPECTED(*firstError);
		}

		std::ranges::sort(merged);
		return merged;
	}

	Expected<void> OverlayBackend::Write(std::string_view relativePath, std::span<const std::byte> data) const
	{
		if (m_layers.empty())
		{
			AE_UNEXPECTED(AetherError::FileSystem("overlay: no writable layer"));
		}

		const auto& layer = m_layers.front();
		return layer.backend->Write(ApplyPrefix(layer.prefix, relativePath), data);
	}
} // namespace aether::io
