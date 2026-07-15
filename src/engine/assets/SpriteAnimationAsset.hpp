#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	enum class SpriteAnimationLoopMode : std::uint8_t
	{
		Loop = 0,
		Once,
		PingPong,
		Hold,
	};

	struct SpriteAnimationFrame
	{
		AssetObjectId spriteId{};
		float durationSeconds = 0.1f;
	};

	struct SpriteAnimationEvent
	{
		std::uint32_t frameIndex = 0;
		std::string name;
		std::string payload;
	};

	struct SpriteAnimationAsset
	{
		static constexpr std::uint32_t kSchemaVersion = 1;

		std::uint32_t schemaVersion = kSchemaVersion;
		std::string name = "Animation";
		std::string atlasPath;
		SpriteAnimationLoopMode loopMode = SpriteAnimationLoopMode::Loop;
		std::vector<SpriteAnimationFrame> frames;
		std::vector<SpriteAnimationEvent> events;

		[[nodiscard]] float DurationSeconds() const noexcept;
		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<SpriteAnimationAsset> Load(const std::filesystem::path& path);
	};
} // namespace aether
