#include "assets/SpriteAnimationAsset.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <sstream>

#include "io/FileSystem.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	namespace
	{
		[[nodiscard]] std::string FrameKey(std::size_t index, std::string_view field)
		{
			return std::format("frame.{}.{}", index, field);
		}

		[[nodiscard]] std::string EventKey(std::size_t index, std::string_view field)
		{
			return std::format("event.{}.{}", index, field);
		}

		[[nodiscard]] const char* LoopModeName(SpriteAnimationLoopMode mode) noexcept
		{
			switch (mode)
			{
				case SpriteAnimationLoopMode::Once:
					return "once";
				case SpriteAnimationLoopMode::PingPong:
					return "ping-pong";
				case SpriteAnimationLoopMode::Hold:
					return "hold";
				case SpriteAnimationLoopMode::Loop:
				default:
					return "loop";
			}
		}

		[[nodiscard]] SpriteAnimationLoopMode ParseLoopMode(std::string_view value) noexcept
		{
			if (value == "once")
			{
				return SpriteAnimationLoopMode::Once;
			}
			if (value == "ping-pong")
			{
				return SpriteAnimationLoopMode::PingPong;
			}
			if (value == "hold")
			{
				return SpriteAnimationLoopMode::Hold;
			}
			return SpriteAnimationLoopMode::Loop;
		}

		[[nodiscard]] std::string IdToString(AssetObjectId id)
		{
			return std::format("{:016x}", id.value);
		}

		[[nodiscard]] AssetObjectId ParseId(std::string_view value)
		{
			AssetObjectId id{};
			const auto result = std::from_chars(value.data(), value.data() + value.size(), id.value, 16);
			return result.ec == std::errc{} ? id : AssetObjectId{};
		}
	} // namespace

	float SpriteAnimationAsset::DurationSeconds() const noexcept
	{
		float duration = 0.0f;
		for (const SpriteAnimationFrame& frame: frames)
		{
			duration += std::max(frame.durationSeconds, 0.001f);
		}
		return duration;
	}

	Expected<void> SpriteAnimationAsset::Save(const std::filesystem::path& path) const
	{
		TomlConfig config;
		config.Set("animation.schema_version", static_cast<float>(kSchemaVersion));
		config.Set("animation.name", name);
		config.Set("animation.atlas", atlasPath);
		config.Set("animation.loop_mode", std::string_view{LoopModeName(loopMode)});
		config.Set("animation.frame_count", static_cast<float>(frames.size()));
		config.Set("animation.event_count", static_cast<float>(events.size()));
		for (std::size_t index = 0; index < frames.size(); ++index)
		{
			config.Set(FrameKey(index, "sprite_id"), IdToString(frames[index].spriteId));
			config.Set(FrameKey(index, "duration"), std::max(frames[index].durationSeconds, 0.001f));
		}
		for (std::size_t index = 0; index < events.size(); ++index)
		{
			config.Set(EventKey(index, "frame"), static_cast<float>(events[index].frameIndex));
			config.Set(EventKey(index, "name"), events[index].name);
			config.Set(EventKey(index, "payload"), events[index].payload);
		}
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			std::ostringstream stream;
			config.Save(stream, "AetherCore sprite animation");
			AE_TRY_VOID(io::FileSystem::WriteFileText(pathString, stream.str()));
		}
		else if (!config.SaveToPath(path, "AetherCore sprite animation"))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to save sprite animation '" + path.string() + "'."));
		}
		return {};
	}

	Expected<SpriteAnimationAsset> SpriteAnimationAsset::Load(const std::filesystem::path& path)
	{
		TomlConfig config;
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			AE_TRY(text, io::FileSystem::ReadFileText(pathString));
			config.Load(*text);
		}
		else if (!config.LoadFromPath(path))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to load sprite animation '" + path.string() + "'."));
		}
		SpriteAnimationAsset animation;
		animation.schemaVersion = static_cast<std::uint32_t>(config.GetFloat("animation.schema_version", 1.0f));
		animation.name = config.GetString("animation.name", "Animation");
		animation.atlasPath = config.GetString("animation.atlas");
		animation.loopMode = ParseLoopMode(config.GetString("animation.loop_mode", "loop"));
		const std::size_t frameCount = static_cast<std::size_t>(std::max(config.GetFloat("animation.frame_count", 0.0f), 0.0f));
		animation.frames.reserve(frameCount);
		for (std::size_t index = 0; index < frameCount; ++index)
		{
			animation.frames.push_back({
			        .spriteId = ParseId(config.GetString(FrameKey(index, "sprite_id"))),
			        .durationSeconds = std::max(config.GetFloat(FrameKey(index, "duration"), 0.1f), 0.001f),
			});
		}
		const std::size_t eventCount = static_cast<std::size_t>(std::max(config.GetFloat("animation.event_count", 0.0f), 0.0f));
		animation.events.reserve(eventCount);
		for (std::size_t index = 0; index < eventCount; ++index)
		{
			animation.events.push_back({
			        .frameIndex = static_cast<std::uint32_t>(std::max(config.GetFloat(EventKey(index, "frame"), 0.0f), 0.0f)),
			        .name = config.GetString(EventKey(index, "name")),
			        .payload = config.GetString(EventKey(index, "payload")),
			});
		}
		return animation;
	}
} // namespace aether
