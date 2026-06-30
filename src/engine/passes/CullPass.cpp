#include "passes/CullPass.hpp"

#include "gpu/ResourceRegistry.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void CullPass::Initialize(gpu::Device device)
	{
		AE_PROFILE_ZONE();
		m_device = device;
	}

	void CullPass::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_singleHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_singleHandle);
			m_singleHandle = {};
		}
		if (m_multiHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_multiHandle);
			m_multiHandle = {};
		}
		m_device = nullptr;
	}

	Expected<void> CullPass::EnsureSinglePipeline()
	{
		AE_PROFILE_ZONE();
		if (m_singleHandle.IsValid())
		{
			return {};
		}

		const gpu::ComputePipelineDesc desc{
		        .shaderVfsPath = "shaders://cull_draws.spv",
		        .shaderEntry = "main",
		        .debugName = "CullPass.cullDraws",
		};
		m_singleHandle = gpu::ResourceRegistry::CreateComputePipeline(m_device, desc);
		if (!m_singleHandle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to register single compute pipeline."));
		}
		return {};
	}

	Expected<void> CullPass::EnsureMultiPipeline()
	{
		AE_PROFILE_ZONE();
		if (m_multiHandle.IsValid())
		{
			return {};
		}

		const gpu::ComputePipelineDesc desc{
		        .shaderVfsPath = "shaders://cull_draws_multi.spv",
		        .shaderEntry = "main",
		        .debugName = "CullPass.cullDrawsMulti",
		};
		m_multiHandle = gpu::ResourceRegistry::CreateComputePipeline(m_device, desc);
		if (!m_multiHandle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to register multi compute pipeline."));
		}
		return {};
	}

	void CullPass::RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix, PreparedDrawList drawList)
	{
		AE_PROFILE_ZONE();
		AE_EXPECT_OR_THROW_VOID(EnsureSinglePipeline());
		AE_EXPECT_OR_THROW_VOID(EnsureMultiPipeline());

		const std::string passName = namePrefix.empty() ? "$CullDraws" : ("$CullDraws_" + namePrefix);

		auto pass = graph.AddQueuePreparePass({
		        .name = passName,
		        .produces = drawList,
		        .sideEffectReason = "produces RenderQueue prepared draw state",
		});
		pass.ExecuteCompute([&renderQueue, this](PassContext& ctx) { renderQueue.PrepareAndDispatch(ctx.recorder, ctx.frameConstantsAddr, GetSinglePipeline(), ctx.frameSlot); })
		        .OnDebugDisabled([&renderQueue](PassContext& ctx) { renderQueue.DiscardPending(ctx.frameSlot); });
	}

	gpu::Pipeline CullPass::GetSinglePipeline() const
	{
		return const_cast<void*>(gpu::ResourceRegistry::ResolvePipeline(m_singleHandle).state);
	}

	gpu::Pipeline CullPass::GetMultiPipeline() const
	{
		return const_cast<void*>(gpu::ResourceRegistry::ResolvePipeline(m_multiHandle).state);
	}
} // namespace aether
