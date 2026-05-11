#include "animation/ModelAnimator.hpp"

#include <algorithm>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>

#include "utils/AetherExceptions.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Keyframe bracket for time t in a sorted time array.
		struct Bracket
		{
			std::size_t lo;
			std::size_t hi;
			float alpha;
		};

		Bracket FindBracket(const std::vector<float>& times, float t)
		{
			if (times.empty())
			{
				return { 0, 0, 0.0f };
			}
			if (times.size() == 1 || t <= times.front())
			{
				return { 0, 0, 0.0f };
			}
			if (t >= times.back())
			{
				return { times.size() - 1, times.size() - 1, 0.0f };
			}
			for (std::size_t i = 0; i + 1 < times.size(); ++i)
			{
				if (t < times[i + 1])
				{
					const float range = times[i + 1] - times[i];
					const float alpha = (range > 1e-6f) ? (t - times[i]) / range : 0.0f;
					return { i, i + 1, alpha };
				}
			}
			return { times.size() - 1, times.size() - 1, 0.0f };
		}

		glm::vec3 SampleVec3(const assets::GltfAnimationChannel& ch, float t)
		{
			if (ch.values.empty())
			{
				return glm::vec3(0.0f);
			}
			if (ch.interpolation == assets::GltfInterpolation::Step || ch.values.size() == 1)
			{
				const auto [lo, hi, a] = FindBracket(ch.times, t);
				return glm::vec3(ch.values[lo]);
			}
			const auto [lo, hi, a] = FindBracket(ch.times, t);
			return glm::mix(glm::vec3(ch.values[lo]), glm::vec3(ch.values[hi]), a);
		}

		glm::quat SampleQuat(const assets::GltfAnimationChannel& ch, float t)
		{
			if (ch.values.empty())
			{
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			}
			// glTF stores quaternion as (x, y, z, w) packed into values[i].xyzw.
			auto toQuat = [](const glm::vec4& v)
			{
				return glm::quat(v.w, v.x, v.y, v.z);
			};
			if (ch.interpolation == assets::GltfInterpolation::Step || ch.values.size() == 1)
			{
				const auto [lo, hi, a] = FindBracket(ch.times, t);
				return toQuat(ch.values[lo]);
			}
			const auto [lo, hi, a] = FindBracket(ch.times, t);
			return glm::normalize(glm::slerp(toQuat(ch.values[lo]), toQuat(ch.values[hi]), a));
		}
	} // namespace

	// ── Factory ───────────────────────────────────────────────────────────────

	ModelAnimator ModelAnimator::Create(VkDevice device, VmaAllocator allocator, const assets::GltfAsset& asset)
	{
		ModelAnimator anim;
		anim.m_device = device;
		anim.m_allocator = allocator;
		anim.m_animations = asset.animations;

		// Build per-node state from the asset's bind-pose TRS.
		anim.m_nodes.resize(asset.nodes.size());
		for (std::size_t i = 0; i < asset.nodes.size(); ++i)
		{
			const assets::GltfNode& src = asset.nodes[i];
			NodeState& dst = anim.m_nodes[i];

			dst.bind.translation = src.translation;
			dst.bind.rotation = src.rotation;
			dst.bind.scale = src.scale;
			dst.bind.hasMatrix = src.hasMatrix;
			dst.bind.matrix = src.matrix;
			dst.parentIndex = src.parentIndex;

			dst.translation = src.translation;
			dst.rotation = src.rotation;
			dst.scale = src.scale;
		}

		// Build per-skin GPU buffers.
		anim.m_skins.resize(asset.skins.size());
		for (std::size_t i = 0; i < asset.skins.size(); ++i)
		{
			const assets::GltfSkin& src = asset.skins[i];
			SkinData& dst = anim.m_skins[i];

			dst.joints = src.joints;
			dst.inverseBindMatrices = src.inverseBindMatrices;

			// Pad inverse bind matrices to joint count if the accessor was missing.
			if (dst.inverseBindMatrices.size() < dst.joints.size())
			{
				dst.inverseBindMatrices.resize(dst.joints.size(), glm::mat4(1.0f));
			}

			if (!dst.joints.empty())
			{
				const VkDeviceSize bufSize = sizeof(glm::mat4) * dst.joints.size();
				const VkBufferCreateInfo bufInfo{
					.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					.size = bufSize,
					.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				};
				const VmaAllocationCreateInfo allocInfo{
					.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
					.usage = VMA_MEMORY_USAGE_AUTO,
				};
				dst.buffer = UniqueBuffer::Create(allocator, device, bufInfo, allocInfo);
				dst.mappedPtr = static_cast<glm::mat4*>(dst.buffer.GetAllocationInfo().pMappedData);

				// Prime with identity so the mesh renders in bind pose before Update().
				for (std::size_t j = 0; j < dst.joints.size(); ++j)
				{
					dst.mappedPtr[j] = glm::mat4(1.0f);
				}
			}
		}

		// Set duration and do an initial pose evaluation.
		anim.SetAnimation(0);
		anim.ComputeGlobalTransforms();
		anim.UploadSkinBuffers();

		return anim;
	}

	void ModelAnimator::Destroy()
	{
		for (SkinData& skin: m_skins)
		{
			skin.buffer.Reset();
			skin.mappedPtr = nullptr;
		}
		m_skins.clear();
		m_nodes.clear();
		m_animations.clear();
		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	// ── Animation control ─────────────────────────────────────────────────────

	float ModelAnimator::ComputeDuration(std::uint32_t animIndex) const
	{
		if (animIndex >= m_animations.size())
		{
			return 0.0f;
		}
		float d = 0.0f;
		for (const assets::GltfAnimationChannel& ch: m_animations[animIndex].channels)
		{
			if (!ch.times.empty())
			{
				d = std::max(d, ch.times.back());
			}
		}
		return d;
	}

	void ModelAnimator::SetAnimation(std::uint32_t index)
	{
		if (index >= m_animations.size())
		{
			return;
		}
		m_currentAnim = index;
		m_time = 0.0f;
		m_duration = ComputeDuration(index);
	}

	// ── Per-frame update ──────────────────────────────────────────────────────

	void ModelAnimator::ResetNodesToBind()
	{
		for (NodeState& node: m_nodes)
		{
			node.translation = node.bind.translation;
			node.rotation = node.bind.rotation;
			node.scale = node.bind.scale;
		}
	}

	void ModelAnimator::Update(float dt)
	{
		AE_PROFILE_ZONE();
		if (m_animations.empty())
		{
			return;
		}

		if (m_duration > 0.0f)
		{
			m_time += dt * m_playbackSpeed;
			// Wrap looping.
			while (m_time > m_duration)
			{
				m_time -= m_duration;
			}
			while (m_time < 0.0f)
			{
				m_time += m_duration;
			}
		}

		// Reset all nodes to their bind-pose TRS so un-animated nodes stay correct.
		ResetNodesToBind();

		if (m_currentAnim < m_animations.size())
		{
			for (const assets::GltfAnimationChannel& ch: m_animations[m_currentAnim].channels)
			{
				if (ch.nodeIndex >= m_nodes.size())
				{
					continue;
				}

				NodeState& node = m_nodes[ch.nodeIndex];
				switch (ch.path)
				{
					case assets::GltfAnimationPath::Translation:
						node.translation = SampleVec3(ch, m_time);
						break;
					case assets::GltfAnimationPath::Rotation:
						node.rotation = SampleQuat(ch, m_time);
						break;
					case assets::GltfAnimationPath::Scale:
						node.scale = SampleVec3(ch, m_time);
						break;
					default:
						break;
				}
			}
		}

		ComputeGlobalTransforms();
		UploadSkinBuffers();
	}

	// ── Internal helpers ──────────────────────────────────────────────────────

	void ModelAnimator::ComputeGlobalTransforms()
	{
		// Pre-compute local matrices.
		std::vector<glm::mat4> local(m_nodes.size());
		for (std::size_t i = 0; i < m_nodes.size(); ++i)
		{
			const NodeState& node = m_nodes[i];
			if (node.bind.hasMatrix)
			{
				local[i] = node.bind.matrix;
			}
			else
			{
				local[i] = glm::translate(glm::mat4(1.0f), node.translation) * glm::mat4_cast(node.rotation) * glm::scale(glm::mat4(1.0f), node.scale);
			}
		}

		// Walk parent chain for each node (robust regardless of node ordering).
		for (std::size_t i = 0; i < m_nodes.size(); ++i)
		{
			glm::mat4 global = local[i];
			std::int32_t parent = m_nodes[i].parentIndex;
			while (parent >= 0)
			{
				global = local[static_cast<std::size_t>(parent)] * global;
				parent = m_nodes[static_cast<std::size_t>(parent)].parentIndex;
			}
			m_nodes[i].globalTransform = global;
		}
	}

	void ModelAnimator::UploadSkinBuffers()
	{
		for (SkinData& skin: m_skins)
		{
			if (skin.mappedPtr == nullptr)
			{
				continue;
			}
			for (std::size_t j = 0; j < skin.joints.size(); ++j)
			{
				const std::uint32_t nodeIdx = skin.joints[j];
				if (nodeIdx >= m_nodes.size())
				{
					skin.mappedPtr[j] = glm::mat4(1.0f);
					continue;
				}
				skin.mappedPtr[j] = m_nodes[nodeIdx].globalTransform * skin.inverseBindMatrices[j];
			}
		}
	}

	// ── Public queries ────────────────────────────────────────────────────────

	// ── Clone / per-instance helpers ──────────────────────────────────────────

	ModelAnimator ModelAnimator::Clone() const
	{
		ModelAnimator clone;
		clone.m_device = m_device;
		clone.m_allocator = m_allocator;
		clone.m_animations = m_animations; // copy clip data (read-only after load)
		clone.m_nodes = m_nodes;           // copy current node TRS state
		clone.m_currentAnim = m_currentAnim;
		clone.m_time = m_time;
		clone.m_duration = m_duration;
		clone.m_playbackSpeed = m_playbackSpeed;

		// Allocate independent GPU skin buffers for this clone.
		clone.m_skins.resize(m_skins.size());
		for (std::size_t i = 0; i < m_skins.size(); ++i)
		{
			const SkinData& src = m_skins[i];
			SkinData& dst = clone.m_skins[i];

			dst.joints = src.joints;
			dst.inverseBindMatrices = src.inverseBindMatrices;

			if (!dst.joints.empty())
			{
				const VkDeviceSize bufSize = sizeof(glm::mat4) * dst.joints.size();
				const VkBufferCreateInfo bufInfo{
					.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					.size = bufSize,
					.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				};
				const VmaAllocationCreateInfo allocInfo{
					.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
					.usage = VMA_MEMORY_USAGE_AUTO,
				};
				dst.buffer = UniqueBuffer::Create(clone.m_allocator, clone.m_device, bufInfo, allocInfo);
				dst.mappedPtr = static_cast<glm::mat4*>(dst.buffer.GetAllocationInfo().pMappedData);

				// Initialise the clone's GPU buffer with the source's current pose.
				if (src.mappedPtr && dst.mappedPtr)
				{
					std::memcpy(dst.mappedPtr, src.mappedPtr, bufSize);
				}
				else if (dst.mappedPtr)
				{
					for (std::size_t j = 0; j < dst.joints.size(); ++j)
					{
						dst.mappedPtr[j] = glm::mat4(1.0f);
					}
				}
			}
		}

		return clone;
	}

	void ModelAnimator::SetAnimTime(float t)
	{
		if (m_duration > 0.0f)
		{
			m_time = std::fmod(t, m_duration);
			if (m_time < 0.0f)
			{
				m_time += m_duration;
			}
		}
		else
		{
			m_time = 0.0f;
		}
		// Re-evaluate the pose at the new time without advancing the clock.
		Update(0.0f);
	}

	void ModelAnimator::SetPlaybackSpeed(float speed)
	{
		m_playbackSpeed = speed;
	}

	std::uint32_t ModelAnimator::GetAnimationCount() const
	{
		return static_cast<std::uint32_t>(m_animations.size());
	}

	std::string_view ModelAnimator::GetAnimationName(std::uint32_t index) const
	{
		if (index >= m_animations.size())
		{
			return {};
		}
		return m_animations[index].name;
	}

	VkDeviceAddress ModelAnimator::GetSkinBufferAddr(std::int32_t skinIndex) const
	{
		if (skinIndex < 0 || static_cast<std::size_t>(skinIndex) >= m_skins.size())
		{
			return 0;
		}
		const SkinData& skin = m_skins[static_cast<std::size_t>(skinIndex)];
		if (!skin.buffer)
		{
			return 0;
		}
		return skin.buffer.GetDeviceAddress();
	}

	std::uint32_t ModelAnimator::GetSkinJointCount(std::int32_t skinIndex) const
	{
		if (skinIndex < 0 || static_cast<std::size_t>(skinIndex) >= m_skins.size())
		{
			return 0;
		}
		return static_cast<std::uint32_t>(m_skins[static_cast<std::size_t>(skinIndex)].joints.size());
	}
} // namespace aether
