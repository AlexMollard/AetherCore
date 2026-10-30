#include "assets/ModelPose.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/GltfAsset.hpp"
#include "scene/Components.hpp"

namespace aether::assets
{
	namespace
	{
		bool Finite(const glm::vec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		bool Finite(const glm::mat4& value)
		{
			return Finite(value[0]) && Finite(value[1]) && Finite(value[2]) && Finite(value[3]);
		}

		glm::vec4 NormalizeRotation(const glm::vec4& value)
		{
			// Scale first so large finite quaternions cannot overflow the length calculation.
			const float magnitude = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z), std::abs(value.w)});
			return magnitude == 0.0f ? glm::vec4{0.0f, 0.0f, 0.0f, 1.0f} : glm::normalize(value / magnitude);
		}

		glm::vec4 Sample(const GltfAnimationChannel& channel, float time)
		{
			const bool cubic = channel.interpolation == GltfInterpolation::CubicSpline;
			const auto valueAt = [&channel, cubic](std::size_t index)
			{
				return channel.values[cubic ? index * 3 + 1 : index];
			};
			if (time <= channel.times.front())
			{
				return valueAt(0);
			}
			if (time >= channel.times.back())
			{
				return valueAt(channel.times.size() - 1);
			}
			const auto hi = static_cast<std::size_t>(std::upper_bound(channel.times.begin(), channel.times.end(), time) - channel.times.begin());
			const auto lo = hi - 1;
			const glm::vec4 start = valueAt(lo);
			glm::vec4 end = valueAt(hi);
			if (channel.interpolation == GltfInterpolation::Step)
			{
				return start;
			}
			const float duration = channel.times[hi] - channel.times[lo];
			const float alpha = (time - channel.times[lo]) / duration;
			if (cubic)
			{
				const float squared = alpha * alpha;
				const float cubed = squared * alpha;
				return (2.0f * cubed - 3.0f * squared + 1.0f) * start + (cubed - 2.0f * squared + alpha) * duration * channel.values[lo * 3 + 2] + (-2.0f * cubed + 3.0f * squared) * end + (cubed - squared) * duration * channel.values[hi * 3];
			}
			if (channel.path == GltfAnimationPath::Rotation && glm::dot(start, end) < 0.0f)
			{
				end = -end;
			}
			return glm::mix(start, end, alpha);
		}
	} // namespace

	Expected<ModelPose> EvaluateModelPose(const GltfAsset& asset, std::optional<std::uint32_t> clip, float time, bool looping, const PoseFade* fade, std::span<const JointRotationOffset> offsets)
	{
		const auto& animations = asset.animations;
		if (!std::isfinite(time) || asset.nodes.size() > std::numeric_limits<std::uint32_t>::max() || asset.skins.size() > std::numeric_limits<std::uint32_t>::max())
		{
			AE_UNEXPECTED(AetherError::Asset("Model pose has a non-finite time or unsupported node/skin count"));
		}
		if ((clip && *clip >= animations.size()) || (fade && fade->weight > 0.0f && (!clip || fade->clip >= animations.size() || !std::isfinite(fade->time))))
		{
			AE_UNEXPECTED(AetherError::Asset("Model pose animation clip index is out of range"));
		}

		std::vector<glm::vec3> translations;
		std::vector<glm::vec4> rotations;
		std::vector<glm::vec3> scales;
		std::vector<glm::mat4> locals;
		translations.reserve(asset.nodes.size());
		rotations.reserve(asset.nodes.size());
		scales.reserve(asset.nodes.size());
		locals.reserve(asset.nodes.size());
		for (const auto& node: asset.nodes)
		{
			if (node.parentIndex < -1 || (node.parentIndex >= 0 && static_cast<std::size_t>(node.parentIndex) >= asset.nodes.size()))
			{
				AE_UNEXPECTED(AetherError::Asset("Model pose node parent index is out of range"));
			}
			for (const auto child: node.children)
			{
				if (child >= asset.nodes.size())
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose node child index is out of range"));
				}
			}
			const glm::vec4 rotation{node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w};
			if (!Finite(glm::vec4(node.translation, 0.0f)) || !Finite(rotation) || !Finite(glm::vec4(node.scale, 0.0f)) || (node.hasMatrix && !Finite(node.matrix)))
			{
				AE_UNEXPECTED(AetherError::Asset("Model pose node transform is non-finite"));
			}
			translations.push_back(node.translation);
			rotations.push_back(NormalizeRotation(rotation));
			scales.push_back(node.scale);
			locals.push_back(node.matrix);
		}

		// Samples every channel of one clip over the pose. weight >= 1 overwrites; below 1 the
		// sample is blended over what is already there (animation_sample.slang SampleClip: lerp
		// for translation/scale, nlerp for rotation), so a crossfade's outgoing clip eases out
		// over the incoming one and nodes it does not animate keep the other clip's value.
		const auto sampleClip = [&](std::uint32_t clipIndex, float clipTime, bool clipLooping, float weight) -> Expected<void>
		{
			const auto& animation = animations[clipIndex];
			float duration = 0.0f;
			for (const auto& channel: animation.channels)
			{
				const bool cubic = channel.interpolation == GltfInterpolation::CubicSpline;
				if (channel.nodeIndex >= asset.nodes.size() || asset.nodes[channel.nodeIndex].hasMatrix || channel.times.empty() || channel.times.size() > std::numeric_limits<std::uint32_t>::max() / (cubic ? 3u : 1u)
				        || channel.values.size() != channel.times.size() * (cubic ? 3u : 1u))
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose animation channel has an invalid target or key/value count"));
				}
				if ((channel.path != GltfAnimationPath::Translation && channel.path != GltfAnimationPath::Rotation && channel.path != GltfAnimationPath::Scale)
				        || (channel.interpolation != GltfInterpolation::Step && channel.interpolation != GltfInterpolation::Linear && !cubic))
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose animation channel path or interpolation is unsupported"));
				}
				float previous = -1.0f;
				for (const float key: channel.times)
				{
					if (!std::isfinite(key) || key < 0.0f || key <= previous)
					{
						AE_UNEXPECTED(AetherError::Asset("Model pose animation key times must be finite, nonnegative and strictly increasing"));
					}
					previous = key;
				}
				for (const auto& value: channel.values)
				{
					if (!Finite(value))
					{
						AE_UNEXPECTED(AetherError::Asset("Model pose animation value is non-finite"));
					}
				}
				duration = std::max(duration, channel.times.back());
			}
			float sampleTime = std::clamp(clipTime, 0.0f, duration);
			if (clipLooping && duration > 0.0f)
			{
				sampleTime = std::fmod(clipTime, duration);
				if (sampleTime < 0.0f)
				{
					sampleTime += duration;
				}
			}
			const bool overwrite = weight >= 1.0f;
			for (const auto& channel: animation.channels)
			{
				const auto node = channel.nodeIndex;
				// Mirror AnimationCompiler's root-lock targets (children of node zero).
				// Keep the authored duration, as AnimationDatabase::Create does. External
				// import compilation removes locked channels before computing its duration.
				if (animation.rootLocked && channel.path == GltfAnimationPath::Translation && asset.nodes[node].parentIndex == 0)
				{
					continue;
				}
				const glm::vec4 sample = Sample(channel, sampleTime);
				if (!Finite(sample))
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose animation sample overflowed"));
				}
				switch (channel.path)
				{
					case GltfAnimationPath::Translation:
						translations[node] = overwrite ? glm::vec3(sample) : glm::mix(translations[node], glm::vec3(sample), weight);
						break;
					case GltfAnimationPath::Rotation:
					{
						if (overwrite)
						{
							rotations[node] = NormalizeRotation(sample);
						}
						else
						{
							glm::vec4 incoming = NormalizeRotation(sample);
							if (glm::dot(rotations[node], incoming) < 0.0f)
							{
								incoming = -incoming;
							}
							rotations[node] = NormalizeRotation(glm::mix(rotations[node], incoming, weight));
						}
						break;
					}
					case GltfAnimationPath::Scale:
						scales[node] = overwrite ? glm::vec3(sample) : glm::mix(scales[node], glm::vec3(sample), weight);
						break;
					default:
						break;
				}
			}
			return {};
		};
		if (clip)
		{
			AE_TRY_VOID(sampleClip(*clip, time, looping, 1.0f));
			if (fade && fade->weight > 0.0f)
			{
				AE_TRY_VOID(sampleClip(fade->clip, fade->time, fade->looping, fade->weight));
			}
		}

		ModelPose pose;
		pose.nodeWorld.resize(asset.nodes.size());
		std::vector<std::uint8_t> states(asset.nodes.size(), 0);
		std::vector<std::size_t> chain;
		for (std::size_t node = 0; node < asset.nodes.size(); ++node)
		{
			if (!asset.nodes[node].hasMatrix)
			{
				const auto& q = rotations[node];
				locals[node] = glm::translate(glm::mat4{1.0f}, translations[node]) * glm::mat4_cast(glm::quat{q.w, q.x, q.y, q.z}) * glm::scale(glm::mat4{1.0f}, scales[node]);
			}
		}
		for (const auto& offset: offsets)
		{
			if (offset.node < locals.size())
			{
				locals[offset.node] *= glm::mat4_cast(offset.rotation);
			}
		}
		for (std::size_t node = 0; node < asset.nodes.size(); ++node)
		{
			chain.clear();
			auto current = static_cast<std::int64_t>(node);
			while (current >= 0 && states[static_cast<std::size_t>(current)] == 0)
			{
				const auto index = static_cast<std::size_t>(current);
				states[index] = 1;
				chain.push_back(index);
				current = asset.nodes[index].parentIndex;
			}
			if (current >= 0 && states[static_cast<std::size_t>(current)] == 1)
			{
				AE_UNEXPECTED(AetherError::Asset("Model pose node hierarchy contains a cycle"));
			}
			for (auto entry = chain.rbegin(); entry != chain.rend(); ++entry)
			{
				const auto parent = asset.nodes[*entry].parentIndex;
				pose.nodeWorld[*entry] = parent < 0 ? locals[*entry] : pose.nodeWorld[static_cast<std::size_t>(parent)] * locals[*entry];
				if (!Finite(pose.nodeWorld[*entry]))
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose node hierarchy transform overflowed"));
				}
				states[*entry] = 2;
			}
		}
		pose.skins.reserve(asset.skins.size());
		for (const auto& skin: asset.skins)
		{
			if (skin.joints.size() > std::numeric_limits<std::uint32_t>::max() || (!skin.inverseBindMatrices.empty() && skin.inverseBindMatrices.size() != skin.joints.size()) || skin.skeletonRoot < -1
			        || (skin.skeletonRoot >= 0 && static_cast<std::size_t>(skin.skeletonRoot) >= asset.nodes.size()))
			{
				AE_UNEXPECTED(AetherError::Asset("Model pose skin has invalid joint/bind counts or skeleton root"));
			}
			auto& palette = pose.skins.emplace_back();
			palette.reserve(skin.joints.size());
			for (std::size_t joint = 0; joint < skin.joints.size(); ++joint)
			{
				if (skin.joints[joint] >= pose.nodeWorld.size())
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose skin joint index is out of range"));
				}
				palette.push_back(pose.nodeWorld[skin.joints[joint]] * (skin.inverseBindMatrices.empty() ? glm::mat4{1.0f} : skin.inverseBindMatrices[joint]));
				if (!Finite(palette.back()))
				{
					AE_UNEXPECTED(AetherError::Asset("Model pose skin matrix is non-finite"));
				}
			}
		}
		return pose;
	}
} // namespace aether::assets
