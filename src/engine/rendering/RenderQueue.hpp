#pragma once

#include <functional>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <glm/glm.hpp>
#include <mutex>
#include <string>
#include <vector>
#include "gpu/CommandList.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

#include "animation/AnimationDatabase.hpp"
#include "rendering/GpuContracts.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether
{
	class AnimationBlendSystem;
	class GraphicsPipeline;
	class Mesh;

	struct RenderQueueSharedPipelines
	{
		gpu::PipelineHandle skinCopy;
		gpu::PipelineHandle animSample;
		gpu::PipelineHandle nodeFlatten;
		gpu::PipelineHandle poseInit;
		gpu::PipelineHandle animBlend;

		void Initialize(gpu::Device device);
		void Shutdown();
	};

	struct DrawCommand
	{
		const GraphicsPipeline* pipeline = nullptr;
		const Mesh* mesh = nullptr; // must be indexed; null draws are not supported
		std::uint32_t instanceCount = 1;
		glm::mat4 modelMatrix{1.0f};
		std::uint32_t materialIndex = 0xFFFFFFFFu;
		std::uint32_t effectParamIndex = 0xFFFFFFFFu;
		std::int32_t skinIndex = -1;
		std::uint32_t skinJointCount = 0;
		std::uint32_t animClipIndex = 0;
		float animTime = 0.0f;
		glm::vec4 worldBoundingSphere{};
		const AnimationDatabase* animDb = nullptr;
		std::uint32_t animDbGeneration = 0;
		std::uint32_t meshGeneration = 0;
	};

	struct RenderQueueConfig
	{
		std::uint32_t maxDraws = 8192;
		std::uint32_t maxBatches = 1024;
		std::uint32_t maxAnimationDraws = UINT32_MAX;
		std::uint32_t outputDrawCapacity = 0;
		const char* debugName = "RenderQueue";
	};

	class RenderQueue
	{
	public:
		static constexpr std::uint32_t kFramesInFlight = Swapchain::kMaxFramesInFlight;
		static constexpr std::uint32_t kDefaultMaxAnimationDraws = 1024u;

		RenderQueue() = default;

		~RenderQueue()
		{
			Shutdown();
		}

		RenderQueue(const RenderQueue&) = delete;
		RenderQueue& operator=(const RenderQueue&) = delete;
		RenderQueue(RenderQueue&&) = delete;
		RenderQueue& operator=(RenderQueue&&) = delete;

		void Initialize(const RenderQueueSharedPipelines& pipelines, const RenderQueueConfig& config = {});
		void Shutdown();

		void SetAnimationDatabase(const AnimationDatabase* db)
		{
			m_animationDb = db;
		}

		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		void Submit(const DrawCommand& cmd);

		void SetDebugForceVisible(bool enabled)
		{
			m_debugForceVisible = enabled;
		}

		[[nodiscard]] bool IsDebugForceVisible() const
		{
			return m_debugForceVisible;
		}

		void SetDebugBypassIndirect(bool enabled)
		{
			m_debugBypassIndirect = enabled;
		}

		[[nodiscard]] bool IsDebugBypassIndirect() const
		{
			return m_debugBypassIndirect;
		}

		void SetDebugDisableAnimation(bool enabled)
		{
			m_debugDisableAnimation = enabled;
		}

		[[nodiscard]] bool IsDebugDisableAnimation() const
		{
			return m_debugDisableAnimation;
		}

		void SetDebugAnimPassMask(std::uint32_t mask)
		{
			m_debugAnimPassMask = mask;
		}

		void SetDebugLogSkinJobs(std::uint32_t frameCount)
		{
			m_debugLogSkinJobsFramesLeft = frameCount;
		}

		void SetAnimationBlendSystem(AnimationBlendSystem* sys)
		{
			m_animationBlendSystem = sys;
		}

		// constants addresses; the computePipeline/layout must then be compatible
		void PrepareAndDispatch(gpu::CommandList& cmd, gpu::DeviceAddress frameAddr, gpu::PipelineView computePipeline, std::uint32_t frameIndex);

		void SetMultiCullFrameAddrs(const gpu::DeviceAddress addrs[3])
		{
			m_multiFrameAddrs[0] = addrs[0];
			m_multiFrameAddrs[1] = addrs[1];
			m_multiFrameAddrs[2] = addrs[2];
		}

		[[nodiscard]] std::uint32_t GetMaxDraws() const
		{
			return m_maxDraws;
		}

		[[nodiscard]] std::uint32_t GetMaxSkinJoints() const
		{
			return m_maxSkinJoints;
		}

		[[nodiscard]] gpu::DeviceAddress GetSkinPaletteBufferAddress() const
		{
			return m_skinPalette[0].address;
		}

		void FlushDraw(gpu::CommandList& cmd,
		        std::uint32_t frameIndex,
		        const DrawContracts::LightingAddresses* lighting = nullptr,
		        const GraphicsPipeline* overridePipeline = nullptr,
		        std::uint32_t cascadeOffset = 0,
		        const gpu::CullMode* cullModeOverride = nullptr);
		void FlushDrawPush(
		        gpu::CommandList& cmd, std::uint32_t frameIndex, const DrawContracts::LightingAddresses& lighting, const GraphicsPipeline* overridePipeline = nullptr, std::uint32_t cascadeOffset = 0, const gpu::CullMode* cullModeOverride = nullptr);

		void FlushDrawWithFrameAddr(gpu::CommandList& cmd,
		        std::uint32_t frameIndex,
		        const DrawContracts::LightingAddresses* lighting,
		        gpu::DeviceAddress overrideFrameAddr,
		        const GraphicsPipeline* overridePipeline = nullptr,
		        std::uint32_t cascadeOffset = 0,
		        const gpu::CullMode* cullModeOverride = nullptr);

		void Clear(std::uint32_t slot);
		void DiscardPending(std::uint32_t slot);
		void DiscardAllPending();

		[[nodiscard]] bool IsEmpty(std::uint32_t slot) const;

		// Whether anything submitted into this slot carried an animation database, i.e.
		// whether PrepareAndDispatch is going to need the skinning pools for it. Read on
		// the game thread from the slot it has just finished filling, alongside IsEmpty.
		[[nodiscard]] bool HasAnimatedDraws(std::uint32_t slot) const
		{
			return m_slotHasAnimatedDraws[slot % kFramesInFlight];
		}

		[[nodiscard]] bool AreAnimationBuffersAllocated() const
		{
			return m_animationBuffersReady;
		}

		// Drops the skinning pools. Over a hundred megabytes per queue, so a session that
		// has walked away from its skinned content should not keep paying for it.
		//
		// The buffers are live for as long as any frame referencing them is in flight, so
		// this must only be called with the GPU idle and the render thread parked - it
		// rides the same quiesced rebuild path as the shadow and AO targets. The next
		// skinned draw re-creates them through EnsureAnimationBuffers.
		void ReleaseAnimationBuffers();

	private:
		// Allocates the skinning buffers the first time this queue sees an animated
		// draw. Called from PrepareAndDispatch on the render thread.
		void EnsureAnimationBuffers();

		// Each slot is protected by m_slotMutexes[slot]. The game thread
		std::array<std::vector<DrawCommand>, kFramesInFlight> m_commandSlots;
		std::array<std::mutex, kFramesInFlight> m_slotMutexes;
		std::array<std::condition_variable, kFramesInFlight> m_slotCv;
		std::array<bool, kFramesInFlight> m_slotConsumed{};
		// Set by Submit when a draw carries an animation database, cleared with the slot.
		// Guarded by m_slotMutexes[slot] on the write side, like the command vector it
		// summarises.
		std::array<bool, kFramesInFlight> m_slotHasAnimatedDraws{};
		std::uint32_t m_writeSlot = 0; // set by game thread via SetWriteSlot()

		struct MappedPerFrame
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		std::array<MappedPerFrame, kFramesInFlight> m_instanceData;
		std::array<MappedPerFrame, kFramesInFlight> m_cullInput;
		std::array<MappedPerFrame, kFramesInFlight> m_batchDesc;
		std::array<MappedPerFrame, kFramesInFlight> m_skinCopyJobs;
		std::array<MappedPerFrame, kFramesInFlight> m_animationSampleJobs;

		struct DevicePerFrame
		{
			gpu::BufferHandle handle{};
			gpu::DeviceAddress address = 0;
		};

		std::array<DevicePerFrame, kFramesInFlight> m_outputIndirect;
		std::array<DevicePerFrame, kFramesInFlight> m_sampledPoses;
		std::array<DevicePerFrame, kFramesInFlight> m_nodeGlobalTransforms;
		std::array<DevicePerFrame, kFramesInFlight> m_skinPalette;

		DrawContracts::InstanceData* m_instanceDataMapped = nullptr;
		CullContracts::DrawInput* m_cullInputMapped = nullptr;
		CullContracts::Batch* m_batchDescMapped = nullptr;
		AnimationContracts::SkinCopyJob* m_skinCopyJobsMapped = nullptr;
		AnimationContracts::AnimatorSampleJob* m_animationSampleJobsMapped = nullptr;

		std::uint32_t m_maxDraws = 0;
		std::uint32_t m_outputDrawCapacity = 0;
		std::uint32_t m_maxBatches = 0;
		std::uint32_t m_maxAnimationDraws = 0;
		std::uint32_t m_maxSkinJoints = 0;
		std::uint32_t m_maxSampledPoses = 0;
		bool m_animationBuffersReady = false;
		std::string m_debugName = "RenderQueue";

		struct BatchRenderInfo
		{
			const GraphicsPipeline* pipeline = nullptr;
			const Mesh* mesh = nullptr;
			std::uint32_t meshGeneration = 0;
			std::uint32_t outputStart = 0;
			std::uint32_t drawCount = 0;
		};

		struct PreparedFrame
		{
			std::vector<BatchRenderInfo> batchRenderInfos;
			gpu::DeviceAddress frameAddr = 0;
			gpu::DeviceAddress instanceDataAddr = 0;
			gpu::DeviceAddress skinPaletteAddr = 0;
			gpu::DeviceAddress nodeGlobalTransformsAddr = 0;
			gpu::DeviceAddress drawBase = 0;
			gpu::BufferHandle indirectHandle{};

			void Reset()
			{
				batchRenderInfos.clear();
				frameAddr = 0;
				instanceDataAddr = 0;
				skinPaletteAddr = 0;
				nodeGlobalTransformsAddr = 0;
				drawBase = 0;
				indirectHandle = {};
			}
		};

		std::array<PreparedFrame, kFramesInFlight> m_preparedFrames;

		gpu::DeviceAddress m_multiFrameAddrs[3] = {};
		bool m_debugForceVisible = false;
		bool m_debugBypassIndirect = false;
		bool m_debugDisableAnimation = false;
		std::uint32_t m_debugAnimPassMask = 0xFFFFFFFFu;
		std::uint32_t m_debugLogSkinJobsFramesLeft = 0;

		void FlushDrawImpl(gpu::CommandList& cmd,
		        std::uint32_t frameIndex,
		        gpu::DeviceAddress frameAddr,
		        const DrawContracts::LightingAddresses* lighting,
		        const GraphicsPipeline* overridePipeline,
		        std::uint32_t cascadeOffset,
		        const gpu::CullMode* cullModeOverride,
		        const char* debugLabel,
		        float r,
		        float g,
		        float b);

		const RenderQueueSharedPipelines* m_sharedPipelines = nullptr;

		const AnimationDatabase* m_animationDb = nullptr;
		std::uint32_t m_animationSampleJobCount = 0;
		std::array<bool, kFramesInFlight> m_animationSlotCleared{};

		AnimationBlendSystem* m_animationBlendSystem = nullptr;
	};
} // namespace aether
