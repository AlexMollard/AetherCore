#include "passes/GTAOPass.hpp"

#include <algorithm>
#include <utility>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace
{
	constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
}

namespace aether
{
	void GTAOPass::Create(const Desc& desc)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT_ALWAYS(desc.device != nullptr, "GTAOPass::Create requires a GPU device");
		AE_ASSERT_ALWAYS(desc.bindlessManager != nullptr, "GTAOPass::Create requires a BindlessManager");
		AE_ASSERT_ALWAYS(desc.renderGraph != nullptr, "GTAOPass::Create requires a RenderGraph");

		m_device = desc.device;
		m_extent = desc.extent;
		m_aoExtent = {
		        std::max(1u, (m_extent.width + 1u) / 2u),
		        std::max(1u, (m_extent.height + 1u) / 2u),
		};
		m_bindlessManager = desc.bindlessManager;
		m_renderGraph = desc.renderGraph;

		const gpu::TextureDesc rawDesc{
		        .format = gpu::Format::R8Unorm,
		        .extent = m_aoExtent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "GTAO.Raw",
		};
		m_rawAoHandle = gpu::ResourceRegistry::CreateTexture(rawDesc);
		if (!m_rawAoHandle.IsValid())
		{
			Throw(AetherError::Engine("GTAOPass: Raw AO CreateTexture failed"));
		}

		const gpu::TextureDesc denoisedDesc{
		        .format = gpu::Format::R8Unorm,
		        .extent = m_aoExtent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "GTAO.Denoised",
		};
		m_denoisedAoHandle = gpu::ResourceRegistry::CreateTexture(denoisedDesc);
		if (!m_denoisedAoHandle.IsValid())
		{
			Throw(AetherError::Engine("GTAOPass: Denoised AO CreateTexture failed"));
		}

