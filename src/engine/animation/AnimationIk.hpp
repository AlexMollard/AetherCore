#pragma once

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/GpuContracts.hpp"
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
	//
	// Storage path: stores two gpu::BufferHandle (8 bytes each, typed,
	// generation-checked). Allocated through gpu::ResourceRegistry::
	// CreateMappedBuffer which uses the registry's 3-frame deferred-
	// destruction ring. CPU writes go through ResolveMappedBuffer().mappedPtr;
	// GPU addresses through ResolveBuffer().deviceAddress.
	class AnimationIkSystem
	{
	public:
		void Init(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize maxEntities);
		void Shutdown(gpu::Device device);

		// One-time per-entity setup: looks up bone indices and computes leg lengths.
		// Call once when entity is spawned with a skeleton.
		void InitEntity(World& world, std::uint32_t entityId, const AnimationDatabase& animDb);

		// Per-frame update: CPU raycast for each entity's feet, writes IkGroundResult
		// to the staging buffer, populates IK jobs for GPU dispatch.
		void Update(World& world, PhysicsSystem& physics, float dt);

		std::uint32_t GetIkJobCount() const
		{
			return m_writtenIkJobCount;
		}

		const AnimationContracts::IkSolveJob* GetIkJobsData() const
		{
			return m_ikJobs.data();
		}

		gpu::DeviceAddress GetIkJobsDeviceAddress() const
		{
			return m_ikJobsAddress;
		}

		gpu::DeviceAddress GetGroundResultsDeviceAddress() const
		{
			return m_groundResultsAddress;
		}

		std::uint32_t GetGroundResultCount() const
		{
			return static_cast<std::uint32_t>(m_groundResults.size());
		}

		void BuildIkSolvePush(gpu::DeviceAddress globalTransformsAddr, gpu::DeviceAddress nodeParentsAddr, gpu::DeviceAddress depthSortedNodesAddr, std::uint32_t nodeCount);

		void SetDatabaseAddrs(gpu::DeviceAddress nodeParents, gpu::DeviceAddress depthSorted)
		{
			m_storedNodeParentsAddr = nodeParents;
			m_storedDepthSortedNodesAddr = depthSorted;
		}

		const AnimationContracts::IkSolvePush& GetIkSolvePush() const
		{
			return m_ikPush;
		}

	private:
		gpu::BufferHandle m_ikJobsHandle{};
		gpu::BufferHandle m_groundResultsHandle{};
		gpu::DeviceAddress m_ikJobsAddress = 0;
		gpu::DeviceAddress m_groundResultsAddress = 0;
		std::vector<AnimationContracts::IkSolveJob> m_ikJobs;
		std::vector<AnimationContracts::IkGroundResult> m_groundResults;
		AnimationContracts::IkSolvePush m_ikPush{};
		AnimationContracts::IkSolveJob* m_mappedIkJobs = nullptr;
		AnimationContracts::IkGroundResult* m_mappedGroundResults = nullptr;
		gpu::Allocator m_allocator = nullptr;
		std::uint32_t m_entityCount = 0;
		std::uint32_t m_maxEntities = 0;
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_writtenIkJobCount = 0;
		gpu::DeviceAddress m_storedNodeParentsAddr = 0;
		gpu::DeviceAddress m_storedDepthSortedNodesAddr = 0;
	};
} // namespace aether
