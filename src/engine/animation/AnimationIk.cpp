#include "animation/AnimationIk.hpp"
#include "animation/AnimationDatabase.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include <entt/entt.hpp>
#include <glm/geometric.hpp>

namespace aether
{
	static std::uint32_t HashBoneName(std::string_view name)
	{
		std::uint32_t hash = 0x811C9DC5u;
		for (char c: name)
		{
			hash ^= static_cast<std::uint32_t>(c);
			hash *= 0x01000193u;
		}
		return hash;
	}

	void AnimationIkSystem::Init(VmaAllocator allocator, VkDevice device, VkDeviceSize maxEntities)
	{
		m_allocator = allocator;
		m_maxEntities = static_cast<std::uint32_t>(maxEntities);
		m_entityCount = 0;

		m_ikJobs.resize(static_cast<std::size_t>(maxEntities));
		m_groundResults.resize(static_cast<std::size_t>(maxEntities) * 2);

		{
			AE_EXPECT_OR_THROW(buffer,
			        UniqueBuffer::CreateMapped(
			                m_allocator, device, static_cast<VkDeviceSize>(maxEntities) * sizeof(AnimationContracts::IkSolveJob), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "AnimationIk.IkJobs"));
			m_ikJobsBuffer = std::move(buffer);
			VmaAllocationInfo info = m_ikJobsBuffer.GetAllocationInfo();
			m_mappedIkJobs = static_cast<AnimationContracts::IkSolveJob*>(info.pMappedData);
		}

		{
			AE_EXPECT_OR_THROW(buffer,
			        UniqueBuffer::CreateMapped(
			                m_allocator, device, static_cast<VkDeviceSize>(maxEntities) * 2 * sizeof(AnimationContracts::IkGroundResult), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "AnimationIk.GroundResults"));
			m_groundResultsBuffer = std::move(buffer);
			VmaAllocationInfo info = m_groundResultsBuffer.GetAllocationInfo();
			m_mappedGroundResults = static_cast<AnimationContracts::IkGroundResult*>(info.pMappedData);
		}
	}

	void AnimationIkSystem::Shutdown(VkDevice /*device*/)
	{
		m_ikJobsBuffer.Reset();
		m_groundResultsBuffer.Reset();
		m_ikJobs.clear();
		m_groundResults.clear();
		m_mappedIkJobs = nullptr;
		m_mappedGroundResults = nullptr;
	}

