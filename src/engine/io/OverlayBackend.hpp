#pragma once

#include <memory>
#include <string>
#include <vector>

#include "IFileBackend.hpp"
#include "utils/Expected.hpp"

namespace aether::io
{
	// Composite IFileBackend that layers ordered sub-backends, each with an
	// optional path prefix prepended to the relative path before delegating.
	//
	// Layer 0 is highest priority: Exists/Read/OpenStream try layers in order
	// and return the first hit; Glob unions results across all layers (higher
	// priority wins on name collisions); Write always targets the first layer.
	//
	// This is a general-purpose composite with no shader-specific logic; it is
	// the reusable backend that the shaders:// mount will later be built on top
	// of to layer project shaders over engine shaders.
	class OverlayBackend final : public IFileBackend
	{
	public:
		struct Layer
		{
			std::shared_ptr<IFileBackend> backend;
			std::string prefix; // prepended to the relative path before delegating (e.g. "shaders/")
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
