#pragma once

#include "physics/PhysicsSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace aether
{
	class AnimationDatabase;

	// CPU-side IK system: raycasts for foot grounding, knee bend sign
	// computation, and GPU IK job population.
	//
	// Per-entity setup (once at spawn):
	//   - Bone index lookup (Hips, knee, foot nodes per leg)
	//   - Leg chain length computation from bind-pose translations
	//   - Knee bend sign: determines which side of the hip-to-foot
	//     vector the knee bends (computed from bind-pose positions)
	//
	// Per-frame:
	//   - CPU raycast (Jolt CastRay) for each foot against static geometry
	//   - Write IkGroundResult entries to a GPU-visible staging buffer
	//   - Populate IkSolveJob array for GPU dispatch
	class AnimationIkSystem
	{
	public:
		void Init(VmaAllocator allocator, VkDevice device, VkDeviceSize maxEntities);
		void Shutdown(VkDevice device);

		// One-time per-entity setup: looks up bone indices and computes leg lengths.
		// Call once when entity is spawned with a skeleton.
		void InitEntity(World& world, std::uint32_t entityId, const AnimationDatabase& animDb);

		// Per-frame update: CPU raycast for each entity's feet, writes IkGroundResult
		// to the staging buffer, populates IK jobs for GPU dispatch.
		void Update(
		    World& world,
		    PhysicsSystem& physics,
		    float dt);

		std::uint32_t GetIkJobCount() const
		{
			return m_writtenIkJobCount;
		}

		const AnimationContracts::IkSolveJob* GetIkJobsData() const
		{
			return m_ikJobs.data();
		}

		VkDeviceAddress GetIkJobsDeviceAddress() const
		{
			return m_ikJobsBuffer.GetDeviceAddress();
		}

		VkDeviceAddress GetGroundResultsDeviceAddress() const
		{
			return m_groundResultsBuffer.GetDeviceAddress();
		}

		std::uint32_t GetGroundResultCount() const
		{
			return static_cast<std::uint32_t>(m_groundResults.size());
		}

		void BuildIkSolvePush(
		    VkDeviceAddress globalTransformsAddr,
		    VkDeviceAddress nodeParentsAddr,
		    VkDeviceAddress depthSortedNodesAddr,
		    std::uint32_t nodeCount);

		const AnimationContracts::IkSolvePush& GetIkSolvePush() const
		{
			return m_ikPush;
		}

private:
		UniqueBuffer m_ikJobsBuffer;
		UniqueBuffer m_groundResultsBuffer;
		std::vector<AnimationContracts::IkSolveJob> m_ikJobs;
		std::vector<AnimationContracts::IkGroundResult> m_groundResults;
		AnimationContracts::IkSolvePush m_ikPush{};
		AnimationContracts::IkSolveJob* m_mappedIkJobs = nullptr;
		AnimationContracts::IkGroundResult* m_mappedGroundResults = nullptr;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		std::uint32_t m_entityCount = 0;
		std::uint32_t m_maxEntities = 0;
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_writtenIkJobCount = 0;
	};
} // namespace aether