	void AnimationIkSystem::InitEntity(World& world, std::uint32_t entityId, const AnimationDatabase& animDb)
	{
		if (m_entityCount >= m_maxEntities)
		{
			AE_WARN(LogCategory::Animation, "AnimationIkSystem: entity overflow at {} entities.", m_entityCount);
			return;
		}

		const auto& nodeNames = animDb.GetNodeNames();
		const auto& bindTrans = animDb.GetBindTranslations();
		const std::uint32_t nodeCount = animDb.GetNodeCount();

		auto findBoneIndex = [&](std::string_view name) -> std::uint32_t
		{
			const std::uint32_t nameHash = HashBoneName(name);
			for (std::uint32_t i = 0; i < nodeCount; ++i)
			{
				if (HashBoneName(nodeNames[i]) == nameHash)
				{
					return i;
				}
			}
			return UINT32_MAX;
		};

		std::uint32_t hipsIdx = findBoneIndex("Hips");
		std::uint32_t leftUpLegIdx = findBoneIndex("LeftUpLeg");
		std::uint32_t leftLegIdx = findBoneIndex("LeftLeg");
		std::uint32_t leftFootIdx = findBoneIndex("LeftFoot");
		std::uint32_t rightUpLegIdx = findBoneIndex("RightUpLeg");
		std::uint32_t rightLegIdx = findBoneIndex("RightLeg");
		std::uint32_t rightFootIdx = findBoneIndex("RightFoot");

		if (hipsIdx == UINT32_MAX || leftUpLegIdx == UINT32_MAX || leftLegIdx == UINT32_MAX || leftFootIdx == UINT32_MAX)
		{
			AE_WARN(LogCategory::Animation, "AnimationIkSystem: could not find all left leg bones for entity {}.", entityId);
		}
		if (hipsIdx == UINT32_MAX || rightUpLegIdx == UINT32_MAX || rightLegIdx == UINT32_MAX || rightFootIdx == UINT32_MAX)
		{
			AE_WARN(LogCategory::Animation, "AnimationIkSystem: could not find all right leg bones for entity {}.", entityId);
		}

		float leftUpperLen = 0.0f;
		float leftLowerLen = 0.4f;
		float rightUpperLen = 0.0f;
		float rightLowerLen = 0.4f;

		if (leftLegIdx < nodeCount)
		{
			leftUpperLen = glm::length(glm::vec3(bindTrans[leftLegIdx]));
			if (leftUpperLen < 0.01f)
			{
				leftUpperLen = 0.4f;
			}
		}
		if (leftFootIdx < nodeCount)
		{
			leftLowerLen = glm::length(glm::vec3(bindTrans[leftFootIdx]));
			if (leftLowerLen < 0.01f)
			{
				leftLowerLen = 0.4f;
			}
		}
		if (rightLegIdx < nodeCount)
		{
			rightUpperLen = glm::length(glm::vec3(bindTrans[rightLegIdx]));
			if (rightUpperLen < 0.01f)
			{
				rightUpperLen = 0.4f;
			}
		}
		if (rightFootIdx < nodeCount)
		{
			rightLowerLen = glm::length(glm::vec3(bindTrans[rightFootIdx]));
			if (rightLowerLen < 0.01f)
			{
				rightLowerLen = 0.4f;
			}
		}

		float leftKneeBendSign = 1.0f;
		float rightKneeBendSign = 1.0f;

		if (leftFootIdx != UINT32_MAX && leftFootIdx < nodeCount)
		{
			glm::vec3 forward = glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 hipToFoot = glm::vec3(bindTrans[leftFootIdx]);
			float hipLen = glm::length(hipToFoot);
			if (hipLen > 1e-6f)
			{
				hipToFoot /= hipLen;
				glm::vec3 cross = glm::cross(forward, hipToFoot);
				float crossLen = glm::length(cross);
				if (crossLen > 1e-6f)
				{
					leftKneeBendSign = (cross.y > 0.0f) ? 1.0f : -1.0f;
				}
			}
		}

		if (rightFootIdx != UINT32_MAX && rightFootIdx < nodeCount)
		{
			glm::vec3 forward = glm::vec3(0.0f, 0.0f, 1.0f);
			glm::vec3 hipToFoot = glm::vec3(bindTrans[rightFootIdx]);
			float hipLen = glm::length(hipToFoot);
			if (hipLen > 1e-6f)
			{
				hipToFoot /= hipLen;
				glm::vec3 cross = glm::cross(forward, hipToFoot);
				float crossLen = glm::length(cross);
				if (crossLen > 1e-6f)
				{
					rightKneeBendSign = (cross.y > 0.0f) ? 1.0f : -1.0f;
				}
			}
		}

		auto& reg = world.GetRegistry();
		IkTargetsComponent& ikComp = reg.get_or_emplace<IkTargetsComponent>(static_cast<entt::entity>(entityId));
		ikComp.hipsNodeIdx = hipsIdx;
		ikComp.leftKneeNodeIdx = leftLegIdx;
		ikComp.leftFootNodeIdx = leftFootIdx;
		ikComp.rightKneeNodeIdx = rightLegIdx;
		ikComp.rightFootNodeIdx = rightFootIdx;
		ikComp.leftUpperLegLen = leftUpperLen;
		ikComp.leftLowerLegLen = leftLowerLen;
		ikComp.rightUpperLegLen = rightUpperLen;
		ikComp.rightLowerLegLen = rightLowerLen;
		ikComp.leftKneeBendSign = leftKneeBendSign;
		ikComp.rightKneeBendSign = rightKneeBendSign;
		ikComp.enabled = true;

		++m_entityCount;
	}

