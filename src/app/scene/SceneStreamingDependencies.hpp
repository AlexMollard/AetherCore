#pragma once

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "scene/World.hpp"
#include "utils/Expected.hpp"

namespace aether::app::scene
{
	struct SceneDescription;

	[[nodiscard]] std::expected<void, std::string> EnsureStreamingSceneNodeIds(SceneDescription& scene);
	[[nodiscard]] std::uint64_t StreamingNodeId(std::string_view region, std::uint64_t source) noexcept;
	[[nodiscard]] Expected<Entity> ResolveSceneNodeReference(World& world, std::uint64_t node);

	// Return the canonical UID or project/engine reference, or an empty string
	// when the supplied authored reference cannot be resolved.
	using SceneDependencyResolver = std::function<std::string(std::string_view)>;

	[[nodiscard]] std::expected<void, std::string> CollectSceneStreamingDependencies(const SceneDescription& scene, std::vector<std::string>& out, const SceneDependencyResolver& resolve);
} // namespace aether::app::scene
