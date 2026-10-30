#pragma once

#include "assets/GltfAsset.hpp"
#include "BinaryFormats.hpp"
#include "utils/BinaryReader.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>

namespace aether::assets
{
	inline std::optional<GltfAnimation> ParseExternalAnimation(const std::vector<std::byte>& data)
	{
		BinaryReader reader(data);
#pragma pack(push, 1)

		struct Header
		{
			char magic[4];
			std::uint32_t version, channels;
			std::uint16_t nameLength;
		};

#pragma pack(pop)
		Header header{};
		if (!reader.TryRead(header) || std::memcmp(header.magic, ANIM_MAGIC, 4) != 0 || header.version < 1 || header.version > ANIM_VERSION || header.channels > 65536)
		{
			return {};
		}
		std::uint16_t flags = 0;
		if (header.version >= 3 && !reader.TryRead(flags))
		{
			return {};
		}
		if ((flags & ~ANIM_FLAG_HAS_BONE_NAMES) != 0 || !reader.CanRead(header.nameLength))
		{
			return {};
		}
		GltfAnimation animation;
		animation.name.assign(reinterpret_cast<const char*>(reader.Data()), header.nameLength);
		reader.Advance(header.nameLength);
		if (static_cast<std::uint64_t>(header.channels) * sizeof(ChannelHeaderDisk) > reader.Remaining())
		{
			return {};
		}
		animation.channels.reserve(header.channels);
		for (std::uint32_t i = 0; i < header.channels; ++i)
		{
			ChannelHeaderDisk disk{};
			if (!reader.TryRead(disk) || disk.path > 3 || disk.interp > 2 || disk.keyCount == 0 || disk.keyCount > (1u << 20))
			{
				return {};
			}
			GltfAnimationChannel channel;
			channel.nodeIndex = disk.nodeIndex;
			channel.path = static_cast<GltfAnimationPath>(disk.path);
			channel.interpolation = static_cast<GltfInterpolation>(disk.interp);
			if (flags & ANIM_FLAG_HAS_BONE_NAMES)
			{
				std::uint16_t length = 0;
				if (!reader.TryRead(length) || !reader.CanRead(length))
				{
					return {};
				}
				channel.boneName.assign(reinterpret_cast<const char*>(reader.Data()), length);
				reader.Advance(length);
			}
			const std::size_t valuesPerKey = channel.interpolation == GltfInterpolation::CubicSpline ? 3u : 1u;
			const std::size_t valueCount = static_cast<std::size_t>(disk.keyCount) * valuesPerKey;
			if (static_cast<std::uint64_t>(disk.keyCount) * sizeof(float) + static_cast<std::uint64_t>(valueCount) * sizeof(glm::vec4) > reader.Remaining())
			{
				return {};
			}
			channel.times.resize(disk.keyCount);
			reader.ReadRaw(channel.times.data(), disk.keyCount * sizeof(float));
			channel.values.resize(valueCount);
			float previous = -1.0f;
			for (float time: channel.times)
			{
				if (!std::isfinite(time) || time < 0 || time <= previous)
				{
					return {};
				}
				previous = time;
			}
			for (auto& value: channel.values)
			{
				float components[4];
				reader.ReadRaw(components, sizeof(components));
				for (float component: components)
				{
					if (!std::isfinite(component))
					{
						return {};
					}
				}
				value = glm::vec4(components[0], components[1], components[2], components[3]);
			}
			animation.channels.push_back(std::move(channel));
		}
		if (reader.Remaining() != 0)
		{
			return {};
		}
		return animation;
	}

	inline std::string StripExternalBonePrefix(std::string name)
	{
		for (const char* prefix: {"mixamorig:", "mixamorig_", "Armature_"})
		{
			const auto length = std::strlen(prefix);
			if (name.size() > length && name.compare(0, length, prefix) == 0)
			{
				return name.substr(length);
			}
		}
		return name;
	}

	inline void RemapExternalAnimation(GltfAnimation& animation, std::span<const std::uint32_t> joints, const auto& nodeName)
	{
		std::unordered_map<std::string, std::uint32_t> names;
		for (auto node: joints)
		{
			std::string name(nodeName(node));
			if (name.empty())
			{
				continue;
			}
			names[name] = node;
			const auto bare = StripExternalBonePrefix(name);
			if (bare != name)
			{
				names.try_emplace(bare, node);
			}
			for (const char* prefix: {"mixamorig:", "mixamorig_", "Armature_"})
			{
				names.try_emplace(prefix + bare, node);
			}
		}
		for (auto& channel: animation.channels)
		{
			if (!channel.boneName.empty())
			{
				auto found = names.find(channel.boneName);
				if (found == names.end())
					found = names.find(StripExternalBonePrefix(channel.boneName));
				channel.nodeIndex = found != names.end() ? found->second : UINT32_MAX;
			}
			else
			{
				channel.nodeIndex = channel.nodeIndex < joints.size() ? joints[channel.nodeIndex] : UINT32_MAX;
			}
		}
	}
} // namespace aether::assets