	void AnimationIkSystem::Update(World& world, PhysicsSystem& physics, float /*dt*/)
	{
		m_writtenIkJobCount = 0;

		auto& reg = world.GetRegistry();
		auto view = reg.view<SkinnedMeshComponent, IkTargetsComponent>();

		std::uint32_t jobIdx = 0;

		for (const auto& [entity, skinned, ikComp]: view.each())
		{
			if (!ikComp.enabled)
			{
				continue;
			}
			if (ikComp.hipsNodeIdx == UINT32_MAX)
			{
				continue;
			}

			std::uint32_t entityId = static_cast<std::uint32_t>(entt::to_integral(entity));

			float rayMaxDist = ikComp.raycastMaxDist;
			if (rayMaxDist < 0.1f)
			{
				rayMaxDist = 2.0f;
			}

			float leftGroundY = 0.0f;
			float rightGroundY = 0.0f;
			float leftOffsetY = 0.0f;
			float rightOffsetY = 0.0f;

			if (ikComp.leftFootNodeIdx != UINT32_MAX)
			{
				glm::vec3 footOrigin(0.0f);
				glm::vec3 footDir(0.0f, -1.0f, 0.0f);
				float footRayLen = ikComp.leftLowerLegLen + 0.15f;
				if (auto result = physics.CastGround(footOrigin, footRayLen); result.hit)
				{
					ikComp.leftFootPlanted = true;
					leftGroundY = result.position.y;
					leftOffsetY = leftGroundY;
				}
				else
				{
					ikComp.leftFootPlanted = false;
				}
			}

			if (ikComp.rightFootNodeIdx != UINT32_MAX)
			{
				glm::vec3 footOrigin(0.0f);
				glm::vec3 footDir(0.0f, -1.0f, 0.0f);
				float footRayLen = ikComp.rightLowerLegLen + 0.15f;
				if (auto result = physics.CastGround(footOrigin, footRayLen); result.hit)
				{
					ikComp.rightFootPlanted = true;
					rightGroundY = result.position.y;
					rightOffsetY = rightGroundY;
				}
				else
				{
					ikComp.rightFootPlanted = false;
				}
			}

			ikComp.leftFootGroundY = leftGroundY;
			ikComp.rightFootGroundY = rightGroundY;

			std::uint32_t groundResultBase = jobIdx * 2;
			if (groundResultBase + 1 < static_cast<std::uint32_t>(m_groundResults.size()))
			{
				m_mappedGroundResults[groundResultBase + 0] = AnimationContracts::IkGroundResult{
				        .entityId = entityId,
				        .footIndex = 0,
				        .groundY = leftGroundY,
				        .footOffsetY = leftOffsetY,
				};
				m_mappedGroundResults[groundResultBase + 1] = AnimationContracts::IkGroundResult{
				        .entityId = entityId,
				        .footIndex = 1,
				        .groundY = rightGroundY,
				        .footOffsetY = rightOffsetY,
				};
			}

			AnimationContracts::IkSolveJob job{};
			job.hipsNodeIdx = ikComp.hipsNodeIdx;
			job.leftKneeNodeIdx = ikComp.leftKneeNodeIdx;
			job.leftFootNodeIdx = ikComp.leftFootNodeIdx;
			job.rightKneeNodeIdx = ikComp.rightKneeNodeIdx;
			job.rightFootNodeIdx = ikComp.rightFootNodeIdx;
			job.leftUpperLegLen = ikComp.leftUpperLegLen;
			job.leftLowerLegLen = ikComp.leftLowerLegLen;
			job.rightUpperLegLen = ikComp.rightUpperLegLen;
			job.rightLowerLegLen = ikComp.rightLowerLegLen;
			job.leftKneeBendSign = ikComp.leftKneeBendSign;
			job.rightKneeBendSign = ikComp.rightKneeBendSign;
			job.entityId = entityId;
			job.globalTransformsAddr = 0;
			job.ikResultsAddr = m_groundResultsBuffer.GetDeviceAddress() + static_cast<VkDeviceSize>(groundResultBase) * sizeof(AnimationContracts::IkGroundResult);

			m_mappedIkJobs[jobIdx] = job;
			++jobIdx;
			++m_writtenIkJobCount;
		}

		AE_EXPECT_OR_THROW_VOID(m_groundResultsBuffer.FlushMapped());
	}

	void AnimationIkSystem::BuildIkSolvePush(gpu::DeviceAddress globalTransformsAddr, gpu::DeviceAddress nodeParentsAddr, gpu::DeviceAddress depthSortedNodesAddr, std::uint32_t nodeCount)
	{
		m_nodeCount = nodeCount;
		if (nodeParentsAddr == 0)
		{
			nodeParentsAddr = m_storedNodeParentsAddr;
		}
		if (depthSortedNodesAddr == 0)
		{
			depthSortedNodesAddr = m_storedDepthSortedNodesAddr;
		}
		m_ikPush = AnimationContracts::IkSolvePush{
		        .globalTransformsAddr = globalTransformsAddr,
		        .ikJobsAddr = m_ikJobsBuffer.GetDeviceAddress(),
		        .ikGroundResultsAddr = m_groundResultsBuffer.GetDeviceAddress(),
		        .nodeParentsAddr = nodeParentsAddr,
		        .depthSortedNodesAddr = depthSortedNodesAddr,
		        .jobCount = GetIkJobCount(),
		        .nodeCount = nodeCount,
		};
	}
} // namespace aether
