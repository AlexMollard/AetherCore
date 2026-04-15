#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "UniqueBuffer.hpp"
#include "assets/GltfAsset.hpp"

namespace aether
{
	// Runtime skeleton animator for skinned models.
	//
	// Usage:
	//   ModelAnimator anim = ModelAnimator::Create(device, allocator, asset);
	//   // per frame:
	//   anim.Update(dt);
	//   VkDeviceAddress skinAddr = anim.GetSkinBufferAddr(primitive.skinIndex);
	//   // pass skinAddr as a push constant to the draw call
	class ModelAnimator
	{
	public:
		ModelAnimator()  = default;
		~ModelAnimator() = default;

		ModelAnimator(const ModelAnimator&)            = delete;
		ModelAnimator& operator=(const ModelAnimator&) = delete;

		ModelAnimator(ModelAnimator&&) noexcept            = default;
		ModelAnimator& operator=(ModelAnimator&&) noexcept = default;

		// Build an animator from a fully-parsed glTF asset.
		// device/allocator must outlive this object.
		static ModelAnimator Create(
			VkDevice                  device,
			VmaAllocator              allocator,
			const assets::GltfAsset&  asset);

		void Destroy();

		// Advance the current animation by dt seconds (auto-loops).
		void Update(float dt);

		// Switch to a different animation clip (resets time to 0).
		void SetAnimation(std::uint32_t index);

		[[nodiscard]] std::uint32_t  GetAnimationCount()                 const;
		[[nodiscard]] std::string_view GetAnimationName(std::uint32_t i) const;
		[[nodiscard]] std::uint32_t  GetCurrentAnimation()               const { return m_currentAnim; }

		// BDA of the flat joint-matrix palette for the given skin.
		// Returns 0 when skinIndex is -1, out of range, or the skin has no joints.
		[[nodiscard]] VkDeviceAddress GetSkinBufferAddr(std::int32_t skinIndex) const;

		[[nodiscard]] bool IsValid() const { return !m_nodes.empty(); }

	private:
		void ComputeGlobalTransforms();
		void UploadSkinBuffers();
		void ResetNodesToBind();
		float ComputeDuration(std::uint32_t animIndex) const;

		struct BindPose
		{
			glm::vec3 translation { 0.0f };
			glm::quat rotation    { 1.0f, 0.0f, 0.0f, 0.0f };
			glm::vec3 scale       { 1.0f };
			bool      hasMatrix   = false;
			glm::mat4 matrix      { 1.0f };
		};

		struct NodeState
		{
			BindPose     bind;
			glm::vec3    translation { 0.0f };
			glm::quat    rotation    { 1.0f, 0.0f, 0.0f, 0.0f };
			glm::vec3    scale       { 1.0f };
			std::int32_t parentIndex = -1;
			glm::mat4    globalTransform { 1.0f };
		};

		struct SkinData
		{
			std::vector<std::uint32_t> joints;
			std::vector<glm::mat4>     inverseBindMatrices;
			UniqueBuffer               buffer;       // device-addressable, persistently mapped
			glm::mat4*                 mappedPtr = nullptr;
		};

		std::vector<NodeState>             m_nodes;
		std::vector<SkinData>              m_skins;
		std::vector<assets::GltfAnimation> m_animations;

		std::uint32_t m_currentAnim = 0;
		float         m_time        = 0.0f;
		float         m_duration    = 0.0f;

		VkDevice     m_device    = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
	};
}