		m_rawAoImage = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_rawAoHandle), gpu::ResourceRegistry::ResolveTexture(m_rawAoHandle).view);
		m_denoisedAoImage = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_denoisedAoHandle), gpu::ResourceRegistry::ResolveTexture(m_denoisedAoHandle).view);

		const auto rawSlot = desc.bindlessManager->AllocateSampledImageSlot();
		if (!rawSlot)
		{
			Throw(AetherError::Engine("GTAOPass: raw AO AllocateSampledImageSlot failed"));
		}
		m_rawAoBindlessSlot = *rawSlot;
		AE_EXPECT_OR_THROW_VOID(desc.bindlessManager->WriteSampledImage(m_rawAoBindlessSlot, gpu::ResourceRegistry::GetViewCreateInfo(m_rawAoHandle), gpu::ImageLayout::ShaderReadOnly));

		const auto denoisedSlot = desc.bindlessManager->AllocateSampledImageSlot();
		if (!denoisedSlot)
		{
			Throw(AetherError::Engine("GTAOPass: denoised AO AllocateSampledImageSlot failed"));
		}
		m_denoisedAoBindlessSlot = *denoisedSlot;
		AE_EXPECT_OR_THROW_VOID(desc.bindlessManager->WriteSampledImage(m_denoisedAoBindlessSlot, gpu::ResourceRegistry::GetViewCreateInfo(m_denoisedAoHandle), gpu::ImageLayout::ShaderReadOnly));

		AE_EXPECT_OR_THROW(mainPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://gtao_main.spv",
		                        .colorFormat = gpu::Format::R8Unorm,
		                        .debugName = "GTAO.Main",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		m_mainPipeline = std::move(mainPipeline);

		AE_EXPECT_OR_THROW(denoisePipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://gtao_denoise.spv",
		                        .colorFormat = gpu::Format::R8Unorm,
		                        .debugName = "GTAO.Denoise",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		m_denoisePipeline = std::move(denoisePipeline);
	}

	void GTAOPass::Destroy()
	{
		AE_PROFILE_ZONE();
		m_denoisePipeline.Destroy();
		m_mainPipeline.Destroy();
		if (m_bindlessManager != nullptr && m_rawAoBindlessSlot != kInvalidBindlessSlot)
		{
			m_bindlessManager->FreeSampledImageSlot(m_rawAoBindlessSlot);
		}
		if (m_bindlessManager != nullptr && m_denoisedAoBindlessSlot != kInvalidBindlessSlot)
		{
			m_bindlessManager->FreeSampledImageSlot(m_denoisedAoBindlessSlot);
		}
		m_rawAoBindlessSlot = kInvalidBindlessSlot;
		m_denoisedAoBindlessSlot = kInvalidBindlessSlot;

		if (m_rawAoHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_rawAoHandle);
			m_rawAoHandle = {};
		}
		if (m_denoisedAoHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_denoisedAoHandle);
			m_denoisedAoHandle = {};
		}
		m_rawAoImage = RGImage{};
		m_denoisedAoImage = RGImage{};
		m_device = nullptr;
		m_extent = {};
		m_aoExtent = {};
		m_bindlessManager = nullptr;
		m_renderGraph = nullptr;
		m_passesRegistered = false;
	}

	void GTAOPass::RegisterPasses(RenderGraph& graph, RGImage depth)
	{
		AE_PROFILE_ZONE();
		if (m_passesRegistered)
		{
			return;
		}
		if (!depth.IsValid() || !m_rawAoImage.IsValid() || !m_denoisedAoImage.IsValid())
		{
			return;
		}

		graph.AddFullscreenPass({
		                                .name = "$GTAO_Main",
		                                .color = m_rawAoImage,
		                                .extent = m_aoExtent,
		                                .loadOp = gpu::LoadOp::DontCare,
		                                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth)},
		                        })
		        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                gpu::CommandList cmd = ctx.recorder.View();
			                m_bindlessManager->CmdBindHeaps(cmd);
			                cmd.BindPipeline(m_mainPipeline.GetPipeline());

			                struct
			                {
				                std::uint32_t fullWidth;
				                std::uint32_t fullHeight;
				                std::uint32_t frameIndex;
				                std::uint64_t frameConstantsAddr;
				                float radius;
				                float strength;
				                std::uint32_t _pad0;
				                std::uint32_t _pad1;
			                } push{
			                        .fullWidth = m_extent.width,
			                        .fullHeight = m_extent.height,
			                        .frameIndex = static_cast<std::uint32_t>(ctx.frame.frameIndex),
			                        .frameConstantsAddr = ctx.frameConstantsAddr,
			                        .radius = 1.5f,
			                        .strength = 1.25f,
			                };

			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(3, 1, 0, 0);
		                });

		graph.AddFullscreenPass({
		                                .name = "$GTAO_Denoise",
		                                .color = m_denoisedAoImage,
		                                .extent = m_aoExtent,
		                                .loadOp = gpu::LoadOp::DontCare,
		                                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth)},
		                                .produces = {RenderGraph::Product<FrameTextureProduct>(kFrameProductGtao)},
		                        })
		        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
		        .ReadTexture(m_rawAoImage)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                gpu::CommandList cmd = ctx.recorder.View();
			                m_bindlessManager->CmdBindHeaps(cmd);
			                cmd.BindPipeline(m_denoisePipeline.GetPipeline());

			                struct
			                {
				                std::uint32_t srcAoSlot;
				                std::uint64_t frameConstantsAddr;
				                std::uint32_t fullWidth;
				                std::uint32_t fullHeight;
				                std::uint32_t aoWidth;
				                std::uint32_t aoHeight;
				                float edgeThreshold;
				                float spatialSigma;
			                } push{
			                        .srcAoSlot = m_rawAoBindlessSlot,
			                        .frameConstantsAddr = ctx.frameConstantsAddr,
			                        .fullWidth = m_extent.width,
			                        .fullHeight = m_extent.height,
			                        .aoWidth = m_aoExtent.width,
			                        .aoHeight = m_aoExtent.height,
			                        .edgeThreshold = 0.0025f,
			                        .spatialSigma = 2.0f,
			                };

			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(3, 1, 0, 0);
		                });

		m_passesRegistered = true;
	}
} // namespace aether
