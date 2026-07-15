#pragma once

#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{
	// World-space 2D render seam. Phase 0 owns packet lifetime and graph
	// composition; sprite GPU upload and drawing are layered onto this class.
	class Renderer2D
	{
	public:
		void BeginFrame(const Render2DFrameData& frameData)
		{
			m_frameData = &frameData;
		}

		void EndFrame()
		{
			m_frameData = nullptr;
		}

		void RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent)
		{
			graph.AddFullscreenPass({
			             .name = "$Renderer2D",
			             .color = color,
			             .extent = extent,
			             .loadOp = gpu::LoadOp::Load,
			     })
			        .Execute([this](PassContext&) { (void) m_frameData; });
		}

		[[nodiscard]] const Render2DFrameData* GetFrameData() const
		{
			return m_frameData;
		}

	private:
		const Render2DFrameData* m_frameData = nullptr;
	};
} // namespace aether
