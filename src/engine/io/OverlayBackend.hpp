#pragma once

#include <memory>
#include <string>
#include <vector>

#include "IFileBackend.hpp"
#include "utils/Expected.hpp"

namespace aether::io
{
	class OverlayBackend final : public IFileBackend
	{
	public:
		struct Layer
		{
			std::shared_ptr<IFileBackend> backend;
			std::string prefix;
		};

		explicit OverlayBackend(std::vector<Layer> layers);

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::byte>> Read(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions& options) const override;
		[[nodiscard]] Expected<void> Write(std::string_view relativePath, std::span<const std::byte> data) const override;

	private:
		std::vector<Layer> m_layers;
	};
} // namespace aether::io
