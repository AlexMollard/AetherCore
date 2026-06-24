#pragma once

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/World.hpp"
#include <vector>

namespace aether
{
	// Per-frame CPU-written SSBO holding the engine's animation blend jobs.
	//
	// Storage path: stores a single gpu::BufferHandle (8 bytes, typed,
	// generation-checked). Allocated through gpu::ResourceRegistry::
	// CreateMappedBuffer which uses the registry's 3-frame deferred-
	// destruction ring. CPU writes go through ResolveMappedBuffer().mappedPtr;
	// GPU addresses through ResolveBuffer().deviceAddress.
	class AnimationBlendSystem
	{
	public:
		void Init(gpu::DeviceSize maxBlendJobCount, std::uint32_t nodeCount);
		void Shutdown();

		void PopulateBlendJobs(World& world, const AnimationDatabase& animDb);

		void BuildBlendPush(const AnimationDatabase& animDb, gpu::DeviceAddress sampledPosesAddr);

		[[nodiscard]] const AnimationContracts::AnimationBlendPush& GetBlendPush() const
		{
			return m_blendPush;
		}

		[[nodiscard]] std::uint32_t GetBlendJobCount() const
		{
			return m_writtenJobCount;
		}

		[[nodiscard]] gpu::DeviceAddress GetBlendJobsDeviceAddress() const
		{
			return m_address;
		}

	private:
		gpu::BufferHandle m_handle{};
		gpu::DeviceAddress m_address = 0;
		std::vector<AnimationContracts::AnimatorBlendJob> m_blendJobs;
		AnimationContracts::AnimationBlendPush m_blendPush{};
		std::uint32_t m_nodeCount = 0;
		std::uint32_t m_writtenJobCount = 0;
		AnimationContracts::AnimatorBlendJob* m_mappedBlendJobs = nullptr;
	};
} // namespace aether
