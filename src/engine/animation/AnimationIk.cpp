#include "animation/AnimationIk.hpp"
#include "animation/AnimationDatabase.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
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

	void AnimationIkSystem::Init(gpu::Allocator allocator, gpu::Device device, gpu::DeviceSize maxEntities)
	{
		(void) allocator;
		(void) device;
		m_maxEntities = static_cast<std::uint32_t>(maxEntities);
		m_entityCount = 0;

		m_ikJobs.resize(static_cast<std::size_t>(maxEntities));
		m_groundResults.resize(static_cast<std::size_t>(maxEntities) * 2);

		const gpu::MappedBufferDesc ikDesc{
		        .size = maxEntities * sizeof(AnimationContracts::IkSolveJob),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "AnimationIk.IkJobs",
		};
		m_ikJobsHandle = gpu::ResourceRegistry::CreateMappedBuffer(ikDesc);
		if (!m_ikJobsHandle.IsValid())
		{
			Throw(AetherError::Engine("AnimationIkSystem: IkJobs CreateMappedBuffer failed"));
		}
		const auto ikView = gpu::ResourceRegistry::ResolveMappedBuffer(m_ikJobsHandle);
		m_mappedIkJobs = static_cast<AnimationContracts::IkSolveJob*>(ikView.mappedPtr);
		m_ikJobsAddress = ikView.deviceAddress;

		const gpu::MappedBufferDesc grDesc{
		        .size = maxEntities * 2 * sizeof(AnimationContracts::IkGroundResult),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "AnimationIk.GroundResults",
		};
		m_groundResultsHandle = gpu::ResourceRegistry::CreateMappedBuffer(grDesc);
		if (!m_groundResultsHandle.IsValid())
		{
			Throw(AetherError::Engine("AnimationIkSystem: GroundResults CreateMappedBuffer failed"));
		}
		const auto grView = gpu::ResourceRegistry::ResolveMappedBuffer(m_groundResultsHandle);
		m_mappedGroundResults = static_cast<AnimationContracts::IkGroundResult*>(grView.mappedPtr);
		m_groundResultsAddress = grView.deviceAddress;
	}

	void AnimationIkSystem::Shutdown(gpu::Device /*device*/)
	{
		if (m_ikJobsHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_ikJobsHandle);
		}
		if (m_groundResultsHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_groundResultsHandle);
		}
		m_ikJobsHandle = {};
		m_groundResultsHandle = {};
		m_ikJobsAddress = 0;
		m_groundResultsAddress = 0;
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
			auto hipToFoot = glm::vec3(bindTrans[leftFootIdx]);
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
			auto hipToFoot = glm::vec3(bindTrans[rightFootIdx]);
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

			auto entityId = static_cast<std::uint32_t>(entt::to_integral(entity));

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
			job.ikResultsAddr = m_groundResultsAddress + static_cast<gpu::DeviceSize>(groundResultBase) * sizeof(AnimationContracts::IkGroundResult);

			m_mappedIkJobs[jobIdx] = job;
			++jobIdx;
			++m_writtenIkJobCount;
		}

		gpu::ResourceRegistry::FlushMappedBuffer(m_groundResultsHandle, 0, static_cast<gpu::DeviceSize>(-1));
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
		        .ikJobsAddr = m_ikJobsAddress,
		        .ikGroundResultsAddr = m_groundResultsAddress,
		        .nodeParentsAddr = nodeParentsAddr,
		        .depthSortedNodesAddr = depthSortedNodesAddr,
		        .jobCount = GetIkJobCount(),
		        .nodeCount = nodeCount,
		};
	}
} // namespace aether
