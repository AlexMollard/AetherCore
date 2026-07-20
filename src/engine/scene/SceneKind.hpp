#pragma once

#include <cstdint>

namespace aether
{
	// Unity-style: a scene's kind is an EDITOR HINT (default camera projection,
	// viewport mode, template seed), never a capability gate. Every feature is
	// available in every scene, which is what makes hybrid 2D/3D games work.
	enum class SceneKind
	{
		Scene3D,
		Scene2D,
		Mixed,
	};

	enum class SceneFeatureFlags : std::uint32_t
	{
		None = 0,
		Sprites = 1u << 0u,
		Tilemaps = 1u << 1u,
		Physics2D = 1u << 2u,
		Meshes3D = 1u << 3u,
		Lighting3D = 1u << 4u,
		Navigation = 1u << 5u,
		Physics3D = 1u << 6u,
	};

	[[nodiscard]] constexpr SceneFeatureFlags operator|(SceneFeatureFlags lhs, SceneFeatureFlags rhs) noexcept
	{
		return static_cast<SceneFeatureFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
	}

	constexpr SceneFeatureFlags& operator|=(SceneFeatureFlags& lhs, SceneFeatureFlags rhs) noexcept
	{
		lhs = lhs | rhs;
		return lhs;
	}

	[[nodiscard]] constexpr bool HasSceneFeature(SceneFeatureFlags features, SceneFeatureFlags feature) noexcept
	{
		return (static_cast<std::uint32_t>(features) & static_cast<std::uint32_t>(feature)) != 0;
	}

	[[nodiscard]] constexpr SceneFeatureFlags DefaultSceneFeatures(SceneKind kind) noexcept
	{
		constexpr SceneFeatureFlags features3D = SceneFeatureFlags::Meshes3D | SceneFeatureFlags::Lighting3D | SceneFeatureFlags::Navigation | SceneFeatureFlags::Physics3D;
		switch (kind)
		{
			case SceneKind::Scene2D:
				return SceneFeatureFlags::Sprites | SceneFeatureFlags::Physics2D | SceneFeatureFlags::Tilemaps;
			case SceneKind::Mixed:
				return SceneFeatureFlags::Sprites | features3D;
			case SceneKind::Scene3D:
			default:
				return features3D;
		}
	}

	// Every feature is available in every scene kind. The flags persist as
	// scene metadata (template seeds, tooling hints) but grant nothing; the
	// only remaining domain rule is per-entity - one entity never simulates
	// in both physics domains (ComponentCatalog conflictsWith).
	[[nodiscard]] constexpr SceneFeatureFlags AllowedSceneFeatures(SceneKind) noexcept
	{
		return static_cast<SceneFeatureFlags>(~0u);
	}

	[[nodiscard]] constexpr bool HasAllSceneFeatures(SceneFeatureFlags features, SceneFeatureFlags required) noexcept
	{
		return (static_cast<std::uint32_t>(features) & static_cast<std::uint32_t>(required)) == static_cast<std::uint32_t>(required);
	}
} // namespace aether
