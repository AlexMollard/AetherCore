#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "MeshProcessor.hpp"

// Shared editor, batch and packer contract for complete, content-verified model bakes.
namespace aether::assetpipeline
{
	struct BakeWriteResult
	{
		bool ok = false;
		std::string error;
		std::filesystem::path meshPath;
		std::vector<std::string> warnings;
	};

	// Writes `baked`'s outputs next to `modelDiskPath` (<stem>.mesh/.skel/.animset), animation
	// clips under `projectRoot/animations`, and materials at their own MeshProcessor-computed
	// project-relative paths. Rejects ambiguous/conflicting generated materials BEFORE writes.
	// Files are replaced atomically; a receipt is published last. Changed/partial bakes
	// cannot match the old receipt, which remains available as ownership evidence for retry.
	// Identical material sharing and authored override paths retain their existing identities.
	[[nodiscard]] BakeWriteResult WriteBakedOutputs(const MeshProcessor::ProcessedResult& baked, const std::filesystem::path& modelDiskPath, const std::filesystem::path& projectRoot);

	// Receipts fingerprint the source, external dependencies, override settings (including
	// absent candidates), importer/format versions and every generated output's complete bytes.
	// Legacy/header-only caches without receipts are deliberately rebuilt once.
	[[nodiscard]] bool HasCurrentModelBake(const std::filesystem::path& modelDiskPath, const std::filesystem::path& projectRoot);
} // namespace aether::assetpipeline
