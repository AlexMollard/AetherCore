#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include "scene/SceneSerializer.hpp"

namespace aether::app::scene
{
	enum class SceneDocumentKind
	{
		Scene,
		Prefab
	};

	using SceneDocumentReader = std::function<std::expected<SceneDescription, std::string>(std::string_view, SceneDocumentKind)>;

	struct SceneDocumentExpansionLimits
	{
		std::size_t maxDepth = 32;
		std::size_t maxEntities = 100000;
		bool preserveExternalNodeReferences = false;
	};

	// CPU-only document composition. Parent and script/joint indices in the result
	// refer to the flattened entity array; node references are resolved after composition.
	// The reader owns path resolution and reports missing/invalid source documents.
	[[nodiscard]] std::expected<SceneDescription, std::string> ExpandSceneDocument(const SceneDescription& scene, const SceneDocumentReader& reader, SceneDocumentExpansionLimits limits = {});
} // namespace aether::app::scene
